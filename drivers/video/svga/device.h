#ifndef RELIEFOS_SVGA_DEVICE_H
#define RELIEFOS_SVGA_DEVICE_H
#include <reliefnt/svga.h>
#include "svga_protocol.h"

#define SVGA_CONTEXTS 64u
#define SVGA_SURFACES 256u
#define SVGA_GMRS 128u
#define SVGA_MAX_GUEST_PAGES 16384u
#define SVGA_MAX_SURFACE_BYTES (128ULL * 1024 * 1024)
#define SVGA_MAX_MIPS 16u
#define SVGA_POLL_LIMIT 4000u
#define SVGA_KIND_CONTEXT 1u
#define SVGA_KIND_SURFACE 2u
#define SVGA_KIND_GMR 3u
#define SVGA_KIND_GPU 4u
#define SVGA_GPU_CONTEXTS 16u
#define SVGA_GB_SHADER_SLOTS 64u
#define SVGA_MOBS (SVGA_CONTEXTS + SVGA_SURFACES + SVGA_GB_SHADER_SLOTS + SVGA_GMRS)
#define SVGA_SHADER_MOB_BASE (SVGA_CONTEXTS + SVGA_SURFACES)
#define SVGA_EXTERNAL_MOB_BASE (SVGA_SHADER_MOB_BASE + SVGA_GB_SHADER_SLOTS)
#define SVGA_QUERY_PENDING_MAX 256u
#define SVGA_BATCH_SHADER_KEY_CAPACITY (SVGA_GB_SHADER_SLOTS * 4u)
#define SVGA_BATCH_QUERY_KEY_CAPACITY 1024u
#define SVGA_BATCH_CONTEXT_HASH_CAPACITY (SVGA_BATCH_MAX_CONTEXT_REFS * 2u)
#define SVGA_BATCH_SURFACE_HASH_CAPACITY (SVGA_BATCH_MAX_SURFACE_REFS * 2u)
#define SVGA_BATCH_GMR_HASH_CAPACITY (SVGA_BATCH_MAX_GMR_REFS * 2u)
#define SVGA_BATCH_METADATA_MAX_BYTES (8u * 1024u * 1024u)
#define SVGA_BATCH_METADATA_MAX_LIVE 32u

/** @brief Clear one caller-owned submit snapshot before a new attempt.
 * @param batch Batch whose previous numeric failure must no longer be visible;
 *        NULL is accepted.
 */
static inline void svga_batch_submit_failure_reset(struct svga_batch *batch)
{
    if (batch)
        batch->submit_failure = (struct svga_batch_submit_failure){
            .reason = SVGA_BATCH_SUBMIT_REASON_NONE,
            .index = SVGA_BATCH_SUBMIT_INDEX_NONE,
        };
}

/** @brief Store a fixed numeric reason without allocating or exposing pointers.
 * @param batch Caller-owned batch that receives the failure snapshot; NULL is accepted.
 * @param reason Final-submit reason category.
 * @param index Operation or retained-reference index, or the no-index sentinel.
 * @param detail0 First reason-specific numeric check or state value.
 * @param detail1 Second reason-specific numeric value.
 * @param detail2 Third reason-specific numeric value.
 */
static inline void svga_batch_submit_failure_set(
    struct svga_batch *batch, enum svga_batch_submit_reason reason,
    uint32_t index, uint64_t detail0, uint64_t detail1, uint64_t detail2)
{
    if (!batch) return;
    batch->submit_failure = (struct svga_batch_submit_failure){
        .reason = (uint32_t)reason, .index = index,
        .detail0 = detail0, .detail1 = detail1, .detail2 = detail2,
    };
}

struct svga_ops {
    uint32_t (*read)(uint32_t reg);
    void (*write)(uint32_t reg, uint32_t value);
    uint64_t (*alloc)(uint32_t pages);
    void (*free)(uint64_t phys, uint32_t pages);
    void *(*pointer)(uint64_t phys);
    uint64_t (*clock)(void);
};
struct svga_mob {
    uint64_t phys, pt_phys;
    uint32_t pages, pt_pages, bytes, depth;
    bool defined, borrowed;
};
struct svga_gb_resource {
    struct svga_mob mob;
    svga_handle retire_fence;
    bool defined, bound, retiring, mode_lease;
};
struct svga_context {
    svga_handle handle;
    uint32_t targets[SVGA3D_RT_MAX], textures[32];
    uint32_t shaders[2];
    svga_handle last_use_fence, ready_fence;
    bool query_active;
    uint32_t query_attachment;
    uint64_t batch_state_owner;
    struct svga_gb_resource gb;
};
struct svga_surface {
    svga_handle handle;
    struct svga_surface_desc desc;
    uint32_t block_w, block_h, block_bytes, faces;
    uint64_t bytes;
    svga_handle last_use_fence, ready_fence, retire_fence;
    bool creating, retiring;
    struct svga_gb_resource gb;
};
struct svga_gmr {
    svga_handle handle;
    uint64_t phys;
    uint32_t bytes, pages;
    uint32_t mapped_pages, map_pending_pages, mobid;
    svga_handle last_use_fence, progress_fence, retire_fence;
    void *owner;
    void (*owner_put)(void *owner);
    bool defined, retiring, mode_lease;
    bool borrowed, mob_defined, map_inflight;
};
struct svga_shader {
    svga_handle context, ready_fence, last_use_fence, retire_fence;
    uint32_t local_id, host_id, mobid, type, bytes, serial;
    uint32_t reserved_pages;
    uint64_t reserved_by;
    struct svga_mob code;
    struct svga_ops allocator;
    bool defined, retiring, reserved;
};
struct svga_query_pending {
    uint32_t context, gmr;
    uint32_t offset;
    svga_handle end_fence, wait_fence;
    bool used;
};
struct svga_batch_shader_key {
    svga_handle context;
    uint32_t local_id, type, slot;
    bool used;
};
struct svga_batch_shader_binding {
    svga_handle context;
    uint32_t slots[2];
    bool used;
};
struct svga_batch_query_key {
    uint32_t context, gmr, offset;
    uint16_t pending_count, staged_count;
    uint16_t pending_head, pending_tail, staged_head, staged_tail;
    bool used;
};
enum svga_batch_operation_array {
    SVGA_BATCH_OPERATION_ARRAY_QUERY = 1,
    SVGA_BATCH_OPERATION_ARRAY_SHADER,
    SVGA_BATCH_OPERATION_ARRAY_BINDING,
};
struct svga_batch_metadata {
    uint32_t operation_capacity, allocation_bytes;
    uint32_t query_op_capacity, shader_op_capacity, binding_op_capacity;
    svga_handle contexts[SVGA_BATCH_MAX_CONTEXT_REFS];
    svga_handle surfaces[SVGA_BATCH_MAX_SURFACE_REFS];
    svga_handle gmrs[SVGA_BATCH_MAX_GMR_REFS];
    svga_handle context_hash[SVGA_BATCH_CONTEXT_HASH_CAPACITY];
    svga_handle surface_hash[SVGA_BATCH_SURFACE_HASH_CAPACITY];
    svga_handle gmr_hash[SVGA_BATCH_GMR_HASH_CAPACITY];
    struct svga_batch_query_op *query_ops;
    struct svga_batch_shader_op *shader_ops;
    struct svga_batch_binding_op *binding_ops;
    uint64_t transaction_id;
    uint32_t state_context_count;
    uint32_t state_context_ids[SVGA_CONTEXTS];
    bool state_context_seen[SVGA_CONTEXTS];
    uint32_t shader_key_count;
    struct svga_batch_shader_key shader_keys[SVGA_BATCH_SHADER_KEY_CAPACITY];
    struct svga_batch_shader_binding shader_bindings[SVGA_CONTEXTS];
    uint32_t reserved_shader_count;
    uint32_t reserved_shader_slots[SVGA_GB_SHADER_SLOTS];
    uint32_t reserved_pages;
    struct svga_mob abort_backings[SVGA_GB_SHADER_SLOTS];
    struct svga_ops allocator;
    bool query_overlay_initialized;
    uint32_t query_free_slots, query_staged_end_count, query_end_count;
    uint16_t query_free_slot_count;
    uint16_t query_free_slot_indices[SVGA_QUERY_PENDING_MAX];
    bool query_active[SVGA_CONTEXTS];
    uint32_t query_end_indices[SVGA_QUERY_PENDING_MAX];
    uint16_t query_pending_next[SVGA_QUERY_PENDING_MAX];
    uint16_t query_end_next[SVGA_QUERY_PENDING_MAX];
    struct svga_batch_query_key query_keys[SVGA_BATCH_QUERY_KEY_CAPACITY];
};
#define SVGA_GPU_NO_SLOT UINT32_MAX
struct svga_gpu_slot {
    svga_handle color, depth, vertex, upload, readback;
    svga_handle fence;
};
struct svga_gpu_context {
    svga_handle handle, context;
    struct svga_gpu_slot slots[2];
    uint32_t owner, generation, width, height, vertex_capacity;
    uint32_t fill_mode, next_slot, pending_slot, last_slot;
    bool retiring, state_ready, has_last;
};
struct svga_gpu_stats {
    uint64_t last_clock, busy_ticks, busy_start;
    uint64_t submitted, completed, failed, triangles;
    uint32_t generation, device_generation;
    bool initialized, busy;
};
struct svga_device {
    struct svga_ops ops;
    volatile uint32_t *fifo;
    uint32_t fifo_bytes, min, caps, fifo_caps, host_version;
    uint32_t generation, handle_serial, next_fence, issued_fence;
    uint64_t transaction_serial;
    uint32_t shader_reserved_pages;
    uint32_t mode_leases;
    bool fifo_notify_deferred;
    /** Whether this configured FIFO header advertises the optional BUSY word. */
    bool fifo_busy_present;
    uint32_t cap_values[SVGA3D_DEVCAP_MAX];
    bool cap_valid[SVGA3D_DEVCAP_MAX];
    uint32_t context_limit, surface_limit, gmr_limit, page_limit, pages;
    uint64_t surface_bytes, surface_limit_bytes, compat_surface_limit_bytes;
    bool bound, fifo_ready, available, memory_ready, transitioning;
    bool traces_enabled;
    bool gb_active;
    uint32_t mob_max_bytes;
    struct svga_mob otables[SVGA_OTABLE_DX9_MAX];
    struct svga_probe_info probe;
    struct svga_context contexts[SVGA_CONTEXTS];
    struct svga_surface surfaces[SVGA_SURFACES];
    struct svga_gmr gmrs[SVGA_GMRS];
    struct svga_mob external_mobs[SVGA_GMRS];
    struct svga_shader shaders[SVGA_GB_SHADER_SLOTS];
    struct svga_query_pending queries[SVGA_QUERY_PENDING_MAX];
    struct svga_gpu_context gpu_contexts[SVGA_GPU_CONTEXTS];
    struct svga_gpu_stats gpu_stats;
    struct reliefos_gpu_diagnostics gpu_error;
    uint32_t gpu_error_owner;
};
struct svga_span { const void *data; uint32_t bytes; };
struct svga_cache_report {
    const char *policy;
    const char *reason;
    int status;
    bool legacy_restored;
};
extern struct svga_device svga;

/** @brief Check allocation against live pages and shader staging reservations.
 * Caller holds svga_lock.
 * @param pages Additional pages the resource would consume.
 * @return True only when the total remains within the negotiated page limit.
 */
static inline bool svga_page_budget_allows(uint32_t pages)
{
    if (svga.pages > svga.page_limit ||
        svga.shader_reserved_pages > svga.page_limit - svga.pages)
        return false;
    return pages <= svga.page_limit - svga.pages - svga.shader_reserved_pages;
}

/* Callers hold the shared device lock for every _locked helper. */
uint64_t svga_lock(void);
/* These helpers require the shared device lock to be held. */
int svga_mode_lease_acquire_locked(void);
int svga_mode_lease_release_locked(void);
/** @brief Attempt the shared device lock once with IRQ state saved.
 * @param flags Receives the saved IRQ state when the lock is acquired.
 * @return True when acquired; false without holding the lock on contention.
 */
bool svga_try_lock_irqsave(uint64_t *flags);
void svga_unlock(uint64_t flags);
void svga_barrier(void);
void svga_relax(void);
void svga_bind(const struct svga_ops *ops, volatile uint32_t *fifo, uint32_t bytes);
int svga_configure_locked(bool extended);
/** @brief Preserve the legacy 2D FIFO after a cache-policy setup failure.
 * @param cache_status Original negative cache setup status.
 * @return The original status when reconfiguration succeeds, otherwise EIO.
 * Caller holds svga_lock after a successful drain/quiesce.
 */
int svga_restore_legacy_after_cache_error_locked(int cache_status);
/** @brief Emit cache-policy diagnostics after releasing svga_lock.
 * @param report Selected policy or setup-failure details; NULL is ignored.
 * @note Caller must not hold the shared SVGA lock because console presentation
 *       can re-enter the framebuffer/SVGA path.
 */
void svga_report_cache_result_unlocked(const struct svga_cache_report *report);
int svga_fifo_packet_locked(uint32_t id, bool three_d, const struct svga_span *spans, uint32_t count);
/** @brief Queue a FIFO packet without waiting for space; full returns SVGA_EBUSY. */
int svga_fifo_packet_try_locked(uint32_t id, bool three_d,
                                const struct svga_span *spans, uint32_t count);
int svga_fifo_batch_try_locked(const uint32_t *words, uint32_t bytes,
                               svga_handle *fence);
int svga_batch_submit_locked(struct svga_batch *batch, svga_handle *fence);
/** @brief Ensure a batch operation array can hold needed live records.
 * The helper grows geometrically from four entries, capped by the batch's
 * source-derived operation_capacity. It copies only live records and must be
 * called without svga_lock held; allocation and replacement free occur before
 * the staging caller acquires that lock. A failed allocation leaves the array,
 * its capacity, and all batch counts unchanged.
 * @param batch Open caller-owned batch whose metadata owns the array.
 * @param array Query, shader, or binding operation array to reserve.
 * @param needed Required record count, bounded by operation_capacity.
 * @return Zero when capacity is available, SVGA_ENOMEM on capacity/allocation
 *         exhaustion, or SVGA_EINVAL for inconsistent batch metadata.
 */
int svga_batch_operation_reserve(struct svga_batch *batch,
                                 enum svga_batch_operation_array array,
                                 uint32_t needed);
int svga_batch_reserve_context_locked(struct svga_batch *batch, uint32_t cid);
int svga_batch_state_validate_locked(struct svga_batch *batch);
void svga_batch_state_commit_locked(const struct svga_batch *batch);
void svga_batch_rollback_references(struct svga_batch *batch,
                                   uint32_t contexts, uint32_t surfaces,
                                   uint32_t gmrs);
void svga_fifo_defer_notify_locked(void);
void svga_fifo_flush_notify_locked(void);
int svga_drain_locked(void);
int svga_fence_locked(svga_handle *out);
int svga_wait_locked(svga_handle fence);
int svga_sync_locked(void);
void svga_fence_quiescence_reset(void);
int svga_shutdown_locked(void);
int svga_negotiate_locked(void);
svga_handle svga_new_handle(uint32_t kind, uint32_t slot);
int svga_find_context(svga_handle handle);
int svga_find_surface(svga_handle handle);
int svga_find_gmr(svga_handle handle);
bool svga_context_usable(uint32_t id);
bool svga_surface_usable(uint32_t id);
int svga_context_destroy_locked(uint32_t id);
int svga_context_create_locked(svga_handle *out);
int svga_surface_create_locked(const struct svga_surface_desc *desc, svga_handle *out);
int svga_surface_destroy_locked(uint32_t id);
int svga_gmr_create_locked(uint32_t bytes, svga_handle *out);
int svga_gmr_destroy_locked(uint32_t id);
int svga_surface_dma_locked(svga_handle surface, uint32_t face, uint32_t mip,
    svga_handle buffer, uint32_t offset, uint32_t pitch, uint32_t direction,
    const struct svga_dma_box *boxes, uint32_t count);
void svga_gpu_reset_locked(void);
int svga_format(uint32_t format, uint32_t *bw, uint32_t *bh, uint32_t *bytes, uint32_t *cap);
int svga_command_locked(uint32_t id, const void *data, uint32_t bytes);
int svga_gb_init_locked(void);
int svga_gb_shutdown_locked(void);
int svga_gb_context_create_locked(uint32_t id);
int svga_gb_surface_create_locked(uint32_t id);
int svga_gb_destroy_locked(struct svga_gb_resource *resource, uint32_t id, bool context);
int svga_gb_backing_alloc_locked(struct svga_mob *mob, uint32_t bytes);
int svga_gb_backing_borrow_locked(struct svga_mob *mob, uint64_t phys,
                                  uint32_t pages, uint32_t bytes);
void svga_gb_backing_free_locked(struct svga_mob *mob);
uint32_t svga_gb_backing_root(const struct svga_mob *mob);
bool svga_fence_reached_locked(svga_handle fence);
void svga_shader_commit_batch_locked(struct svga_batch *batch,
                                     svga_handle fence);
void svga_shader_abort_batch_locked(struct svga_batch *batch);
void svga_shader_backing_free(const struct svga_ops *ops, struct svga_mob *mob);
int svga_query_validate_batch_locked(struct svga_batch *batch);
void svga_query_commit_batch_locked(const struct svga_batch *batch,
                                    svga_handle fence);
bool svga_query_gmr_in_use_locked(uint32_t gmr);
bool svga_query_context_busy_locked(uint32_t context);
int svga_query_prepare_gmr_retire_locked(uint32_t gmr);
int svga_query_prepare_context_retire_locked(uint32_t context);

/* Bootstrap framebuffer adapter. All register I/O uses this same lock. */
void svga_platform_bind(uint16_t port, volatile uint32_t *fifo, uint32_t bytes);
/** @brief Queue a 2D scanout update while the SVGA device lock is held. */
int svga_update_locked(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
/** @brief Queue an UPDATE without waiting when FIFO space is unavailable. */
int svga_update_try_locked(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
int svga_update(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
/** @brief Restore CAP_TRACES policy while the shared device lock is held. */
bool svga_restore_traces_locked(void);
int svga_mode_begin(uint64_t *flags);
void svga_mode_end(uint64_t flags);
uint32_t svga_read_register(uint32_t reg);
void svga_write_register(uint32_t reg, uint32_t value);

#endif
