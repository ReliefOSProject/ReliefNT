#include "device.h"
#include <reliefnt/heap.h>

/* Packed as high 32-bit live-batch count and low 32-bit reserved bytes. This
 * static accounting survives device reconfiguration while caller-owned
 * sidecars remain live. */
static uint64_t batch_metadata_usage;

/* Keep completion evidence across FIFO generations. If bounded history is
 * exhausted, old jobs remain reset-pending rather than being released on an
 * unproven generation change. */
#define SVGA_FENCE_QUIESCENCE_HISTORY 64u
struct svga_fence_quiescence_record {
    uint32_t generation, passed_seq;
};
static struct svga_fence_quiescence_record
    fence_quiescence_history[SVGA_FENCE_QUIESCENCE_HISTORY];
static uint32_t fence_quiescence_cursor;

void svga_fence_quiescence_reset(void)
{
    __builtin_memset(fence_quiescence_history, 0,
                     sizeof(fence_quiescence_history));
    fence_quiescence_cursor = 0;
}

static void fence_quiescence_record_locked(uint32_t generation,
                                           uint32_t passed_seq)
{
    if (!generation || !passed_seq) return;
    for (uint32_t i = 0; i < SVGA_FENCE_QUIESCENCE_HISTORY; ++i) {
        if (fence_quiescence_history[i].generation != generation) continue;
        if ((int32_t)(passed_seq -
                      fence_quiescence_history[i].passed_seq) >= 0)
            fence_quiescence_history[i].passed_seq = passed_seq;
        return;
    }
    uint32_t slot = fence_quiescence_cursor++ %
                    SVGA_FENCE_QUIESCENCE_HISTORY;
    fence_quiescence_history[slot] = (struct svga_fence_quiescence_record){
        .generation = generation, .passed_seq = passed_seq,
    };
}

/** @brief Reserve global sidecar bytes and one live batch atomically.
 * @param bytes Exact bytes for the metadata object and both operation arrays.
 * @return True when the global byte and live-batch limits admit the request.
 */
static bool batch_metadata_reserve(uint32_t bytes)
{
    uint64_t old = __atomic_load_n(&batch_metadata_usage, __ATOMIC_ACQUIRE);
    for (;;) {
        uint32_t live = (uint32_t)(old >> 32);
        uint32_t used = (uint32_t)old;
        if (live >= SVGA_BATCH_METADATA_MAX_LIVE ||
            used > SVGA_BATCH_METADATA_MAX_BYTES ||
            bytes > SVGA_BATCH_METADATA_MAX_BYTES - used)
            return false;
        uint64_t next = ((uint64_t)(live + 1u) << 32) | (used + bytes);
        if (__atomic_compare_exchange_n(&batch_metadata_usage, &old, next,
                false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
}

/** @brief Release one global sidecar reservation after its heap is freed.
 * @param bytes Exact bytes previously reserved by a successful CAS.
 */
static void batch_metadata_release(uint32_t bytes)
{
    uint64_t old = __atomic_load_n(&batch_metadata_usage, __ATOMIC_ACQUIRE);
    for (;;) {
        uint32_t live = (uint32_t)(old >> 32);
        uint32_t used = (uint32_t)old;
        if (!live || used < bytes) return;
        uint64_t next = ((uint64_t)(live - 1u) << 32) | (used - bytes);
        if (__atomic_compare_exchange_n(&batch_metadata_usage, &old, next,
                false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return;
    }
}

static uint32_t read_word(const void *data, uint32_t offset)
{
    const uint8_t *p = (const uint8_t *)data + offset;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** @brief Validate FIFO ring geometry and optionally return the first bad check.
 * @param next Receives NEXT_CMD after the storage preconditions hold.
 * @param stop Receives STOP after the storage preconditions hold.
 * @param detail0 Optional numeric first-failure check code.
 * @param detail1 Optional observed value for that check.
 * @param detail2 Optional expected bound or paired value for that check.
 * @return Zero for a valid ring, SVGA_ENODEV for unavailable storage, or
 *         SVGA_EIO for corrupt ring registers.
 */
static int ring_state(uint32_t *next, uint32_t *stop, uint64_t *detail0,
                      uint64_t *detail1, uint64_t *detail2)
{
    if (!svga.fifo_ready) {
        if (detail0) *detail0 = SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_NOT_READY;
        if (detail1) *detail1 = 0;
        if (detail2) *detail2 = 1;
        return SVGA_ENODEV;
    }
    if (!svga.fifo) {
        if (detail0) *detail0 = SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_NULL_STORAGE;
        if (detail1) *detail1 = 0;
        if (detail2) *detail2 = 1;
        return SVGA_ENODEV;
    }
    if (svga.min < 16) {
        if (detail0) *detail0 = SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_CONFIG_MIN;
        if (detail1) *detail1 = svga.min;
        if (detail2) *detail2 = 16;
        return SVGA_ENODEV;
    }
    if (svga.fifo_bytes <= svga.min + 4) {
        if (detail0) *detail0 = SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_STORAGE_SIZE;
        if (detail1) *detail1 = svga.fifo_bytes;
        if (detail2) *detail2 = svga.min + 4;
        return SVGA_ENODEV;
    }
    *next = svga.fifo[SVGA_FIFO_NEXT_CMD];
    *stop = svga.fifo[SVGA_FIFO_STOP];
    if (svga.fifo[SVGA_FIFO_MIN] != svga.min) {
        if (detail0) *detail0 = SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_MIN_REGISTER;
        if (detail1) *detail1 = svga.fifo[SVGA_FIFO_MIN];
        if (detail2) *detail2 = svga.min;
        return SVGA_EIO;
    }
    if (svga.fifo[SVGA_FIFO_MAX] != svga.fifo_bytes) {
        if (detail0) *detail0 = SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_MAX_REGISTER;
        if (detail1) *detail1 = svga.fifo[SVGA_FIFO_MAX];
        if (detail2) *detail2 = svga.fifo_bytes;
        return SVGA_EIO;
    }
    if ((*next | *stop) & 3) {
        if (detail0) *detail0 = SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_ALIGNMENT;
        if (detail1) *detail1 = *next;
        if (detail2) *detail2 = *stop;
        return SVGA_EIO;
    }
    if (*next < svga.min) {
        if (detail0) *detail0 = SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_NEXT_LOWER_BOUND;
        if (detail1) *detail1 = *next;
        if (detail2) *detail2 = svga.min;
        return SVGA_EIO;
    }
    if (*stop < svga.min) {
        if (detail0) *detail0 = SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_STOP_LOWER_BOUND;
        if (detail1) *detail1 = *stop;
        if (detail2) *detail2 = svga.min;
        return SVGA_EIO;
    }
    if (*next >= svga.fifo_bytes) {
        if (detail0) *detail0 = SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_NEXT_UPPER_BOUND;
        if (detail1) *detail1 = *next;
        if (detail2) *detail2 = svga.fifo_bytes;
        return SVGA_EIO;
    }
    if (*stop >= svga.fifo_bytes) {
        if (detail0) *detail0 = SVGA_BATCH_SUBMIT_DETAIL_FIFO_RING_STOP_UPPER_BOUND;
        if (detail1) *detail1 = *stop;
        if (detail2) *detail2 = svga.fifo_bytes;
        return SVGA_EIO;
    }
    return 0;
}

static void put_word(uint32_t *next, uint32_t word, bool reserve)
{
    svga.fifo[*next / 4] = word;
    *next += 4;
    if (*next == svga.fifo_bytes) *next = svga.min;
    if (!reserve) {
        /* Legacy hosts require a NEXT_CMD publication for each DWORD. */
        svga_barrier();
        svga.fifo[SVGA_FIFO_NEXT_CMD] = *next;
    }
}

static void fifo_wake_locked(void)
{
    svga_barrier();
    svga.ops.write(SVGA_REG_SYNC, 1);
}

void svga_fifo_defer_notify_locked(void)
{
    svga.fifo_notify_deferred = true;
}

void svga_fifo_flush_notify_locked(void)
{
    svga.fifo_notify_deferred = false;
    fifo_wake_locked();
}

/** @brief Publish one packet, optionally returning immediately on backpressure. */
static int fifo_packet_locked(uint32_t id, bool three_d,
                              const struct svga_span *spans, uint32_t count,
                              bool wait_for_space)
{
    uint32_t bytes = three_d ? 8 : 4, next = 0, stop = 0;
    if (count && !spans) return SVGA_EINVAL;
    for (uint32_t i = 0; i < count; ++i) {
        if ((spans[i].bytes & 3) || (spans[i].bytes && !spans[i].data) ||
            spans[i].bytes > UINT32_MAX - bytes) return SVGA_EINVAL;
        bytes += spans[i].bytes;
    }
    int ret = ring_state(&next, &stop, NULL, NULL, NULL);
    if (ret) return ret;
    if (bytes >= svga.fifo_bytes - svga.min) return SVGA_EINVAL;
    for (uint32_t poll = 0;; ++poll) {
        ret = ring_state(&next, &stop, NULL, NULL, NULL);
        if (ret) return ret;
        uint32_t free_bytes = next >= stop ? svga.fifo_bytes - next + stop - svga.min : stop - next;
        if (bytes < free_bytes) break;
        if (!wait_for_space) {
            fifo_wake_locked();
            (void)svga.ops.read(SVGA_REG_BUSY);
            return SVGA_EBUSY;
        }
        if (poll == SVGA_POLL_LIMIT) return SVGA_ETIMEDOUT;
        if (!poll) fifo_wake_locked();
        (void)svga.ops.read(SVGA_REG_BUSY);
        svga_relax();
    }
    bool reserve = (svga.fifo_caps & SVGA_FIFO_CAP_RESERVE) != 0;
    if (reserve) { svga.fifo[SVGA_FIFO_RESERVED] = bytes; svga_barrier(); }
    put_word(&next, id, reserve);
    if (three_d) put_word(&next, bytes - 8, reserve);
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = 0; j < spans[i].bytes; j += 4)
            put_word(&next, read_word(spans[i].data, j), reserve);
    svga_barrier();
    svga.fifo[SVGA_FIFO_NEXT_CMD] = next;
    svga_barrier();
    if (reserve) svga.fifo[SVGA_FIFO_RESERVED] = 0;
    if (!svga.fifo_notify_deferred) fifo_wake_locked();
    return 0;
}

int svga_fifo_packet_locked(uint32_t id, bool three_d,
                            const struct svga_span *spans, uint32_t count)
{
    return fifo_packet_locked(id, three_d, spans, count, true);
}

int svga_fifo_packet_try_locked(uint32_t id, bool three_d,
                                const struct svga_span *spans, uint32_t count)
{
    return fifo_packet_locked(id, three_d, spans, count, false);
}

/** @brief Write one batch DWORD and wrap at FIFO_MAX back to FIFO_MIN.
 * Caller holds svga_lock and reserved enough ring space for the full batch.
 * @param next Mutable FIFO cursor in bytes.
 * @param value DWORD to place in the FIFO.
 */
static void batch_write_word(uint32_t *next, uint32_t value)
{
    svga.fifo[*next / 4] = value;
    *next += 4;
    if (*next == svga.fifo_bytes) *next = svga.min;
}

/** @brief Publish a batch body and fence, recording final-submit failures.
 * Caller holds svga_lock.
 * @param words DWORD-aligned command body.
 * @param bytes Body length, bounded by SVGA_BATCH_MAX_OUTPUT_BYTES.
 * @param fence Writable output for the generation-tagged issued sequence.
 * @param diagnostic_batch Optional caller-owned batch that receives numeric
 *        failure details; NULL keeps the generic FIFO helper silent.
 * @return Zero on publication, SVGA_EBUSY for transient FIFO/fence pressure,
 *         SVGA_ENOMEM when body, fence, and guard cannot statically fit, or a
 *         negative SVGA error for invalid/unavailable FIFO state.
 */
static int fifo_batch_try_locked(const uint32_t *words, uint32_t bytes,
                                 svga_handle *fence,
                                 struct svga_batch *diagnostic_batch)
{
    if (!words && bytes) {
        svga_batch_submit_failure_set(diagnostic_batch,
            SVGA_BATCH_SUBMIT_REASON_FIFO_ARGUMENT,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_FIFO_ARGUMENT_WORDS, bytes, 0);
        return SVGA_EINVAL;
    }
    if (bytes & 3u) {
        svga_batch_submit_failure_set(diagnostic_batch,
            SVGA_BATCH_SUBMIT_REASON_FIFO_ARGUMENT,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_FIFO_ARGUMENT_ALIGNMENT, bytes, 4);
        return SVGA_EINVAL;
    }
    if (!fence) {
        svga_batch_submit_failure_set(diagnostic_batch,
            SVGA_BATCH_SUBMIT_REASON_FIFO_ARGUMENT,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_FIFO_ARGUMENT_FENCE, 0, 1);
        return SVGA_EINVAL;
    }
    if (bytes > SVGA_BATCH_MAX_OUTPUT_BYTES) {
        svga_batch_submit_failure_set(diagnostic_batch,
            SVGA_BATCH_SUBMIT_REASON_FIFO_ARGUMENT,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_FIFO_ARGUMENT_SIZE, bytes,
            SVGA_BATCH_MAX_OUTPUT_BYTES);
        return SVGA_EINVAL;
    }
    if (!(svga.fifo_caps & SVGA_FIFO_CAP_FENCE)) {
        svga_batch_submit_failure_set(diagnostic_batch,
            SVGA_BATCH_SUBMIT_REASON_FIFO_ARGUMENT,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_FIFO_FENCE_UNSUPPORTED,
            svga.fifo_caps, SVGA_FIFO_CAP_FENCE);
        return SVGA_ENOTSUP;
    }
    uint32_t next, stop;
    uint64_t ring_detail0 = SVGA_BATCH_SUBMIT_DETAIL_NONE;
    uint64_t ring_detail1 = 0, ring_detail2 = 0;
    int ret = ring_state(&next, &stop, &ring_detail0,
                         &ring_detail1, &ring_detail2);
    if (ret) {
        svga_batch_submit_failure_set(diagnostic_batch,
            SVGA_BATCH_SUBMIT_REASON_FIFO_RING,
            SVGA_BATCH_SUBMIT_INDEX_NONE, ring_detail0,
            ring_detail1, ring_detail2);
        return ret;
    }

    if (bytes > UINT32_MAX - 8u) {
        svga_batch_submit_failure_set(diagnostic_batch,
            SVGA_BATCH_SUBMIT_REASON_FIFO_STATIC_FIT,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_FIFO_SIZE_OVERFLOW, bytes,
            UINT32_MAX - 8u);
        return SVGA_ENOMEM;
    }
    const uint32_t total = bytes + 8u;
    /* The ring's one-DWORD empty/full guard is accounted by the strict
     * comparison: body + fence may leave at least four bytes unused. */
    if (total >= svga.fifo_bytes - svga.min) {
        svga_batch_submit_failure_set(diagnostic_batch,
            SVGA_BATCH_SUBMIT_REASON_FIFO_STATIC_FIT,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_FIFO_STATIC_FIT, total,
            svga.fifo_bytes - svga.min);
        return SVGA_ENOMEM;
    }
    uint32_t seq = svga.next_fence;
    if (!seq) seq = 1;
    uint32_t pending = (uint32_t)(seq - svga.fifo[SVGA_FIFO_FENCE]);
    if (pending >= 0x80000000u) {
        svga_batch_submit_failure_set(diagnostic_batch,
            SVGA_BATCH_SUBMIT_REASON_FIFO_TRANSIENT,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_FIFO_FENCE_WINDOW, pending,
            0x80000000u);
        return SVGA_EBUSY;
    }
    uint32_t free_bytes = next >= stop ? svga.fifo_bytes - next + stop - svga.min : stop - next;
    if (total >= free_bytes) {
        fifo_wake_locked();
        svga_batch_submit_failure_set(diagnostic_batch,
            SVGA_BATCH_SUBMIT_REASON_FIFO_TRANSIENT,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_FIFO_FREE_BYTES, total, free_bytes);
        return SVGA_EBUSY;
    }

    bool reserve = (svga.fifo_caps & SVGA_FIFO_CAP_RESERVE) != 0;
    if (reserve) {
        svga.fifo[SVGA_FIFO_RESERVED] = total;
        svga_barrier();
    }
    for (uint32_t i = 0; i < bytes / 4; ++i) batch_write_word(&next, words[i]);
    batch_write_word(&next, SVGA_CMD_FENCE);
    batch_write_word(&next, seq);
    svga_barrier();
    svga.fifo[SVGA_FIFO_NEXT_CMD] = next;
    svga_barrier();
    if (reserve) svga.fifo[SVGA_FIFO_RESERVED] = 0;
    svga.issued_fence = seq;
    svga.next_fence = seq + 1u;
    *fence = ((uint64_t)svga.generation << 32) | seq;
    svga.fifo_notify_deferred = false;
    fifo_wake_locked();
    return 0;
}

/** @brief Queue a batch with no-wait semantics, leaving submit diagnostics absent.
 * Caller holds svga_lock.
 * @param words DWORD-aligned command body.
 * @param bytes Body length, bounded by SVGA_BATCH_MAX_OUTPUT_BYTES.
 * @param fence Writable output for the generation-tagged issued sequence.
 * @return Zero on publication or the unchanged negative SVGA error.
 */
int svga_fifo_batch_try_locked(const uint32_t *words, uint32_t bytes,
                               svga_handle *fence)
{
    return fifo_batch_try_locked(words, bytes, fence, NULL);
}

/** @brief Free heap-backed operation arrays and their enclosing sidecar.
 * @param metadata Sidecar to release; NULL is accepted.
 */
static void batch_metadata_free(struct svga_batch_metadata *metadata)
{
    if (!metadata) return;
    uint32_t allocation_bytes = metadata->allocation_bytes;
    kernel_free(metadata->shader_ops);
    kernel_free(metadata->query_ops);
    kernel_free(metadata->binding_ops);
    kernel_free(metadata);
    batch_metadata_release(allocation_bytes);
}

/**
 * @brief Initialize bounded batch storage and source-sized heap metadata.
 * @param batch Writable caller-owned batch state.
 * @param generation Device generation captured before preparation.
 * @param storage Caller-owned writable DWORD storage.
 * @param output_capacity Storage capacity in bytes, at most 256 KiB.
 * @param source_bytes Accepted source size in bytes, at most 64 KiB.
 * @return Zero on success, SVGA_EINVAL for invalid bounds, or SVGA_ENOMEM for
 *         sidecar/operation-array allocation failure.
 */
int svga_batch_init_sized(struct svga_batch *batch, uint32_t generation,
                          uint32_t *storage, uint32_t output_capacity,
                          uint32_t source_bytes)
{
    if (!batch || !storage || ((uintptr_t)storage & 3u) ||
        !output_capacity || output_capacity > SVGA_BATCH_MAX_OUTPUT_BYTES ||
        (output_capacity & 3u) || !source_bytes ||
        source_bytes > SVGA_BATCH_MAX_INPUT_BYTES)
        return SVGA_EINVAL;
    uint32_t operation_capacity = source_bytes / 8u;
    size_t query_bytes = (size_t)operation_capacity *
                         sizeof(struct svga_batch_query_op);
    size_t shader_bytes = (size_t)operation_capacity *
                          sizeof(struct svga_batch_shader_op);
    size_t binding_bytes = (size_t)operation_capacity *
                           sizeof(struct svga_batch_binding_op);
    if ((operation_capacity &&
         (query_bytes / sizeof(struct svga_batch_query_op) != operation_capacity ||
          shader_bytes / sizeof(struct svga_batch_shader_op) != operation_capacity ||
          binding_bytes / sizeof(struct svga_batch_binding_op) != operation_capacity)) ||
        query_bytes > SIZE_MAX - sizeof(struct svga_batch_metadata) ||
        shader_bytes > SIZE_MAX - sizeof(struct svga_batch_metadata) - query_bytes ||
        binding_bytes > SIZE_MAX - sizeof(struct svga_batch_metadata) - query_bytes -
                        shader_bytes)
        return SVGA_ENOMEM;
    size_t total_bytes = sizeof(struct svga_batch_metadata) + query_bytes +
                         shader_bytes + binding_bytes;
    if (total_bytes > UINT32_MAX || total_bytes > SVGA_BATCH_METADATA_MAX_BYTES)
        return SVGA_ENOMEM;
    uint32_t allocation_bytes = (uint32_t)total_bytes;
    if (!batch_metadata_reserve(allocation_bytes)) return SVGA_ENOMEM;

    struct svga_batch_metadata *metadata = kernel_malloc(sizeof(*metadata));
    if (!metadata) {
        batch_metadata_release(allocation_bytes);
        return SVGA_ENOMEM;
    }
    __builtin_memset(metadata, 0, sizeof(*metadata));
    metadata->operation_capacity = operation_capacity;
    metadata->allocation_bytes = allocation_bytes;
    if (operation_capacity) {
        metadata->query_ops = kernel_malloc(query_bytes);
        metadata->shader_ops = kernel_malloc(shader_bytes);
        metadata->binding_ops = kernel_malloc(binding_bytes);
        if (!metadata->query_ops || !metadata->shader_ops || !metadata->binding_ops) {
            batch_metadata_free(metadata);
            return SVGA_ENOMEM;
        }
        __builtin_memset(metadata->query_ops, 0, query_bytes);
        __builtin_memset(metadata->shader_ops, 0, shader_bytes);
        __builtin_memset(metadata->binding_ops, 0, binding_bytes);
    }
    *batch = (struct svga_batch){.generation = generation,
        .capacity = output_capacity, .source_bytes = source_bytes,
        .words = storage, .metadata = metadata,
        .submit_failure = {.reason = SVGA_BATCH_SUBMIT_REASON_NONE,
            .index = SVGA_BATCH_SUBMIT_INDEX_NONE}};
    return 0;
}

/** @brief Reserve one context for all staged state mutations in this batch.
 * Caller holds svga_lock. This ownership also prevents immediate shader or
 * resource lifetime operations from racing a not-yet-published batch.
 */
int svga_batch_reserve_context_locked(struct svga_batch *batch, uint32_t cid)
{
    if (!batch || !batch->metadata || cid >= SVGA_CONTEXTS ||
        !svga.contexts[cid].handle) return SVGA_EINVAL;
    struct svga_batch_metadata *metadata = batch->metadata;
    struct svga_context *context = &svga.contexts[cid];
    uint32_t old_context_count = batch->context_count;
    int ret = svga_batch_retain_context(batch, context->handle);
    if (ret) return ret;
    if (!metadata->transaction_id) {
        ++svga.transaction_serial;
        if (!svga.transaction_serial) ++svga.transaction_serial;
        metadata->transaction_id = svga.transaction_serial;
        metadata->allocator = svga.ops;
    }
    if (context->batch_state_owner &&
        context->batch_state_owner != metadata->transaction_id) {
        svga_batch_rollback_references(batch, old_context_count,
            batch->surface_count, batch->gmr_count);
        return SVGA_EBUSY;
    }
    if (!metadata->state_context_seen[cid] &&
        metadata->state_context_count >= SVGA_CONTEXTS) {
        svga_batch_rollback_references(batch, old_context_count,
            batch->surface_count, batch->gmr_count);
        return SVGA_ENOMEM;
    }
    context->batch_state_owner = metadata->transaction_id;
    if (!metadata->state_context_seen[cid]) {
        metadata->state_context_ids[metadata->state_context_count++] = cid;
        metadata->state_context_seen[cid] = true;
    }
    return 0;
}

/**
 * @brief Initialize a source-compatible batch with one shared byte capacity.
 * @param batch Writable caller-owned batch state.
 * @param generation Device generation captured before preparation.
 * @param storage Caller-owned writable DWORD storage.
 * @param capacity Input and output capacity in bytes, at most 64 KiB.
 * @return Zero on success or a negative SVGA error on bounds/allocation failure.
 */
int svga_batch_init(struct svga_batch *batch, uint32_t generation,
                    uint32_t *storage, uint32_t capacity)
{
    if (capacity > SVGA_BATCH_MAX_INPUT_BYTES) return SVGA_EINVAL;
    return svga_batch_init_sized(batch, generation, storage, capacity, capacity);
}

/**
 * @brief Copy one trusted packet into a batch without publishing FIFO work.
 * @param batch Open batch owning the destination storage.
 * @param command FIFO command identifier.
 * @param three_d True when the packet uses the 8-byte SVGA3D header.
 * @param payload Trusted readable DWORD-aligned command payload.
 * @param bytes Payload length in bytes.
 * @return Zero on append, SVGA_ENOMEM for output exhaustion, or SVGA_EINVAL
 *         for malformed batch/payload state.
 */
int svga_batch_append(struct svga_batch *batch, uint32_t command, bool three_d,
                      const void *payload, uint32_t bytes)
{
    if (!batch || !batch->words || batch->submitted || (bytes & 3u) ||
        (bytes && !payload) || !batch->metadata ||
        batch->capacity > SVGA_BATCH_MAX_OUTPUT_BYTES ||
        batch->bytes > batch->capacity)
        return SVGA_EINVAL;
    uint32_t header_bytes = three_d ? 8u : 4u;
    if (bytes > UINT32_MAX - header_bytes) return SVGA_ENOMEM;
    uint32_t packet_bytes = bytes + header_bytes;
    if (packet_bytes > batch->capacity - batch->bytes)
        return SVGA_ENOMEM;
    uint32_t *words = batch->words + batch->bytes / 4;
    words[0] = command;
    uint32_t index = 1;
    if (three_d) words[index++] = bytes;
    for (uint32_t i = 0; i < bytes / 4; ++i) {
        uint32_t value;
        __builtin_memcpy(&value, (const uint8_t *)payload + i * 4, sizeof(value));
        words[index + i] = value;
    }
    batch->bytes += packet_bytes;
    return 0;
}

/** @brief Validate all batch state, publish its FIFO body, then commit overlays.
 * Caller holds svga_lock; all fallible validation precedes FIFO publication.
 * @param batch Open batch to submit.
 * @param fence Writable output for the actual trailing fence token.
 * @return Zero after commit, SVGA_EBUSY for retryable FIFO pressure, or a
 *         negative SVGA error with no FIFO publication or fence advancement.
 */
int svga_batch_submit_locked(struct svga_batch *batch, svga_handle *fence)
{
    if (!batch) return SVGA_EINVAL;
    svga_batch_submit_failure_reset(batch);
    if (!fence) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_BATCH_SHAPE,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_FENCE_NULL, 0, 1);
        return SVGA_EINVAL;
    }
    if (batch->submitted) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_BATCH_SHAPE,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_BATCH_SUBMITTED, 1, 0);
        return SVGA_EINVAL;
    }
    if (!batch->bytes) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_BATCH_SHAPE,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_BATCH_EMPTY, 0, 1);
        return SVGA_EINVAL;
    }
    if (!batch->metadata) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_BATCH_SHAPE,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_BATCH_METADATA, 0, 1);
        return SVGA_EINVAL;
    }
    if (batch->context_count > SVGA_BATCH_MAX_CONTEXT_REFS) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_BATCH_SHAPE,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_CONTEXT_COUNT, batch->context_count,
            SVGA_BATCH_MAX_CONTEXT_REFS);
        return SVGA_EINVAL;
    }
    if (batch->surface_count > SVGA_BATCH_MAX_SURFACE_REFS) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_BATCH_SHAPE,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_SURFACE_COUNT, batch->surface_count,
            SVGA_BATCH_MAX_SURFACE_REFS);
        return SVGA_EINVAL;
    }
    if (batch->gmr_count > SVGA_BATCH_MAX_GMR_REFS) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_BATCH_SHAPE,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_GMR_COUNT, batch->gmr_count,
            SVGA_BATCH_MAX_GMR_REFS);
        return SVGA_EINVAL;
    }
    if (batch->shader_op_count > batch->metadata->operation_capacity) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_BATCH_SHAPE,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_SHADER_OP_COUNT, batch->shader_op_count,
            batch->metadata->operation_capacity);
        return SVGA_EINVAL;
    }
    if (batch->binding_op_count > batch->metadata->operation_capacity) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_BATCH_SHAPE,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_BINDING_OP_COUNT, batch->binding_op_count,
            batch->metadata->operation_capacity);
        return SVGA_EINVAL;
    }
    if (batch->query_op_count > batch->metadata->operation_capacity) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_BATCH_SHAPE,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_QUERY_OP_COUNT, batch->query_op_count,
            batch->metadata->operation_capacity);
        return SVGA_EINVAL;
    }
    if (batch->operation_count > batch->metadata->operation_capacity) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_BATCH_SHAPE,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_OPERATION_COUNT, batch->operation_count,
            batch->metadata->operation_capacity);
        return SVGA_EINVAL;
    }
    if (batch->generation != svga.generation) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_GENERATION,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_GENERATION, batch->generation,
            svga.generation);
        return SVGA_EIO;
    }
    struct svga_batch_metadata *metadata = batch->metadata;
    for (uint32_t i = 0; i < batch->context_count; ++i) {
        int id = svga_find_context(metadata->contexts[i]);
        if (id < 0) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_RESOURCE, i,
                SVGA_BATCH_SUBMIT_DETAIL_CONTEXT_MISSING,
                metadata->contexts[i], 0);
            return SVGA_EINVAL;
        }
        if (svga.contexts[id].gb.retiring) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_RESOURCE, i,
                SVGA_BATCH_SUBMIT_DETAIL_CONTEXT_RETIRING,
                metadata->contexts[i], 1);
            return SVGA_EINVAL;
        }
    }
    for (uint32_t i = 0; i < batch->surface_count; ++i) {
        int id = svga_find_surface(metadata->surfaces[i]);
        if (id < 0) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_RESOURCE, i,
                SVGA_BATCH_SUBMIT_DETAIL_SURFACE_MISSING,
                metadata->surfaces[i], 0);
            return SVGA_EINVAL;
        }
        if (svga.surfaces[id].retiring) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_RESOURCE, i,
                SVGA_BATCH_SUBMIT_DETAIL_SURFACE_RETIRING,
                metadata->surfaces[i], 1);
            return SVGA_EINVAL;
        }
    }
    for (uint32_t i = 0; i < batch->gmr_count; ++i) {
        int id = svga_find_gmr(metadata->gmrs[i]);
        if (id < 0) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_RESOURCE, i,
                SVGA_BATCH_SUBMIT_DETAIL_GMR_MISSING,
                metadata->gmrs[i], 0);
            return SVGA_EINVAL;
        }
        if (svga.gmrs[id].retiring) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_RESOURCE, i,
                SVGA_BATCH_SUBMIT_DETAIL_GMR_RETIRING,
                metadata->gmrs[i], 1);
            return SVGA_EINVAL;
        }
        if (svga.gmrs[id].borrowed && svga.gmrs[id].map_inflight) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_RESOURCE, i,
                SVGA_BATCH_SUBMIT_DETAIL_GMR_MAP_INFLIGHT, 1, 0);
            return SVGA_EBUSY;
        }
        if (svga.gmrs[id].borrowed && !svga.gmrs[id].mob_defined) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_RESOURCE, i,
                SVGA_BATCH_SUBMIT_DETAIL_GMR_MOB_UNDEFINED, 0, 1);
            return SVGA_EBUSY;
        }
        if (svga.gmrs[id].borrowed &&
            svga.gmrs[id].mapped_pages != svga.gmrs[id].pages) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_RESOURCE, i,
                SVGA_BATCH_SUBMIT_DETAIL_GMR_PAGES_INCOMPLETE,
                svga.gmrs[id].mapped_pages, svga.gmrs[id].pages);
            return SVGA_EBUSY;
        }
    }
    int binding_ret = svga_batch_state_validate_locked(batch);
    if (binding_ret) return binding_ret;
    uint32_t shader_define_count = 0;
    for (uint32_t i = 0; i < batch->shader_op_count; ++i) {
        const struct svga_batch_shader_op *op = &metadata->shader_ops[i];
        int cid = svga_find_context(op->context);
        if (cid < 0) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                SVGA_BATCH_SUBMIT_DETAIL_SHADER_CONTEXT_MISSING,
                op->context, 0);
            return SVGA_EINVAL;
        }
        if (!metadata->transaction_id) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                SVGA_BATCH_SUBMIT_DETAIL_SHADER_TRANSACTION,
                metadata->transaction_id, 1);
            return SVGA_EINVAL;
        }
        if (svga.contexts[cid].batch_state_owner != metadata->transaction_id) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                SVGA_BATCH_SUBMIT_DETAIL_SHADER_CONTEXT_OWNER,
                metadata->transaction_id,
                svga.contexts[cid].batch_state_owner);
            return SVGA_EINVAL;
        }
        if (op->type != SVGA3D_SHADERTYPE_VS &&
            op->type != SVGA3D_SHADERTYPE_PS) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                SVGA_BATCH_SUBMIT_DETAIL_SHADER_TYPE, op->type,
                SVGA3D_SHADERTYPE_VS);
            return SVGA_EINVAL;
        }
        if (op->kind == SVGA_BATCH_SHADER_BIND && op->slot == UINT32_MAX) {
            if (op->local_id != UINT32_MAX) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_UNBIND_LOCAL_ID,
                    op->local_id, UINT32_MAX);
                return SVGA_EINVAL;
            }
            continue;
        }
        if (op->kind != SVGA_BATCH_SHADER_BIND &&
            op->kind != SVGA_BATCH_SHADER_DEFINE &&
            op->kind != SVGA_BATCH_SHADER_DESTROY) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                SVGA_BATCH_SUBMIT_DETAIL_SHADER_KIND, op->kind,
                SVGA_BATCH_SHADER_DESTROY);
            return SVGA_EINVAL;
        }
        if (op->slot >= SVGA_GB_SHADER_SLOTS) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                SVGA_BATCH_SUBMIT_DETAIL_SHADER_SLOT, op->slot,
                SVGA_GB_SHADER_SLOTS);
            return SVGA_EINVAL;
        }
        const struct svga_shader *shader = &svga.shaders[op->slot];
        if (op->kind == SVGA_BATCH_SHADER_DEFINE) {
            ++shader_define_count;
            if (!shader->reserved) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_DEFINE_RESERVED,
                    0, 1);
                return SVGA_EINVAL;
            }
            if (shader->reserved_by != metadata->transaction_id) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_DEFINE_OWNER,
                    metadata->transaction_id, shader->reserved_by);
                return SVGA_EINVAL;
            }
            if (shader->context != op->context) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_DEFINE_CONTEXT,
                    op->context, shader->context);
                return SVGA_EINVAL;
            }
            if (shader->local_id != op->local_id) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_DEFINE_LOCAL_ID,
                    op->local_id, shader->local_id);
                return SVGA_EINVAL;
            }
            if (shader->type != op->type) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_DEFINE_TYPE,
                    op->type, shader->type);
                return SVGA_EINVAL;
            }
            if (!shader->code.phys) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_DEFINE_CODE, 0, 1);
                return SVGA_EINVAL;
            }
        } else if (op->kind == SVGA_BATCH_SHADER_DESTROY) {
            if (!shader->defined && !shader->reserved) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_STATE,
                    shader->defined, shader->reserved);
                return SVGA_EINVAL;
            }
            if (shader->retiring) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_RETIRING,
                    1, 0);
                return SVGA_EINVAL;
            }
            if (shader->context != op->context) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_CONTEXT,
                    op->context, shader->context);
                return SVGA_EINVAL;
            }
            if (shader->local_id != op->local_id) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_LOCAL_ID,
                    op->local_id, shader->local_id);
                return SVGA_EINVAL;
            }
            if (shader->type != op->type) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_TYPE,
                    op->type, shader->type);
                return SVGA_EINVAL;
            }
            if (shader->reserved &&
                shader->reserved_by != metadata->transaction_id) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_OWNER,
                    metadata->transaction_id, shader->reserved_by);
                return SVGA_EINVAL;
            }
        } else if ((!shader->defined && !shader->reserved) || shader->retiring ||
                   shader->context != op->context || shader->local_id != op->local_id ||
                   shader->type != op->type ||
                   (shader->reserved && shader->reserved_by != metadata->transaction_id)) {
            if (!shader->defined && !shader->reserved) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_STATE,
                    shader->defined, shader->reserved);
            } else if (shader->retiring) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_RETIRING,
                    1, 0);
            } else if (shader->context != op->context) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_CONTEXT,
                    op->context, shader->context);
            } else if (shader->local_id != op->local_id) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_LOCAL_ID,
                    op->local_id, shader->local_id);
            } else if (shader->type != op->type) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_TYPE,
                    op->type, shader->type);
            } else {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_SHADER, i,
                    SVGA_BATCH_SUBMIT_DETAIL_SHADER_RESOURCE_OWNER,
                    metadata->transaction_id, shader->reserved_by);
            }
            return SVGA_EINVAL;
        }
    }
    if (shader_define_count != metadata->reserved_shader_count) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_SHADER,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_SHADER_DEFINE_COUNT,
            shader_define_count, metadata->reserved_shader_count);
        return SVGA_EINVAL;
    }
    int query_ret = svga_query_validate_batch_locked(batch);
    if (query_ret) return query_ret;
    int ret = fifo_batch_try_locked(batch->words, batch->bytes, fence, batch);
    if (!ret) {
        batch->submitted = true;
        svga_batch_state_commit_locked(batch);
        svga_shader_commit_batch_locked(batch, *fence);
        for (uint32_t i = 0; i < batch->context_count; ++i) {
            int id = svga_find_context(metadata->contexts[i]);
            svga.contexts[id].last_use_fence = *fence;
            for (uint32_t type = 0; type < 2; ++type) {
                uint32_t shid = svga.contexts[id].shaders[type];
                if (shid < SVGA_GB_SHADER_SLOTS && svga.shaders[shid].defined)
                    svga.shaders[shid].last_use_fence = *fence;
            }
        }
        for (uint32_t i = 0; i < batch->surface_count; ++i) {
            int id = svga_find_surface(metadata->surfaces[i]);
            svga.surfaces[id].last_use_fence = *fence;
        }
        for (uint32_t i = 0; i < batch->gmr_count; ++i) {
            int id = svga_find_gmr(metadata->gmrs[i]);
            svga.gmrs[id].last_use_fence = *fence;
        }
        svga_query_commit_batch_locked(batch, *fence);
    }
    return ret;
}

/**
 * @brief Submit an open batch atomically and release metadata after commit.
 * @param batch Caller-owned batch prepared for the current generation.
 * @param fence Writable output for the issued fence token.
 * @return Zero after commit, SVGA_EBUSY for retryable pressure, or a negative
 *         SVGA error; an unsubmitted batch remains available for retry/abort.
 */
int svga_batch_submit(struct svga_batch *batch, svga_handle *fence)
{
    if (!batch) return SVGA_EINVAL;
    svga_batch_submit_failure_reset(batch);
    if (!fence) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_BATCH_SHAPE,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_FENCE_NULL, 0, 1);
        return SVGA_EINVAL;
    }
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int ret;
    if (!svga.available || !svga.fifo_ready) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_DEVICE,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_DEVICE_UNAVAILABLE,
            svga.available, svga.fifo_ready);
        ret = SVGA_ENODEV;
    } else {
        ret = svga_batch_submit_locked(batch, fence);
    }
    struct svga_batch_metadata *metadata = NULL;
    if (!ret) {
        metadata = batch->metadata;
        batch->metadata = NULL;
    }
    svga_unlock(flags);
    batch_metadata_free(metadata);
    return ret;
}

/**
 * @brief Abort staged work and reclaim all unpublished shader backing.
 * @param batch Open unsubmitted batch to discard.
 * @return Zero after release or SVGA_EBUSY if the first lock attempt fails.
 */
int svga_batch_abort(struct svga_batch *batch)
{
    if (!batch || batch->submitted || !batch->metadata) return SVGA_EINVAL;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    struct svga_batch_metadata *metadata = batch->metadata;
    svga_shader_abort_batch_locked(batch);
    svga_unlock(flags);
    for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i)
        svga_shader_backing_free(&metadata->allocator, &metadata->abort_backings[i]);
    if (metadata->reserved_pages) {
        flags = svga_lock();
        if (svga.shader_reserved_pages >= metadata->reserved_pages)
            svga.shader_reserved_pages -= metadata->reserved_pages;
        else
            svga.shader_reserved_pages = 0;
        svga_unlock(flags);
    }
    batch_metadata_free(metadata);
    batch->metadata = NULL;
    batch->words = NULL;
    batch->capacity = 0;
    batch->bytes = 0;
    return 0;
}

/** @brief Check a generation-tagged token against issued and raw passed sequences.
 * Caller holds svga_lock.
 * @param fence Fence token returned by a FIFO submission.
 * @return True only when the token was issued in this generation and passed.
 */
bool svga_fence_reached_locked(svga_handle fence)
{
    uint32_t seq = (uint32_t)fence;
    if (!seq || (fence >> 32) != svga.generation ||
        (int32_t)(svga.issued_fence - seq) < 0 || !svga.fifo)
        return false;
    svga_barrier();
    return (int32_t)(svga.fifo[SVGA_FIFO_FENCE] - seq) >= 0;
}

/**
 * @brief Query real FIFO completion for a generation-tagged fence token.
 * @param fence Previously issued fence token.
 * @param signaled Writable completion output, false for an unfinished token.
 * @return Zero on a valid issued token, SVGA_EINVAL for an unissued token,
 *         SVGA_EIO for stale generation, or a negative SVGA error.
 */
int svga_fence_status(svga_handle fence, bool *signaled)
{
    if (!signaled) return SVGA_EINVAL;
    *signaled = false;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    uint32_t seq = (uint32_t)fence;
    int ret = 0;
    if (!seq || (fence >> 32) != svga.generation) ret = SVGA_EIO;
    else if (!(svga.fifo_caps & SVGA_FIFO_CAP_FENCE)) ret = SVGA_ENOTSUP;
    else if ((int32_t)(svga.issued_fence - seq) < 0) ret = SVGA_EINVAL;
    else *signaled = svga_fence_reached_locked(fence);
    svga_unlock(flags);
    return ret;
}

/**
 * @brief Read generation and raw issued/passed fence progress under svga_lock.
 * @param out Writable snapshot output.
 * @return Zero on success or a negative SVGA error for lock/FIFO state.
 */
int svga_fence_progress_snapshot(struct svga_fence_progress *out)
{
    if (!out) return SVGA_EINVAL;
    *out = (struct svga_fence_progress){0};
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int ret = 0;
    if (!svga.available || !svga.fifo_ready || !svga.fifo)
        ret = SVGA_ENODEV;
    else if (!(svga.fifo_caps & SVGA_FIFO_CAP_FENCE))
        ret = SVGA_ENOTSUP;
    else {
        svga_barrier();
        *out = (struct svga_fence_progress){svga.generation,
            svga.issued_fence, svga.fifo[SVGA_FIFO_FENCE]};
    }
    svga_unlock(flags);
    return ret;
}

bool svga_fence_is_quiesced(svga_handle fence)
{
    uint32_t generation = (uint32_t)(fence >> 32);
    uint32_t sequence = (uint32_t)fence;
    if (!generation || !sequence) return false;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return false;
    bool quiesced = false;
    for (uint32_t i = 0; i < SVGA_FENCE_QUIESCENCE_HISTORY; ++i) {
        const struct svga_fence_quiescence_record *record =
            &fence_quiescence_history[i];
        if (record->generation == generation &&
            (int32_t)(record->passed_seq - sequence) >= 0) {
            quiesced = true;
            break;
        }
    }
    svga_unlock(flags);
    return quiesced;
}

/** @brief Queue a real fence-only FIFO submission without blocking.
 * @param out Writable generation-tagged token for the newly issued fence.
 * @return Zero on commit, SVGA_EBUSY for transient lock/FIFO pressure, or a
 *         negative SVGA error. A failed call publishes no FIFO words.
 */
int svga_fence_try_insert(svga_handle *out)
{
    if (!out) return SVGA_EINVAL;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int ret = svga.available && svga.fifo_ready
        ? svga_fifo_batch_try_locked(NULL, 0, out) : SVGA_ENODEV;
    svga_unlock(flags);
    return ret;
}

/** @brief Hash a generation-tagged handle into the typed batch reference index.
 * @param handle Nonzero generation-tagged resource handle.
 * @param capacity Power-of-two hash table capacity.
 * @return Index of the first probe slot.
 */
static uint32_t batch_reference_hash(svga_handle handle, uint32_t capacity)
{
    uint64_t mixed = handle ^ (handle >> 33);
    mixed *= 0xff51afd7ed558ccdULL;
    mixed ^= mixed >> 33;
    return (uint32_t)mixed & (capacity - 1u);
}

/** @brief Rebuild a typed batch reference index after a staged append rolls back.
 * @param hash Open-addressed hash table to clear and rebuild.
 * @param hash_capacity Power-of-two hash table capacity.
 * @param items Ordered retained-handle list.
 * @param count Number of retained handles to index.
 */
static void batch_reference_hash_rebuild(svga_handle *hash,
                                         uint32_t hash_capacity,
                                         const svga_handle *items,
                                         uint32_t count)
{
    __builtin_memset(hash, 0, hash_capacity * sizeof(*hash));
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t first = batch_reference_hash(items[i], hash_capacity);
        for (uint32_t probe = 0; probe < hash_capacity; ++probe) {
            uint32_t slot = (first + probe) & (hash_capacity - 1u);
            if (hash[slot]) continue;
            hash[slot] = items[i];
            break;
        }
    }
}

/** @brief Deduplicate one typed handle in the fixed batch reference index.
 * @param batch Open batch owning typed reference metadata.
 * @param handle Nonzero generation-tagged resource handle.
 * @param items Typed array to update.
 * @param count Current number of unique handles in the array.
 * @param capacity Maximum distinct handles allowed for this resource kind.
 * @param hash Open-addressed hash table storing deduplication keys.
 * @param hash_capacity Power-of-two hash table capacity.
 * @return Zero when already retained or appended, or SVGA_ENOMEM at capacity.
 */
static int batch_retain(struct svga_batch *batch, svga_handle handle,
                        svga_handle *items, uint32_t *count, uint32_t capacity,
                        svga_handle *hash, uint32_t hash_capacity)
{
    if (!batch || !handle || batch->submitted || !batch->metadata ||
        !items || !count || !hash || !hash_capacity ||
        (hash_capacity & (hash_capacity - 1u)))
        return SVGA_EINVAL;
    uint32_t slot = batch_reference_hash(handle, hash_capacity);
    for (uint32_t probe = 0; probe < hash_capacity; ++probe) {
        svga_handle existing = hash[(slot + probe) & (hash_capacity - 1u)];
        if (existing == handle) return 0;
        if (existing) continue;
        if (*count >= capacity) return SVGA_ENOMEM;
        hash[(slot + probe) & (hash_capacity - 1u)] = handle;
        items[(*count)++] = handle;
        return 0;
    }
    return SVGA_ENOMEM;
}

/** @brief Restore typed references and dedup indexes after a failed append.
 * @param batch Open batch whose sidecar reference tables are rebuilt.
 * @param contexts Number of context handles to keep.
 * @param surfaces Number of surface handles to keep.
 * @param gmrs Number of GMR handles to keep.
 */
void svga_batch_rollback_references(struct svga_batch *batch,
                                   uint32_t contexts, uint32_t surfaces,
                                   uint32_t gmrs)
{
    if (!batch || !batch->metadata || contexts > batch->context_count ||
        surfaces > batch->surface_count || gmrs > batch->gmr_count)
        return;
    struct svga_batch_metadata *metadata = batch->metadata;
    batch->context_count = contexts;
    batch->surface_count = surfaces;
    batch->gmr_count = gmrs;
    batch_reference_hash_rebuild(metadata->context_hash,
        SVGA_BATCH_CONTEXT_HASH_CAPACITY, metadata->contexts, contexts);
    batch_reference_hash_rebuild(metadata->surface_hash,
        SVGA_BATCH_SURFACE_HASH_CAPACITY, metadata->surfaces, surfaces);
    batch_reference_hash_rebuild(metadata->gmr_hash,
        SVGA_BATCH_GMR_HASH_CAPACITY, metadata->gmrs, gmrs);
}

int svga_batch_retain_context(struct svga_batch *batch, svga_handle context)
{
    struct svga_batch_metadata *metadata = batch ? batch->metadata : NULL;
    return batch_retain(batch, context, metadata ? metadata->contexts : NULL,
                        batch ? &batch->context_count : NULL,
                        SVGA_BATCH_MAX_CONTEXT_REFS,
                        metadata ? metadata->context_hash : NULL,
                        SVGA_BATCH_CONTEXT_HASH_CAPACITY);
}

int svga_batch_retain_surface(struct svga_batch *batch, svga_handle surface)
{
    struct svga_batch_metadata *metadata = batch ? batch->metadata : NULL;
    return batch_retain(batch, surface, metadata ? metadata->surfaces : NULL,
                        batch ? &batch->surface_count : NULL,
                        SVGA_BATCH_MAX_SURFACE_REFS,
                        metadata ? metadata->surface_hash : NULL,
                        SVGA_BATCH_SURFACE_HASH_CAPACITY);
}

int svga_batch_retain_gmr(struct svga_batch *batch, svga_handle buffer)
{
    struct svga_batch_metadata *metadata = batch ? batch->metadata : NULL;
    return batch_retain(batch, buffer, metadata ? metadata->gmrs : NULL,
                        batch ? &batch->gmr_count : NULL,
                        SVGA_BATCH_MAX_GMR_REFS,
                        metadata ? metadata->gmr_hash : NULL,
                        SVGA_BATCH_GMR_HASH_CAPACITY);
}

int svga_command_locked(uint32_t id, const void *data, uint32_t bytes)
{
    struct svga_span span = {data, bytes};
    return svga_fifo_packet_locked(id, true, &span, 1);
}

int svga_drain_locked(void)
{
    uint32_t next, stop;
    int ret = ring_state(&next, &stop, NULL, NULL, NULL);
    if (ret) return ret;
    fifo_wake_locked();
    for (uint32_t i = 0; i < SVGA_POLL_LIMIT; ++i) {
        uint32_t busy = svga.ops.read(SVGA_REG_BUSY);
        ret = ring_state(&next, &stop, NULL, NULL, NULL);
        if (ret) return ret;
        if (!busy && next == stop) { svga_barrier(); return 0; }
        svga_relax();
    }
    return SVGA_ETIMEDOUT;
}

int svga_fence_locked(svga_handle *out)
{
    if (!out || !(svga.fifo_caps & SVGA_FIFO_CAP_FENCE)) return SVGA_ENOTSUP;
    uint32_t seq = svga.next_fence;
    if (!seq) seq = 1;
    if ((uint32_t)(seq - svga.fifo[SVGA_FIFO_FENCE]) >= 0x80000000u) {
        int ret = svga_drain_locked();
        if (ret) return ret;
    }
    struct svga_span span = {&seq, 4};
    int ret = svga_fifo_packet_locked(SVGA_CMD_FENCE, false, &span, 1);
    if (ret) return ret;
    svga.issued_fence = seq;
    svga.next_fence = seq + 1;
    *out = ((uint64_t)svga.generation << 32) | seq;
    return 0;
}

static void wait_delay_locked(uint32_t poll)
{
    /* Polling FIFO_FENCE is plain MMIO; pace the loop without the
     * synchronous FIFO processing caused by repeated SVGA_REG_BUSY reads. */
    uint32_t pauses = 16u + (poll >> 3);
    if (pauses > 1024u) pauses = 1024u;
    for (uint32_t i = 0; i < pauses; ++i) svga_relax();
}

int svga_wait_locked(svga_handle fence)
{
    uint32_t seq = (uint32_t)fence;
    if (!seq || (fence >> 32) != svga.generation ||
        (int32_t)(svga.issued_fence - seq) < 0) return SVGA_EINVAL;
    if (!(svga.fifo_caps & SVGA_FIFO_CAP_FENCE)) return SVGA_ENOTSUP;
    fifo_wake_locked();
    /* Fast path: Fence completion is published through plain FIFO MMIO.
     * Poll it without SVGA_REG_BUSY reads, which synchronously process the
     * FIFO in the guest vCPU and are billed as guest CPU time. */
    for (uint32_t i = 0; i < SVGA_POLL_LIMIT; ++i) {
        if ((int32_t)(svga.fifo[SVGA_FIFO_FENCE] - seq) >= 0) {
            svga_barrier(); return 0;
        }
        wait_delay_locked(i);
    }
    /* Slow-start fallback: preserve the old bounded synchronous loop for
     * hosts that only make progress while SVGA_REG_BUSY is being read. */
    for (uint32_t i = 0;; ++i) {
        if ((int32_t)(svga.fifo[SVGA_FIFO_FENCE] - seq) >= 0) {
            svga_barrier(); return 0;
        }
        if (i == SVGA_POLL_LIMIT) return SVGA_ETIMEDOUT;
        (void)svga.ops.read(SVGA_REG_BUSY);
        svga_relax();
    }
}

int svga_sync_locked(void)
{
    svga_handle fence;
    svga.fifo_notify_deferred = false;
    if (!(svga.fifo_caps & SVGA_FIFO_CAP_FENCE)) return svga_drain_locked();
    int ret = svga_fence_locked(&fence);
    if (ret) return ret;
    ret = svga_wait_locked(fence);
    if (!ret)
        fence_quiescence_record_locked(svga.generation, svga.issued_fence);
    return ret;
}

int svga_fence_insert(svga_handle *out)
{
    uint64_t flags = svga_lock();
    int ret = svga_fence_locked(out);
    svga_unlock(flags);
    return ret;
}

int svga_fence_wait(svga_handle fence)
{
    uint64_t flags = svga_lock();
    int ret = svga_wait_locked(fence);
    svga_unlock(flags);
    return ret;
}
