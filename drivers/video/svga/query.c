#include "device.h"

#define SVGA_QUERY_OP_BEGIN 1u
#define SVGA_QUERY_OP_END 2u
#define SVGA_QUERY_OP_WAIT 3u

/** @brief Drop waited query records whose actual FIFO fence has passed.
 * Caller holds svga_lock; only completed wait records are reclaimed.
 */
static void query_reap_locked(void)
{
    for (uint32_t i = 0; i < SVGA_QUERY_PENDING_MAX; ++i) {
        struct svga_query_pending *query = &svga.queries[i];
        if (query->used && query->wait_fence &&
            svga_fence_reached_locked(query->wait_fence))
            *query = (struct svga_query_pending){0};
    }
}

/** @brief Hash a query result key into the fixed transaction-local table.
 * @param context SVGA context table index.
 * @param gmr SVGA GMR table index.
 * @param offset Byte offset of the query result.
 * @return Masked initial hash-table index.
 */
static uint32_t query_key_hash(uint32_t context, uint32_t gmr,
                               uint32_t offset)
{
    uint32_t value = context * 0x9e3779b1u ^ gmr * 0x85ebca6bu ^
                     offset * 0xc2b2ae35u;
    value ^= value >> 16;
    return value & (SVGA_BATCH_QUERY_KEY_CAPACITY - 1u);
}

/** @brief Find or create a context/GMR/offset query overlay entry.
 * @param metadata Batch sidecar holding the bounded key table.
 * @param context SVGA context table index.
 * @param gmr SVGA GMR table index.
 * @param offset Byte offset of the result.
 * @param create True to initialize an absent entry.
 * @return Matching entry or NULL when absent/table capacity is exhausted.
 */
static struct svga_batch_query_key *query_key_get(
    struct svga_batch_metadata *metadata, uint32_t context, uint32_t gmr,
    uint32_t offset, bool create)
{
    uint32_t first = query_key_hash(context, gmr, offset);
    for (uint32_t probe = 0; probe < SVGA_BATCH_QUERY_KEY_CAPACITY; ++probe) {
        struct svga_batch_query_key *key =
            &metadata->query_keys[(first + probe) &
                                  (SVGA_BATCH_QUERY_KEY_CAPACITY - 1u)];
        if (!key->used) {
            if (!create) return NULL;
            *key = (struct svga_batch_query_key){.context = context,
                .gmr = gmr, .offset = offset,
                .pending_head = UINT16_MAX, .pending_tail = UINT16_MAX,
                .staged_head = UINT16_MAX, .staged_tail = UINT16_MAX,
                .used = true};
            return key;
        }
        if (key->context == context && key->gmr == gmr && key->offset == offset)
            return key;
    }
    return NULL;
}

/** @brief Append a persistent pending-result slot to its indexed key queue.
 * @param metadata Batch sidecar containing link storage.
 * @param key Key queue that receives the slot.
 * @param index Persistent query-table slot, less than SVGA_QUERY_PENDING_MAX.
 */
static void query_pending_append(struct svga_batch_metadata *metadata,
                                 struct svga_batch_query_key *key,
                                 uint16_t index)
{
    metadata->query_pending_next[index] = UINT16_MAX;
    if (key->pending_tail == UINT16_MAX) key->pending_head = index;
    else metadata->query_pending_next[key->pending_tail] = index;
    key->pending_tail = index;
    ++key->pending_count;
}

/** @brief Remove the oldest persistent result slot from a key queue.
 * @param metadata Batch sidecar containing link storage.
 * @param key Key queue to consume.
 * @return Query slot index, or UINT16_MAX when the queue is empty.
 */
static uint16_t query_pending_pop(struct svga_batch_metadata *metadata,
                                 struct svga_batch_query_key *key)
{
    if (!key->pending_count || key->pending_head == UINT16_MAX)
        return UINT16_MAX;
    uint16_t index = key->pending_head;
    key->pending_head = metadata->query_pending_next[index];
    if (key->pending_head == UINT16_MAX) key->pending_tail = UINT16_MAX;
    metadata->query_pending_next[index] = UINT16_MAX;
    --key->pending_count;
    return index;
}

/** @brief Restore a persistent slot after a failed staged WAIT append.
 * @param metadata Batch sidecar containing link storage.
 * @param key Key queue that receives the slot.
 * @param index Persistent query-table slot to restore.
 */
static void query_pending_push_front(struct svga_batch_metadata *metadata,
                                     struct svga_batch_query_key *key,
                                     uint16_t index)
{
    metadata->query_pending_next[index] = key->pending_head;
    key->pending_head = index;
    if (key->pending_tail == UINT16_MAX) key->pending_tail = index;
    ++key->pending_count;
}

/** @brief Append a staged END ordinal to the matching key queue.
 * @param metadata Batch sidecar containing END index and link arrays.
 * @param key Key queue that receives the ordinal.
 * @param ordinal Staged END ordinal less than SVGA_QUERY_PENDING_MAX.
 */
static void query_staged_append(struct svga_batch_metadata *metadata,
                                struct svga_batch_query_key *key,
                                uint16_t ordinal)
{
    metadata->query_end_next[ordinal] = UINT16_MAX;
    if (key->staged_tail == UINT16_MAX) key->staged_head = ordinal;
    else metadata->query_end_next[key->staged_tail] = ordinal;
    key->staged_tail = ordinal;
    ++key->staged_count;
}

/** @brief Remove the oldest staged END ordinal from a key queue.
 * @param metadata Batch sidecar containing END index and link arrays.
 * @param key Key queue to consume.
 * @return END ordinal, or UINT16_MAX when the queue is empty.
 */
static uint16_t query_staged_pop(struct svga_batch_metadata *metadata,
                                struct svga_batch_query_key *key)
{
    if (!key->staged_count || key->staged_head == UINT16_MAX)
        return UINT16_MAX;
    uint16_t ordinal = key->staged_head;
    key->staged_head = metadata->query_end_next[ordinal];
    if (key->staged_head == UINT16_MAX) key->staged_tail = UINT16_MAX;
    metadata->query_end_next[ordinal] = UINT16_MAX;
    --key->staged_count;
    return ordinal;
}

/** @brief Restore a staged END after a failed WAIT append.
 * @param metadata Batch sidecar containing END index and link arrays.
 * @param key Key queue that receives the ordinal.
 * @param ordinal Staged END ordinal to restore.
 */
static void query_staged_push_front(struct svga_batch_metadata *metadata,
                                    struct svga_batch_query_key *key,
                                    uint16_t ordinal)
{
    metadata->query_end_next[ordinal] = key->staged_head;
    key->staged_head = ordinal;
    if (key->staged_tail == UINT16_MAX) key->staged_tail = ordinal;
    ++key->staged_count;
}

/** @brief Rebuild incremental query matching state from committed records.
 * Caller holds svga_lock; pending records and free commit slots are indexed.
 * @param batch Batch whose sidecar receives a fresh validation overlay.
 */
static void query_overlay_reset_locked(struct svga_batch *batch)
{
    struct svga_batch_metadata *metadata = batch->metadata;
    query_reap_locked();
    __builtin_memset(metadata->query_keys, 0, sizeof(metadata->query_keys));
    for (uint32_t i = 0; i < SVGA_QUERY_PENDING_MAX; ++i) {
        metadata->query_pending_next[i] = UINT16_MAX;
        metadata->query_end_next[i] = UINT16_MAX;
    }
    metadata->query_free_slots = 0;
    metadata->query_free_slot_count = 0;
    metadata->query_staged_end_count = 0;
    metadata->query_end_count = 0;
    for (uint32_t i = 0; i < SVGA_CONTEXTS; ++i)
        metadata->query_active[i] = svga.contexts[i].query_active;
    for (uint32_t i = 0; i < SVGA_QUERY_PENDING_MAX; ++i) {
        const struct svga_query_pending *pending = &svga.queries[i];
        if (!pending->used) {
            ++metadata->query_free_slots;
            metadata->query_free_slot_indices[metadata->query_free_slot_count++] =
                (uint16_t)i;
            continue;
        }
        if (pending->wait_fence) continue;
        struct svga_batch_query_key *key = query_key_get(metadata,
            pending->context, pending->gmr, pending->offset, true);
        if (key && key->pending_count != UINT16_MAX)
            query_pending_append(metadata, key, (uint16_t)i);
    }
    metadata->query_overlay_initialized = true;
}

/** @brief Lazily seed query state for the first query operation in a batch.
 * Caller holds svga_lock.
 * @param batch Batch whose sidecar receives the committed-state snapshot.
 */
static void query_overlay_init_locked(struct svga_batch *batch)
{
    if (!batch->metadata->query_overlay_initialized)
        query_overlay_reset_locked(batch);
}

/** @brief Check the source-derived operation budget for another query event.
 * @param batch Candidate caller-owned batch.
 * @return True when both shared operation and query-event capacity remain.
 */
static bool query_batch_has_capacity(const struct svga_batch *batch)
{
    return batch->metadata &&
           batch->operation_count < batch->metadata->operation_capacity &&
           batch->query_op_count < batch->metadata->operation_capacity;
}

/** @brief Validate a borrowed result attachment while svga_lock is held.
 * @param handle Borrowed GMR handle.
 * @param offset Byte offset of the fixed-size GB query result.
 * @param id Writable GMR table index output.
 * @return Zero when mapped and in bounds, or a negative SVGA error.
 */
static int query_result_attachment_locked(svga_handle handle, uint32_t offset,
                                          uint32_t *id)
{
    if (offset & 3u) return SVGA_EINVAL;
    int found = svga_find_gmr(handle);
    if (found < 0) return SVGA_EINVAL;
    struct svga_gmr *g = &svga.gmrs[found];
    if (!g->borrowed || g->retiring || !g->mob_defined || g->map_inflight ||
        g->mapped_pages != g->pages)
        return g->borrowed ? SVGA_EBUSY : SVGA_ENOTSUP;
    if (offset > g->bytes || SVGA_GB_QUERY_RESULT_BYTES > g->bytes - offset)
        return SVGA_EINVAL;
    *id = (uint32_t)found;
    return 0;
}

/** @brief Append and stage an ordered BEGIN query operation.
 * Caller holds svga_lock.
 * @param batch Open batch receiving the command and operation record.
 * @param cid Live context table index.
 * @param type Supported GB query type.
 * @return Zero on success or a negative SVGA error on invalid state/capacity.
 */
static int query_batch_begin_locked(struct svga_batch *batch, uint32_t cid,
                                    uint32_t type)
{
    if (!query_batch_has_capacity(batch)) return SVGA_ENOMEM;
    if (type != SVGA_GB_QUERY_TYPE_OCCLUSION) return SVGA_ENOTSUP;
    query_overlay_init_locked(batch);
    struct svga_batch_metadata *metadata = batch->metadata;
    if (metadata->query_active[cid]) return SVGA_EINVAL;
    SVGA3dCmdBeginGBQuery command = {cid, type};
    uint32_t old_context_count = batch->context_count;
    int ret = svga_batch_retain_context(batch, svga.contexts[cid].handle);
    if (!ret) ret = svga_batch_append(batch, SVGA_3D_CMD_BEGIN_GB_QUERY, true,
                                      &command, sizeof(command));
    if (ret) {
        svga_batch_rollback_references(batch, old_context_count,
            batch->surface_count, batch->gmr_count);
        return ret;
    }
    metadata->query_ops[batch->query_op_count++] =
        (struct svga_batch_query_op){.kind = SVGA_QUERY_OP_BEGIN,
            .context = cid, .gmr = UINT32_MAX, .commit_slot = UINT32_MAX};
    ++batch->operation_count;
    metadata->query_active[cid] = true;
    return 0;
}

/** @brief Append and stage an ordered END with a reserved pending-result slot.
 * Caller holds svga_lock.
 * @param batch Open batch receiving the command and operation record.
 * @param cid Live context table index.
 * @param type Supported GB query type.
 * @param result Borrowed GMR storing the result.
 * @param offset Byte offset within the result GMR.
 * @return Zero on success or a negative SVGA error on invalid state/capacity.
 */
static int query_batch_end_locked(struct svga_batch *batch, uint32_t cid,
                                  uint32_t type, svga_handle result,
                                  uint32_t offset)
{
    if (!query_batch_has_capacity(batch)) return SVGA_ENOMEM;
    if (type != SVGA_GB_QUERY_TYPE_OCCLUSION) return SVGA_ENOTSUP;
    query_overlay_init_locked(batch);
    struct svga_batch_metadata *metadata = batch->metadata;
    if (!metadata->query_active[cid]) return SVGA_EINVAL;
    if (metadata->query_staged_end_count >= metadata->query_free_slots)
        return SVGA_ENOMEM;
    uint32_t id;
    int ret = query_result_attachment_locked(result, offset, &id);
    if (ret) return ret;
    struct svga_batch_query_key *key = query_key_get(metadata, cid, id, offset,
                                                       true);
    if (!key || key->staged_count == UINT16_MAX) return SVGA_ENOMEM;
    SVGA3dCmdEndGBQuery command = {cid, type, svga.gmrs[id].mobid, offset};
    uint32_t old_context_count = batch->context_count;
    uint32_t old_gmr_count = batch->gmr_count;
    ret = svga_batch_retain_context(batch, svga.contexts[cid].handle);
    if (!ret) ret = svga_batch_retain_gmr(batch, result);
    if (!ret) ret = svga_batch_append(batch, SVGA_3D_CMD_END_GB_QUERY, true,
                                      &command, sizeof(command));
    if (ret) {
        svga_batch_rollback_references(batch, old_context_count,
            batch->surface_count, old_gmr_count);
        return ret;
    }
    uint32_t op_index = batch->query_op_count++;
    metadata->query_ops[op_index] = (struct svga_batch_query_op){
        .kind = SVGA_QUERY_OP_END, .context = cid, .gmr = id,
        .offset = offset, .commit_slot = UINT32_MAX};
    uint16_t ordinal = (uint16_t)metadata->query_end_count++;
    metadata->query_end_indices[ordinal] = op_index;
    query_staged_append(metadata, key, ordinal);
    ++metadata->query_staged_end_count;
    ++batch->operation_count;
    metadata->query_active[cid] = false;
    return 0;
}

/** @brief Append WAIT and record its exact persistent or staged END match.
 * Caller holds svga_lock.
 * @param batch Open batch receiving the command and operation record.
 * @param cid Live context table index.
 * @param type Supported GB query type.
 * @param result Borrowed GMR storing the result.
 * @param offset Byte offset within the result GMR.
 * @return Zero on success or a negative SVGA error for unmatched/capacity state.
 */
static int query_batch_wait_locked(struct svga_batch *batch, uint32_t cid,
                                   uint32_t type, svga_handle result,
                                   uint32_t offset)
{
    if (!query_batch_has_capacity(batch)) return SVGA_ENOMEM;
    if (type != SVGA_GB_QUERY_TYPE_OCCLUSION) return SVGA_ENOTSUP;
    query_overlay_init_locked(batch);
    struct svga_batch_metadata *metadata = batch->metadata;
    uint32_t id;
    int ret = query_result_attachment_locked(result, offset, &id);
    if (ret) return ret;
    struct svga_batch_query_key *key = query_key_get(metadata, cid, id, offset,
                                                       false);
    uint32_t match_kind = 0, match_index = UINT32_MAX;
    if (key && key->pending_count) {
        uint16_t pending = query_pending_pop(metadata, key);
        if (pending != UINT16_MAX) {
            match_kind = 1;
            match_index = pending;
        }
    }
    if (!match_kind && key && key->staged_count) {
        uint16_t ordinal = query_staged_pop(metadata, key);
        if (ordinal != UINT16_MAX) {
            match_kind = 2;
            match_index = metadata->query_end_indices[ordinal];
        }
    }
    if (!match_kind) return SVGA_EINVAL;
    SVGA3dCmdWaitForGBQuery command = {cid, type, svga.gmrs[id].mobid, offset};
    uint32_t old_context_count = batch->context_count;
    uint32_t old_gmr_count = batch->gmr_count;
    ret = svga_batch_retain_context(batch, svga.contexts[cid].handle);
    if (!ret) ret = svga_batch_retain_gmr(batch, result);
    if (!ret) ret = svga_batch_append(batch, SVGA_3D_CMD_WAIT_FOR_GB_QUERY, true,
                                      &command, sizeof(command));
    if (ret) {
        svga_batch_rollback_references(batch, old_context_count,
            batch->surface_count, old_gmr_count);
        if (match_kind == 1) {
            query_pending_push_front(metadata, key, (uint16_t)match_index);
        } else {
            for (uint32_t i = 0; i < metadata->query_end_count; ++i)
                if (metadata->query_end_indices[i] == match_index) {
                    query_staged_push_front(metadata, key, (uint16_t)i);
                    break;
                }
        }
        return ret;
    }
    metadata->query_ops[batch->query_op_count++] =
        (struct svga_batch_query_op){.kind = SVGA_QUERY_OP_WAIT,
            .context = cid, .gmr = id, .offset = offset,
            .match_kind = match_kind, .match_index = match_index,
            .commit_slot = UINT32_MAX};
    ++batch->operation_count;
    return 0;
}

/**
 * @brief Stage a GB query BEGIN into an open trusted batch.
 * @param batch Open batch whose source bound covers this operation.
 * @param context Live SVGA context handle.
 * @param type Supported GB query type.
 * @return Zero when staged or a negative SVGA error for lock/state/capacity.
 */
int svga_batch_query_begin(struct svga_batch *batch, svga_handle context,
                           uint32_t type)
{
    if (!batch || !context || batch->submitted || !batch->metadata)
        return SVGA_EINVAL;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int cid = svga_find_context(context), ret = SVGA_EINVAL;
    if (batch->generation == svga.generation && cid >= 0 &&
        !svga.contexts[cid].gb.retiring)
        ret = query_batch_begin_locked(batch, (uint32_t)cid, type);
    svga_unlock(flags);
    return ret;
}

/**
 * @brief Stage a GB query END and retain its result attachment through WAIT.
 * @param batch Open batch whose source bound covers this operation.
 * @param context Live SVGA context handle.
 * @param type Supported GB query type.
 * @param result Fully mapped borrowed GMR holding the result.
 * @param offset DWORD-aligned result byte offset.
 * @return Zero when staged or a negative SVGA error for lock/state/capacity.
 */
int svga_batch_query_end(struct svga_batch *batch, svga_handle context,
                         uint32_t type, svga_handle result, uint32_t offset)
{
    if (!batch || !context || !result || batch->submitted || !batch->metadata)
        return SVGA_EINVAL;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int cid = svga_find_context(context), ret = SVGA_EINVAL;
    if (batch->generation == svga.generation && cid >= 0 &&
        !svga.contexts[cid].gb.retiring)
        ret = query_batch_end_locked(batch, (uint32_t)cid, type, result, offset);
    svga_unlock(flags);
    return ret;
}

/**
 * @brief Stage a GB query WAIT matched to one pending or same-batch END.
 * @param batch Open batch whose source bound covers this operation.
 * @param context Live SVGA context handle.
 * @param type Supported GB query type.
 * @param result Fully mapped borrowed GMR holding the result.
 * @param offset DWORD-aligned result byte offset.
 * @return Zero when staged or a negative SVGA error for an unmatched result.
 */
int svga_batch_query_wait(struct svga_batch *batch, svga_handle context,
                          uint32_t type, svga_handle result, uint32_t offset)
{
    if (!batch || !context || !result || batch->submitted || !batch->metadata)
        return SVGA_EINVAL;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int cid = svga_find_context(context), ret = SVGA_EINVAL;
    if (batch->generation == svga.generation && cid >= 0 &&
        !svga.contexts[cid].gb.retiring)
        ret = query_batch_wait_locked(batch, (uint32_t)cid, type, result, offset);
    svga_unlock(flags);
    return ret;
}

/** @brief Validate query ordering and assign commit slots before FIFO append.
 * Caller holds svga_lock; this performs no host writes or persistent mutations.
 * @param batch Open batch whose query sidecar is checked.
 * @return Zero when all BEGIN/END/WAIT operations resolve, else a negative
 *         SVGA error before the batch's FIFO commit point.
 */
int svga_query_validate_batch_locked(struct svga_batch *batch)
{
    if (!batch) return SVGA_EINVAL;
    if (!batch->metadata) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_QUERY,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_QUERY_METADATA, 0, 1);
        return SVGA_EINVAL;
    }
    if (batch->query_op_count > batch->metadata->operation_capacity) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_QUERY,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_QUERY_METADATA,
            batch->query_op_count, batch->metadata->operation_capacity);
        return SVGA_EINVAL;
    }
    if (!batch->query_op_count) return 0;
    query_overlay_reset_locked(batch);
    struct svga_batch_metadata *metadata = batch->metadata;
    for (uint32_t i = 0; i < batch->query_op_count; ++i) {
        struct svga_batch_query_op *op = &metadata->query_ops[i];
        op->match_kind = 0;
        op->match_index = UINT32_MAX;
        op->commit_slot = UINT32_MAX;
        if (op->context >= SVGA_CONTEXTS) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                SVGA_BATCH_SUBMIT_DETAIL_QUERY_CONTEXT_RANGE,
                op->context, SVGA_CONTEXTS);
            return SVGA_EINVAL;
        }
        if (!svga.contexts[op->context].handle) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                SVGA_BATCH_SUBMIT_DETAIL_QUERY_CONTEXT_MISSING,
                op->context, 0);
            return SVGA_EINVAL;
        }
        if (op->kind == SVGA_QUERY_OP_BEGIN) {
            if (metadata->query_active[op->context]) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                    SVGA_BATCH_SUBMIT_DETAIL_QUERY_BEGIN_ACTIVE,
                    1, 0);
                return SVGA_EINVAL;
            }
            metadata->query_active[op->context] = true;
            continue;
        }
        if (op->kind == SVGA_QUERY_OP_END) {
            if (!metadata->query_active[op->context]) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_NOT_ACTIVE,
                    0, 1);
                return SVGA_EINVAL;
            }
            if (op->gmr >= svga.gmr_limit) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_GMR_RANGE,
                    op->gmr, svga.gmr_limit);
                return SVGA_EINVAL;
            }
            if (!svga.gmrs[op->gmr].borrowed) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_GMR_KIND,
                    0, 1);
                return SVGA_EINVAL;
            }
            if (svga.gmrs[op->gmr].retiring) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_GMR_RETIRING,
                    1, 0);
                return SVGA_EINVAL;
            }
            if (!svga.gmrs[op->gmr].mob_defined) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_GMR_MOB,
                    0, 1);
                return SVGA_EINVAL;
            }
            if (svga.gmrs[op->gmr].map_inflight) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_GMR_MAP,
                    1, 0);
                return SVGA_EINVAL;
            }
            if (svga.gmrs[op->gmr].mapped_pages != svga.gmrs[op->gmr].pages) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_GMR_PAGES,
                    svga.gmrs[op->gmr].mapped_pages,
                    svga.gmrs[op->gmr].pages);
                return SVGA_EINVAL;
            }
            if (op->offset > svga.gmrs[op->gmr].bytes) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_OFFSET,
                    op->offset, svga.gmrs[op->gmr].bytes);
                return SVGA_EINVAL;
            }
            if (SVGA_GB_QUERY_RESULT_BYTES >
                svga.gmrs[op->gmr].bytes - op->offset) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_RESULT_RANGE,
                    svga.gmrs[op->gmr].bytes - op->offset,
                    SVGA_GB_QUERY_RESULT_BYTES);
                return SVGA_EINVAL;
            }
            if (metadata->query_staged_end_count >= metadata->query_free_slots)
            {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_CAPACITY,
                    metadata->query_staged_end_count,
                    metadata->query_free_slots);
                return SVGA_ENOMEM;
            }
            if (metadata->query_end_count >= SVGA_QUERY_PENDING_MAX) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_PENDING_CAPACITY,
                    metadata->query_end_count, SVGA_QUERY_PENDING_MAX);
                return SVGA_ENOMEM;
            }
            metadata->query_active[op->context] = false;
            struct svga_batch_query_key *key = query_key_get(metadata,
                op->context, op->gmr, op->offset, true);
            if (!key) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_KEY,
                    op->context, op->gmr);
                return SVGA_ENOMEM;
            }
            if (key->staged_count == UINT16_MAX) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                    SVGA_BATCH_SUBMIT_DETAIL_QUERY_END_STAGED_COUNT,
                    key->staged_count, UINT16_MAX);
                return SVGA_ENOMEM;
            }
            uint16_t ordinal = (uint16_t)metadata->query_end_count++;
            metadata->query_end_indices[ordinal] = i;
            query_staged_append(metadata, key, ordinal);
            op->commit_slot = metadata->query_free_slot_indices[
                metadata->query_staged_end_count];
            ++metadata->query_staged_end_count;
            continue;
        }
        if (op->kind != SVGA_QUERY_OP_WAIT) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                SVGA_BATCH_SUBMIT_DETAIL_QUERY_KIND,
                op->kind, SVGA_QUERY_OP_WAIT);
            return SVGA_EINVAL;
        }
        if (op->gmr >= svga.gmr_limit) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_GMR_RANGE,
                op->gmr, svga.gmr_limit);
            return SVGA_EINVAL;
        }
        if (!svga.gmrs[op->gmr].borrowed) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_GMR_KIND,
                0, 1);
            return SVGA_EINVAL;
        }
        if (svga.gmrs[op->gmr].retiring) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_GMR_RETIRING,
                1, 0);
            return SVGA_EINVAL;
        }
        if (!svga.gmrs[op->gmr].mob_defined) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_GMR_MOB,
                0, 1);
            return SVGA_EINVAL;
        }
        if (svga.gmrs[op->gmr].map_inflight) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_GMR_MAP,
                1, 0);
            return SVGA_EINVAL;
        }
        if (svga.gmrs[op->gmr].mapped_pages != svga.gmrs[op->gmr].pages) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_GMR_PAGES,
                svga.gmrs[op->gmr].mapped_pages,
                svga.gmrs[op->gmr].pages);
            return SVGA_EINVAL;
        }
        if (op->offset > svga.gmrs[op->gmr].bytes) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_OFFSET,
                op->offset, svga.gmrs[op->gmr].bytes);
            return SVGA_EINVAL;
        }
        if (SVGA_GB_QUERY_RESULT_BYTES >
            svga.gmrs[op->gmr].bytes - op->offset) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_RESULT_RANGE,
                svga.gmrs[op->gmr].bytes - op->offset,
                SVGA_GB_QUERY_RESULT_BYTES);
            return SVGA_EINVAL;
        }
        struct svga_batch_query_key *key = query_key_get(metadata,
            op->context, op->gmr, op->offset, false);
        if (!key) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_KEY,
                op->context, op->gmr);
            return SVGA_EINVAL;
        }
        if (key->pending_count) {
            uint16_t pending = query_pending_pop(metadata, key);
            if (pending != UINT16_MAX) {
                op->match_kind = 1;
                op->match_index = pending;
            }
        }
        if (!op->match_kind && key->staged_count) {
            uint16_t ordinal = query_staged_pop(metadata, key);
            if (ordinal != UINT16_MAX) {
                op->match_kind = 2;
                op->match_index = metadata->query_end_indices[ordinal];
            }
        }
        if (!op->match_kind) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_QUERY, i,
                SVGA_BATCH_SUBMIT_DETAIL_QUERY_WAIT_NO_MATCH,
                op->context, op->gmr);
            return SVGA_EINVAL;
        }
    }
    return 0;
}

/** @brief Publish validated query state after the FIFO body and fence commit.
 * Caller holds svga_lock.
 * @param batch Submitted batch with validated query matches and commit slots.
 * @param fence Real FIFO fence covering all query operations.
 */
void svga_query_commit_batch_locked(const struct svga_batch *batch,
                                    svga_handle fence)
{
    if (!batch || !batch->metadata) return;
    struct svga_batch_metadata *metadata = batch->metadata;
    for (uint32_t i = 0; i < batch->query_op_count; ++i) {
        struct svga_batch_query_op *op = &metadata->query_ops[i];
        struct svga_context *context = &svga.contexts[op->context];
        if (op->kind == SVGA_QUERY_OP_BEGIN) {
            context->query_active = true;
            continue;
        }
        if (op->kind == SVGA_QUERY_OP_END) {
            context->query_active = false;
            context->query_attachment = op->gmr;
            uint32_t slot = op->commit_slot;
            if (slot < SVGA_QUERY_PENDING_MAX)
                svga.queries[slot] = (struct svga_query_pending){
                    .context = op->context, .gmr = op->gmr, .offset = op->offset,
                    .end_fence = fence, .used = true};
            continue;
        }
        uint32_t slot = op->match_kind == 1 ? op->match_index :
                        metadata->query_ops[op->match_index].commit_slot;
        if (slot < SVGA_QUERY_PENDING_MAX && svga.queries[slot].used)
            svga.queries[slot].wait_fence = fence;
    }
}

/** @brief Queue one WAIT barrier before releasing a pending query attachment.
 * Caller holds svga_lock; completion remains tracked by the returned fence.
 * @param query Live unmatched query record to retire.
 * @return SVGA_EBUSY while the barrier is queued, zero is not returned until
 *         later cleanup, or a negative SVGA error before publication.
 */
static int query_submit_retirement_barrier_locked(struct svga_query_pending *query)
{
    if (!query || !query->used || query->wait_fence ||
        query->context >= SVGA_CONTEXTS || query->gmr >= svga.gmr_limit)
        return SVGA_EINVAL;
    struct svga_context *context = &svga.contexts[query->context];
    struct svga_gmr *gmr = &svga.gmrs[query->gmr];
    if (!context->handle || context->query_active || !gmr->borrowed ||
        gmr->retiring || !gmr->mob_defined || gmr->map_inflight ||
        gmr->mapped_pages != gmr->pages)
        return SVGA_EBUSY;

    SVGA3dCmdWaitForGBQuery command = {query->context,
        SVGA_GB_QUERY_TYPE_OCCLUSION, gmr->mobid, query->offset};
    uint32_t words[6] = {SVGA_3D_CMD_WAIT_FOR_GB_QUERY, sizeof(command)};
    __builtin_memcpy(words + 2, &command, sizeof(command));
    svga_handle fence = 0;
    int ret = svga_fifo_batch_try_locked(words, sizeof(words), &fence);
    if (ret) return ret;
    query->wait_fence = fence;
    context->last_use_fence = fence;
    gmr->last_use_fence = fence;
    return SVGA_EBUSY;
}

/** @brief Ensure pending query results stop referencing a GMR before retirement.
 * Caller holds svga_lock.
 * @param gmr GMR table index being retired.
 * @return Zero when unused, SVGA_EBUSY while a query barrier remains, or a
 *         negative SVGA error before the GMR can be released.
 */
int svga_query_prepare_gmr_retire_locked(uint32_t gmr)
{
    if (gmr >= svga.gmr_limit) return SVGA_EINVAL;
    query_reap_locked();
    for (uint32_t i = 0; i < SVGA_QUERY_PENDING_MAX; ++i) {
        struct svga_query_pending *query = &svga.queries[i];
        if (!query->used || query->gmr != gmr) continue;
        if (query->wait_fence) return SVGA_EBUSY;
        return query_submit_retirement_barrier_locked(query);
    }
    return 0;
}

/** @brief Ensure a context has no active or unconsumed query before retirement.
 * Caller holds svga_lock.
 * @param context Context table index being retired.
 * @return Zero when safe, SVGA_EBUSY while a query is active/pending, or a
 *         negative SVGA error before the context can be released.
 */
int svga_query_prepare_context_retire_locked(uint32_t context)
{
    if (context >= SVGA_CONTEXTS || !svga.contexts[context].handle)
        return SVGA_EINVAL;
    query_reap_locked();
    if (svga.contexts[context].query_active) return SVGA_EBUSY;
    for (uint32_t i = 0; i < SVGA_QUERY_PENDING_MAX; ++i) {
        struct svga_query_pending *query = &svga.queries[i];
        if (!query->used || query->context != context) continue;
        if (query->wait_fence) return SVGA_EBUSY;
        return query_submit_retirement_barrier_locked(query);
    }
    return 0;
}

/** @brief Check whether a GMR is referenced by any live query record.
 * Caller holds svga_lock.
 * @param gmr GMR table index to inspect.
 * @return True when an active or pending query retains the GMR.
 */
bool svga_query_gmr_in_use_locked(uint32_t gmr)
{
    query_reap_locked();
    for (uint32_t i = 0; i < SVGA_QUERY_PENDING_MAX; ++i)
        if (svga.queries[i].used && svga.queries[i].gmr == gmr) return true;
    return false;
}

/** @brief Check whether a context has active or pending query work.
 * Caller holds svga_lock.
 * @param context Context table index to inspect.
 * @return True when query work prevents context teardown.
 */
bool svga_query_context_busy_locked(uint32_t context)
{
    query_reap_locked();
    if (context >= SVGA_CONTEXTS || svga.contexts[context].query_active)
        return true;
    for (uint32_t i = 0; i < SVGA_QUERY_PENDING_MAX; ++i)
        if (svga.queries[i].used && svga.queries[i].context == context)
            return true;
    return false;
}
