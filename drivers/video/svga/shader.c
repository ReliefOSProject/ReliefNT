#include "device.h"
#include <reliefnt/heap.h>

/* Protected by svga_lock; a one-record reap resumes after the last reclaimed
 * slot so a ready low slot cannot starve older high slots. */
static uint32_t shader_reap_cursor;

#define SVGA_GB_SHADER_MAX_CODE_BYTES (8u * 1024u * 1024u)

/** @brief Find a live or retiring VGPU9 shader key while svga_lock is held.
 * @param context Context handle that owns the shader.
 * @param local_id VGPU9-local shader identifier.
 * @param type Vertex or pixel shader type.
 * @return Host slot index, preferring a live version, or -1 when absent.
 */
static int shader_key_locked(svga_handle context, uint32_t local_id,
                             uint32_t type)
{
    int retiring = -1;
    for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i) {
        const struct svga_shader *shader = &svga.shaders[i];
        if (shader->defined && shader->context == context &&
            shader->local_id == local_id && shader->type == type) {
            if (!shader->retiring) return (int)i;
            if (retiring < 0) retiring = (int)i;
        }
    }
    return retiring;
}

/** @brief Resolve only executable shader keys while svga_lock is held.
 * @param context Context handle that owns the shader.
 * @param local_id VGPU9-local shader identifier.
 * @param type Vertex or pixel shader type.
 * @return Live host slot index, or -1 when no live version exists.
 */
static int shader_key_live_locked(svga_handle context, uint32_t local_id,
                                  uint32_t type)
{
    int slot = shader_key_locked(context, local_id, type);
    return slot >= 0 && !svga.shaders[slot].retiring ? slot : -1;
}

/** @brief Count page-table backing pages for a shader MOB.
 * @param pages Number of shader-code pages.
 * @return Root and leaf table pages needed by the GB page-table format.
 */
static uint32_t shader_backing_table_pages(uint32_t pages)
{
    return pages <= 1 ? 0 : pages <= 1024 ? 1 :
           1 + (pages + 1023u) / 1024u;
}

/** @brief Zero a page-aligned allocation through its captured allocator.
 * @param ops Allocator snapshot that owns the physical range.
 * @param phys Page-aligned physical base returned by that allocator.
 * @param pages Number of pages to clear.
 */
static void shader_zero_pages(const struct svga_ops *ops, uint64_t phys,
                              uint32_t pages)
{
    uint32_t *words = ops->pointer(phys);
    for (uint32_t i = 0; i < pages * 1024u; ++i) words[i] = 0;
}

/** @brief Allocate and initialize shader code and page-table backing unlocked.
 * @param ops Captured allocator callbacks owned by the staging transaction.
 * @param bytes Shader bytecode length in bytes.
 * @param mob Writable output describing the allocated backing.
 * @return Zero on success or SVGA_ENOMEM if any allocation or mapping fails.
 */
static int shader_backing_alloc(const struct svga_ops *ops, uint32_t bytes,
                                struct svga_mob *mob)
{
    if (!ops || !ops->alloc || !ops->free || !ops->pointer || !bytes || !mob)
        return SVGA_ENOMEM;
    uint32_t pages = (bytes + 4095u) / 4096u;
    uint32_t tables = shader_backing_table_pages(pages);
    uint64_t phys = ops->alloc(pages);
    if (!phys) return SVGA_ENOMEM;
    if ((phys & 4095u) || (phys >> 12) > UINT32_MAX - pages ||
        !ops->pointer(phys)) {
        ops->free(phys, pages);
        return SVGA_ENOMEM;
    }
    shader_zero_pages(ops, phys, pages);
    uint64_t pt_phys = tables ? ops->alloc(tables) : 0;
    if (tables && (!pt_phys || (pt_phys & 4095u) ||
                   (pt_phys >> 12) > UINT32_MAX - tables ||
                   !ops->pointer(pt_phys))) {
        if (pt_phys) ops->free(pt_phys, tables);
        ops->free(phys, pages);
        return SVGA_ENOMEM;
    }
    if (tables) shader_zero_pages(ops, pt_phys, tables);
    *mob = (struct svga_mob){.phys = phys, .pt_phys = pt_phys,
        .pages = pages, .pt_pages = tables, .bytes = bytes,
        .depth = tables > 1 ? SVGA3D_MOBFMT_PT_2 :
                 tables ? SVGA3D_MOBFMT_PT_1 : SVGA3D_MOBFMT_PT_0};
    if (tables) {
        uint32_t *pt = ops->pointer(pt_phys);
        uint32_t *leaves = tables > 1 ? pt + 1024 : pt;
        if (tables > 1)
            for (uint32_t i = 0; i < tables - 1; ++i)
                pt[i] = (uint32_t)(pt_phys >> 12) + i + 1;
        for (uint32_t i = 0; i < pages; ++i)
            leaves[i] = (uint32_t)(phys >> 12) + i;
    }
    return 0;
}

/** @brief Release shader backing with the allocator that created it.
 * @param ops Allocator snapshot stored with the backing owner.
 * @param mob Backing to free; cleared before return.
 */
void svga_shader_backing_free(const struct svga_ops *ops, struct svga_mob *mob)
{
    if (!ops || !mob) return;
    if (mob->pt_phys && ops->free) ops->free(mob->pt_phys, mob->pt_pages);
    if (mob->phys && !mob->borrowed && ops->free) ops->free(mob->phys, mob->pages);
    *mob = (struct svga_mob){0};
}

/** @brief Encode one legacy 3D command into a caller-owned batch word array.
 * @param words Destination for the two-word command header and payload.
 * @param id Legacy SVGA3D command identifier.
 * @param body Trusted DWORD-aligned command body.
 * @param bytes Body length in bytes.
 */
static void shader_packet(uint32_t *words, uint32_t id, const void *body,
                          uint32_t bytes)
{
    words[0] = id;
    words[1] = bytes;
    for (uint32_t i = 0; i < bytes / 4; ++i)
        __builtin_memcpy(&words[2 + i], (const uint8_t *)body + i * 4, 4);
}

/**
 * @brief Define a shader immediately and publish its host objects with a fence.
 * @param context Live context handle that owns the shader.
 * @param local_id VGPU9-local shader identifier.
 * @param type Vertex or pixel shader type.
 * @param bytecode Trusted readable kernel bytecode.
 * @param bytes DWORD-aligned bytecode length.
 * @return Zero when published, SVGA_EBUSY for lock/FIFO pressure, or a negative
 *         SVGA error when the key, host slot, or backing allocation is invalid.
 */
int svga3d_shader_define(svga_handle context, uint32_t local_id, uint32_t type,
                         const void *bytecode, uint32_t bytes)
{
    if (!context || local_id == UINT32_MAX || local_id >= 5000u ||
        (type != SVGA3D_SHADERTYPE_VS && type != SVGA3D_SHADERTYPE_PS) ||
        !bytecode || bytes < 4 || (bytes & 3u))
        return SVGA_EINVAL;

    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int ret = SVGA_ENODEV;
    int cid = svga_find_context(context);
    if (!svga.available || !svga.fifo_ready || !svga.gb_active ||
        cid < 0 || svga.contexts[cid].gb.retiring ||
        svga.contexts[cid].batch_state_owner)
        goto done;
    if (bytes > SVGA_GB_SHADER_MAX_CODE_BYTES || bytes > svga.mob_max_bytes) {
        ret = SVGA_ENOMEM;
        goto done;
    }
    if (shader_key_locked(context, local_id, type) >= 0) {
        ret = SVGA_EINVAL;
        goto done;
    }

    uint32_t slot;
    for (slot = 0; slot < SVGA_GB_SHADER_SLOTS &&
         (svga.shaders[slot].defined || svga.shaders[slot].reserved); ++slot) {}
    if (slot == SVGA_GB_SHADER_SLOTS) {
        ret = SVGA_ENOMEM;
        goto done;
    }
    const uint32_t mobid = SVGA_SHADER_MOB_BASE + slot;
    struct svga_mob code = {0};
    ret = svga_gb_backing_alloc_locked(&code, bytes);
    if (ret) goto done;
    uint8_t *destination = svga.ops.pointer(code.phys);
    if (!destination) {
        svga_gb_backing_free_locked(&code);
        ret = SVGA_ENOMEM;
        goto done;
    }
    const uint8_t *source = bytecode;
    for (uint32_t i = 0; i < bytes; ++i) destination[i] = source[i];
    svga_barrier();

    uint32_t words[18] = {0};
    SVGA3dCmdDefineGBMob mob = {mobid, code.depth,
        svga_gb_backing_root(&code), bytes};
    SVGA3dCmdDefineGBShader define = {slot, type, bytes};
    SVGA3dCmdBindGBShader bind = {slot, mobid, 0};
    shader_packet(words, SVGA_3D_CMD_DEFINE_GB_MOB, &mob, sizeof(mob));
    shader_packet(words + 6, SVGA_3D_CMD_DEFINE_GB_SHADER, &define, sizeof(define));
    shader_packet(words + 11, SVGA_3D_CMD_BIND_GB_SHADER, &bind, sizeof(bind));
    svga_handle fence = 0;
    ret = svga_fifo_batch_try_locked(words, 16u * 4u, &fence);
    if (ret) {
        svga_gb_backing_free_locked(&code);
        goto done;
    }
    code.defined = true;
    uint32_t serial = ++svga.handle_serial;
    if (!serial) serial = ++svga.handle_serial;
    svga.shaders[slot] = (struct svga_shader){.context = context,
        .ready_fence = fence, .local_id = local_id, .host_id = slot,
        .mobid = mobid, .type = type, .bytes = bytes, .serial = serial,
        .code = code, .allocator = svga.ops, .defined = true};
    ret = 0;
done:
    svga_unlock(flags);
    return ret;
}

/**
 * @brief Unbind and destroy an immediate shader, retaining backing to its fence.
 * @param context Live context handle that owns the shader.
 * @param local_id VGPU9-local shader identifier.
 * @param type Vertex or pixel shader type.
 * @return SVGA_EBUSY while destruction is queued or its fence is pending, zero
 *         after retirement, or a negative SVGA error for an absent shader.
 */
int svga3d_shader_destroy(svga_handle context, uint32_t local_id, uint32_t type)
{
    if (!context || local_id == UINT32_MAX ||
        (type != SVGA3D_SHADERTYPE_VS && type != SVGA3D_SHADERTYPE_PS))
        return SVGA_EINVAL;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int ret = SVGA_EINVAL;
    int slot = shader_key_locked(context, local_id, type);
    if (slot < 0) goto done;
    struct svga_shader *shader = &svga.shaders[slot];
    int cid = svga_find_context(context);
    if (cid >= 0 && svga.contexts[cid].batch_state_owner) {
        ret = SVGA_EBUSY;
        goto done;
    }
    if (shader->retiring) {
        if (!svga_fence_reached_locked(shader->retire_fence)) {
            ret = SVGA_EBUSY;
            goto done;
        }
        struct svga_mob code = shader->code;
        struct svga_ops allocator = shader->allocator;
        uint32_t pages = code.pages + code.pt_pages;
        if (svga.pages >= pages) svga.pages -= pages;
        else svga.pages = 0;
        *shader = (struct svga_shader){0};
        ret = 0;
        svga_unlock(flags);
        svga_shader_backing_free(&allocator, &code);
        return ret;
    }
    if (cid < 0) goto done;
    bool bound = svga.contexts[cid].shaders[type - SVGA3D_SHADERTYPE_VS] ==
                 (uint32_t)slot;
    uint32_t words[20] = {0}, used = 0;
    if (bound) {
        SVGA3dCmdSetShader unbind_context = {(uint32_t)cid, type, SVGA3D_INVALID_ID};
        shader_packet(words + used, SVGA_3D_CMD_SET_SHADER, &unbind_context,
                      sizeof(unbind_context));
        used += 5;
    }
    SVGA3dCmdBindGBShader unbind_code = {shader->host_id, SVGA3D_INVALID_ID, 0};
    SVGA3dCmdDestroyGBShader destroy_shader = {shader->host_id};
    shader_packet(words + used, SVGA_3D_CMD_BIND_GB_SHADER, &unbind_code,
                  sizeof(unbind_code));
    used += 5;
    shader_packet(words + used, SVGA_3D_CMD_DESTROY_GB_SHADER, &destroy_shader,
                  sizeof(destroy_shader));
    used += 3;
    shader_packet(words + used, SVGA_3D_CMD_DESTROY_GB_MOB, &shader->mobid,
                  sizeof(shader->mobid));
    used += 3;
    svga_handle fence = 0;
    ret = svga_fifo_batch_try_locked(words, used * 4u, &fence);
    if (ret) goto done;
    shader->retiring = true;
    shader->retire_fence = fence;
    if (bound) svga.contexts[cid].shaders[type - SVGA3D_SHADERTYPE_VS] = UINT32_MAX;
    ret = SVGA_EBUSY;
done:
    svga_unlock(flags);
    return ret;
}

/** @brief Append a packet assembled from a fixed prefix and variable tail.
 * @param batch Open output batch; owns the destination words.
 * @param command Legacy SVGA3D command identifier.
 * @param prefix Fixed, DWORD-aligned command fields.
 * @param prefix_bytes Prefix length in bytes.
 * @param tail Optional DWORD-aligned inline data.
 * @param tail_bytes Tail length in bytes.
 * @return Zero on append, SVGA_ENOMEM when translated output capacity is short,
 *         or SVGA_EINVAL for invalid batch/payload state.
 */
static int batch_append_two_parts(struct svga_batch *batch, uint32_t command,
                                  const void *prefix, uint32_t prefix_bytes,
                                  const void *tail, uint32_t tail_bytes)
{
    if (!batch || !batch->words || batch->submitted || !prefix ||
        (prefix_bytes & 3u) || (tail_bytes & 3u) || (tail_bytes && !tail) ||
        !batch->metadata || batch->capacity > SVGA_BATCH_MAX_OUTPUT_BYTES ||
        batch->bytes > batch->capacity)
        return SVGA_EINVAL;
    uint32_t payload = prefix_bytes + tail_bytes;
    uint32_t packet_bytes = payload + 8u;
    if (packet_bytes < payload || packet_bytes > batch->capacity - batch->bytes)
        return SVGA_ENOMEM;
    uint32_t *words = batch->words + batch->bytes / 4;
    words[0] = command;
    words[1] = payload;
    for (uint32_t i = 0; i < prefix_bytes / 4; ++i)
        __builtin_memcpy(&words[2 + i], (const uint8_t *)prefix + i * 4, 4);
    uint32_t tail_start = 2 + prefix_bytes / 4;
    for (uint32_t i = 0; i < tail_bytes / 4; ++i)
        __builtin_memcpy(&words[tail_start + i], (const uint8_t *)tail + i * 4, 4);
    batch->bytes += packet_bytes;
    return 0;
}

/** @brief Hash a context-scoped VGPU9 shader key to the fixed overlay table.
 * @param context Context handle included in the key.
 * @param local_id VGPU9-local shader identifier.
 * @param type Vertex or pixel shader type.
 * @return Masked initial index in the batch overlay.
 */
static uint32_t shader_batch_key_hash(svga_handle context, uint32_t local_id,
                                      uint32_t type)
{
    uint64_t value = context ^ ((uint64_t)local_id * 0x9e3779b185ebca87ULL) ^
                     ((uint64_t)type * 0xc2b2ae3d27d4eb4fULL);
    value ^= value >> 32;
    value ^= value >> 16;
    return (uint32_t)value & (SVGA_BATCH_SHADER_KEY_CAPACITY - 1u);
}

/** @brief Lookup or seed a transaction-local shader-key overlay entry.
 * @param batch Open batch whose sidecar owns the overlay.
 * @param context Context handle in the shader key.
 * @param local_id VGPU9-local shader identifier.
 * @param type Vertex or pixel shader type.
 * @param create True to snapshot the committed key when absent from the overlay.
 * @return Overlay entry, or NULL when the bounded table is full or the key is absent.
 */
static struct svga_batch_shader_key *shader_batch_key_locked(
    struct svga_batch *batch, svga_handle context, uint32_t local_id,
    uint32_t type, bool create)
{
    struct svga_batch_metadata *metadata = batch->metadata;
    uint32_t first = shader_batch_key_hash(context, local_id, type);
    for (uint32_t probe = 0; probe < SVGA_BATCH_SHADER_KEY_CAPACITY; ++probe) {
        struct svga_batch_shader_key *key = &metadata->shader_keys[
            (first + probe) & (SVGA_BATCH_SHADER_KEY_CAPACITY - 1u)];
        if (!key->used) {
            if (!create) return NULL;
            if (metadata->shader_key_count >= SVGA_BATCH_SHADER_KEY_CAPACITY)
                return NULL;
            int slot = shader_key_live_locked(context, local_id, type);
            *key = (struct svga_batch_shader_key){.context = context,
                .local_id = local_id, .type = type,
                .slot = slot < 0 ? UINT32_MAX : (uint32_t)slot, .used = true};
            ++metadata->shader_key_count;
            return key;
        }
        if (key->context == context && key->local_id == local_id &&
            key->type == type)
            return key;
    }
    return NULL;
}

/** @brief Lookup or initialize a transaction-local context binding snapshot.
 * @param batch Open batch whose sidecar owns the binding overlay.
 * @param cid Live context table index protected by svga_lock.
 * @return Binding overlay entry, or NULL for an out-of-range context.
 */
static struct svga_batch_shader_binding *shader_batch_binding_locked(
    struct svga_batch *batch, uint32_t cid)
{
    struct svga_batch_metadata *metadata = batch->metadata;
    if (cid >= SVGA_CONTEXTS) return NULL;
    struct svga_batch_shader_binding *binding =
        &metadata->shader_bindings[cid];
    if (!binding->used) {
        binding->context = svga.contexts[cid].handle;
        binding->slots[0] = svga.contexts[cid].shaders[0];
        binding->slots[1] = svga.contexts[cid].shaders[1];
        binding->used = true;
    }
    return binding;
}

/** @brief Reserve one context's shader mutations for a batch transaction.
 * @param batch Open batch whose transaction ID is assigned on first use.
 * @param cid Live context table index protected by svga_lock.
 * @return Zero on reservation, SVGA_EBUSY for another transaction owner, or
 *         SVGA_ENOMEM if typed context-reference capacity is exhausted.
 */
static int shader_batch_reserve_context_locked(struct svga_batch *batch,
                                               uint32_t cid)
{
    int ret = svga_batch_reserve_context_locked(batch, cid);
    if (ret) return ret;
    (void)shader_batch_binding_locked(batch, cid);
    return 0;
}

/** @brief Check source-derived shader-event capacity for an open batch.
 * @param batch Candidate caller-owned batch.
 * @return True when another shader event can be recorded in its sidecar.
 */
static bool shader_batch_has_capacity(const struct svga_batch *batch)
{
    return batch && batch->metadata && !batch->submitted &&
           batch->operation_count < batch->metadata->operation_capacity &&
           batch->shader_op_count < batch->metadata->operation_capacity;
}

/** @brief Remove one staged host slot from the transaction reservation list.
 * @param metadata Batch sidecar that owns the reserved-slot list.
 * @param slot Host shader slot to remove.
 */
static void shader_remove_reserved_slot(struct svga_batch_metadata *metadata,
                                       uint32_t slot)
{
    for (uint32_t i = 0; i < metadata->reserved_shader_count; ++i) {
        if (metadata->reserved_shader_slots[i] != slot) continue;
        metadata->reserved_shader_slots[i] =
            metadata->reserved_shader_slots[--metadata->reserved_shader_count];
        return;
    }
}

/** @brief Release an uncommitted host slot and its page-budget reservation.
 * @param batch Batch transaction that owns the reservation.
 * @param slot Host shader slot to clear while svga_lock is held.
 */
static void shader_release_reservation_locked(struct svga_batch *batch,
                                              uint32_t slot)
{
    struct svga_batch_metadata *metadata = batch->metadata;
    if (slot >= SVGA_GB_SHADER_SLOTS) return;
    struct svga_shader *shader = &svga.shaders[slot];
    if (!shader->reserved || shader->reserved_by != metadata->transaction_id)
        return;
    if (svga.shader_reserved_pages >= shader->reserved_pages)
        svga.shader_reserved_pages -= shader->reserved_pages;
    else svga.shader_reserved_pages = 0;
    if (metadata->reserved_pages >= shader->reserved_pages)
        metadata->reserved_pages -= shader->reserved_pages;
    else metadata->reserved_pages = 0;
    *shader = (struct svga_shader){0};
    shader_remove_reserved_slot(metadata, slot);
}

/** @brief Allocate backing and copy trusted shader bytes without svga_lock.
 * @param ops Captured allocator callbacks reserved by the batch.
 * @param code Writable output backing owner.
 * @param bytecode Trusted readable bytecode snapshot.
 * @param bytes Bytecode length in bytes.
 * @return Zero on success or SVGA_ENOMEM if backing or pointer mapping fails.
 */
static int shader_copy_bytecode(const struct svga_ops *ops,
                                struct svga_mob *code, const void *bytecode,
                                uint32_t bytes)
{
    int ret = shader_backing_alloc(ops, bytes, code);
    if (ret) return ret;
    uint8_t *destination = ops->pointer(code->phys);
    if (!destination) {
        svga_shader_backing_free(ops, code);
        return SVGA_ENOMEM;
    }
    const uint8_t *source = bytecode;
    for (uint32_t i = 0; i < bytes; ++i) destination[i] = source[i];
    svga_barrier();
    return 0;
}

/**
 * @brief Stage an ordered VGPU9 shader bind or unbind in a trusted batch.
 * @param batch Open batch that owns translated packets and operation metadata.
 * @param context Live context reserved until the batch commits or aborts.
 * @param type Vertex or pixel shader type.
 * @param local_id VGPU9-local key, or SVGA3D_INVALID_ID to unbind.
 * @return Zero when staged, SVGA_EBUSY for lock contention, SVGA_ENOMEM for
 *         metadata/output exhaustion, or SVGA_EINVAL for an unresolved key.
 */
int svga_batch_bind_shader(struct svga_batch *batch, svga_handle context,
                           uint32_t type, uint32_t local_id)
{
    if (!batch || !batch->metadata || batch->submitted) return SVGA_EINVAL;
    if (!shader_batch_has_capacity(batch)) return SVGA_ENOMEM;
    if (!context ||
        (type != SVGA3D_SHADERTYPE_VS && type != SVGA3D_SHADERTYPE_PS))
        return SVGA_EINVAL;
    int reserve = svga_batch_operation_reserve(batch,
        SVGA_BATCH_OPERATION_ARRAY_SHADER, batch->shader_op_count + 1u);
    if (reserve) return reserve;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int ret = SVGA_EINVAL;
    int cid = svga_find_context(context);
    if (batch->generation != svga.generation || cid < 0 ||
        svga.contexts[cid].gb.retiring) goto done;
    ret = shader_batch_reserve_context_locked(batch, (uint32_t)cid);
    if (ret) goto done;
    struct svga_batch_metadata *metadata = batch->metadata;
    uint32_t slot = UINT32_MAX;
    if (local_id != SVGA3D_INVALID_ID) {
        struct svga_batch_shader_key *key = shader_batch_key_locked(
            batch, context, local_id, type, false);
        if (!key) {
            int live = shader_key_live_locked(context, local_id, type);
            if (live < 0) { ret = SVGA_EINVAL; goto done; }
            key = shader_batch_key_locked(batch, context, local_id, type, true);
        }
        if (!key || key->slot >= SVGA_GB_SHADER_SLOTS) {
            ret = SVGA_EINVAL;
            goto done;
        }
        slot = key->slot;
        const struct svga_shader *shader = &svga.shaders[slot];
        if (shader->retiring || shader->context != context ||
            shader->type != type ||
            (shader->reserved && shader->reserved_by != metadata->transaction_id)) {
            ret = SVGA_EINVAL;
            goto done;
        }
    }
    struct svga_batch_shader_binding *binding =
        shader_batch_binding_locked(batch, (uint32_t)cid);
    if (!binding) { ret = SVGA_EINVAL; goto done; }
    SVGA3dCmdSetShader command = {(uint32_t)cid, type,
        slot == UINT32_MAX ? SVGA3D_INVALID_ID : slot};
    uint32_t old_context_count = batch->context_count;
    ret = svga_batch_retain_context(batch, context);
    if (!ret) ret = svga_batch_append(batch, SVGA_3D_CMD_SET_SHADER, true,
                                      &command, sizeof(command));
    if (ret) {
        svga_batch_rollback_references(batch, old_context_count,
            batch->surface_count, batch->gmr_count);
        goto done;
    }
    metadata->shader_ops[batch->shader_op_count++] =
        (struct svga_batch_shader_op){.context = context,
            .kind = SVGA_BATCH_SHADER_BIND,
            .local_id = local_id == SVGA3D_INVALID_ID ? UINT32_MAX : local_id,
            .type = type, .slot = slot};
    ++batch->operation_count;
    binding->slots[type - SVGA3D_SHADERTYPE_VS] = slot;
    ret = 0;
done:
    svga_unlock(flags);
    return ret;
}

/**
 * @brief Stage an ordered shader define with copied bytecode and reserved backing.
 * @param batch Open batch owning the sidecar and translated packets.
 * @param context Live context reserved until commit or abort.
 * @param local_id VGPU9-local shader identifier.
 * @param type Vertex or pixel shader type.
 * @param bytecode Trusted readable kernel bytecode copied outside svga_lock.
 * @param bytes DWORD-aligned bytecode length within host and page limits.
 * @return Zero when staged, SVGA_EBUSY on lock contention, SVGA_ENOMEM for
 *         resource/capacity exhaustion, or SVGA_EINVAL for invalid input/key.
 */
int svga_batch_shader_define(struct svga_batch *batch, svga_handle context,
                             uint32_t local_id, uint32_t type,
                             const void *bytecode, uint32_t bytes)
{
    if (!batch || !batch->metadata || batch->submitted) return SVGA_EINVAL;
    if (!shader_batch_has_capacity(batch)) return SVGA_ENOMEM;
    if (!context ||
        local_id == UINT32_MAX || local_id >= 5000u ||
        (type != SVGA3D_SHADERTYPE_VS && type != SVGA3D_SHADERTYPE_PS) ||
        !bytecode || bytes < 4 || (bytes & 3u))
        return SVGA_EINVAL;
    int reserve = svga_batch_operation_reserve(batch,
        SVGA_BATCH_OPERATION_ARRAY_SHADER, batch->shader_op_count + 1u);
    if (reserve) return reserve;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int ret = SVGA_EINVAL;
    int cid = svga_find_context(context);
    if (batch->generation != svga.generation || cid < 0 ||
        !svga.available || !svga.fifo_ready || !svga.gb_active ||
        svga.contexts[cid].gb.retiring) goto unlock_reserve;
    if (bytes > SVGA_GB_SHADER_MAX_CODE_BYTES || bytes > svga.mob_max_bytes) {
        ret = SVGA_ENOMEM;
        goto unlock_reserve;
    }
    uint32_t pages = (bytes + 4095u) / 4096u;
    uint32_t tables = shader_backing_table_pages(pages);
    uint32_t reserved_pages = pages + tables;
    if (svga.pages > svga.page_limit ||
        svga.shader_reserved_pages > svga.page_limit - svga.pages ||
        reserved_pages > svga.page_limit - svga.pages - svga.shader_reserved_pages) {
        ret = SVGA_ENOMEM;
        goto unlock_reserve;
    }
    ret = shader_batch_reserve_context_locked(batch, (uint32_t)cid);
    if (ret) goto unlock_reserve;
    struct svga_batch_shader_key *key = shader_batch_key_locked(
        batch, context, local_id, type, true);
    if (!key) { ret = SVGA_ENOMEM; goto unlock_reserve; }
    if (key->slot != UINT32_MAX) { ret = SVGA_EINVAL; goto unlock_reserve; }
    if (batch->metadata->reserved_shader_count >= SVGA_GB_SHADER_SLOTS) {
        ret = SVGA_ENOMEM;
        goto unlock_reserve;
    }
    uint32_t slot;
    for (slot = 0; slot < SVGA_GB_SHADER_SLOTS; ++slot)
        if (!svga.shaders[slot].defined && !svga.shaders[slot].reserved) break;
    if (slot == SVGA_GB_SHADER_SLOTS) { ret = SVGA_ENOMEM; goto unlock_reserve; }
    svga.shader_reserved_pages += reserved_pages;
    batch->metadata->reserved_pages += reserved_pages;
    batch->metadata->reserved_shader_slots[
        batch->metadata->reserved_shader_count++] = slot;
    svga.shaders[slot] = (struct svga_shader){.context = context,
        .local_id = local_id, .host_id = slot,
        .mobid = SVGA_SHADER_MOB_BASE + slot, .type = type, .bytes = bytes,
        .reserved_pages = reserved_pages,
        .reserved_by = batch->metadata->transaction_id,
        .allocator = svga.ops, .reserved = true};
    struct svga_ops allocator = svga.ops;
    svga_unlock(flags);

    struct svga_mob code = {0};
    ret = shader_copy_bytecode(&allocator, &code, bytecode, bytes);
    if (ret) {
        flags = svga_lock();
        shader_release_reservation_locked(batch, slot);
        svga_unlock(flags);
        return ret;
    }
    SVGA3dCmdDefineGBMob mob = {SVGA_SHADER_MOB_BASE + slot, code.depth,
        svga_gb_backing_root(&code), bytes};
    SVGA3dCmdDefineGBShader define = {slot, type, bytes};
    SVGA3dCmdBindGBShader bind = {slot, SVGA_SHADER_MOB_BASE + slot, 0};
    uint32_t old_bytes = batch->bytes;
    ret = svga_batch_append(batch, SVGA_3D_CMD_DEFINE_GB_MOB, true,
                            &mob, sizeof(mob));
    if (!ret) ret = svga_batch_append(batch, SVGA_3D_CMD_DEFINE_GB_SHADER, true,
                                      &define, sizeof(define));
    if (!ret) ret = svga_batch_append(batch, SVGA_3D_CMD_BIND_GB_SHADER, true,
                                      &bind, sizeof(bind));
    if (ret) {
        batch->bytes = old_bytes;
        svga_shader_backing_free(&allocator, &code);
        flags = svga_lock();
        shader_release_reservation_locked(batch, slot);
        svga_unlock(flags);
        return ret;
    }

    flags = svga_lock();
    struct svga_shader *shader = &svga.shaders[slot];
    if (!shader->reserved ||
        shader->reserved_by != batch->metadata->transaction_id ||
        batch->generation != svga.generation) {
        batch->bytes = old_bytes;
        svga_unlock(flags);
        svga_shader_backing_free(&allocator, &code);
        flags = svga_lock();
        shader_release_reservation_locked(batch, slot);
        svga_unlock(flags);
        return SVGA_EIO;
    }
    shader->code = code;
    key = shader_batch_key_locked(batch, context, local_id, type, false);
    if (!key) {
        batch->bytes = old_bytes;
        shader->code = (struct svga_mob){0};
        svga_unlock(flags);
        svga_shader_backing_free(&allocator, &code);
        flags = svga_lock();
        shader_release_reservation_locked(batch, slot);
        svga_unlock(flags);
        return SVGA_EIO;
    }
    key->slot = slot;
    batch->metadata->shader_ops[batch->shader_op_count++] =
        (struct svga_batch_shader_op){.context = context,
            .kind = SVGA_BATCH_SHADER_DEFINE, .local_id = local_id,
            .type = type, .slot = slot};
    ++batch->operation_count;
    svga_unlock(flags);
    return 0;

unlock_reserve:
    svga_unlock(flags);
    return ret;
}

/**
 * @brief Stage unbind and destruction of the current transaction-visible version.
 * @param batch Open batch owning ordered shader events.
 * @param context Live context reserved until commit or abort.
 * @param local_id VGPU9-local shader identifier resolved in stream order.
 * @param type Vertex or pixel shader type.
 * @return Zero when staged, SVGA_EBUSY on lock contention, SVGA_ENOMEM for
 *         capacity exhaustion, or SVGA_EINVAL when no visible version exists.
 */
int svga_batch_shader_destroy(struct svga_batch *batch, svga_handle context,
                              uint32_t local_id, uint32_t type)
{
    if (!batch || !batch->metadata || batch->submitted) return SVGA_EINVAL;
    if (!shader_batch_has_capacity(batch)) return SVGA_ENOMEM;
    if (!context ||
        local_id == UINT32_MAX || local_id >= 5000u ||
        (type != SVGA3D_SHADERTYPE_VS && type != SVGA3D_SHADERTYPE_PS))
        return SVGA_EINVAL;
    int reserve = svga_batch_operation_reserve(batch,
        SVGA_BATCH_OPERATION_ARRAY_SHADER, batch->shader_op_count + 1u);
    if (reserve) return reserve;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int ret = SVGA_EINVAL;
    int cid = svga_find_context(context);
    if (batch->generation != svga.generation || cid < 0 ||
        svga.contexts[cid].gb.retiring) goto done;
    ret = shader_batch_reserve_context_locked(batch, (uint32_t)cid);
    if (ret) goto done;
    struct svga_batch_shader_key *key = shader_batch_key_locked(
        batch, context, local_id, type, false);
    if (!key) {
        int live = shader_key_live_locked(context, local_id, type);
        if (live < 0) { ret = SVGA_EINVAL; goto done; }
        key = shader_batch_key_locked(batch, context, local_id, type, true);
    }
    if (!key || key->slot >= SVGA_GB_SHADER_SLOTS) {
        ret = SVGA_EINVAL;
        goto done;
    }
    uint32_t slot = key->slot;
    struct svga_shader *shader = &svga.shaders[slot];
    if ((!shader->defined && !shader->reserved) || shader->retiring ||
        shader->context != context || shader->local_id != local_id ||
        shader->type != type || (shader->reserved &&
        shader->reserved_by != batch->metadata->transaction_id)) {
        ret = SVGA_EINVAL;
        goto done;
    }
    struct svga_batch_shader_binding *binding =
        shader_batch_binding_locked(batch, (uint32_t)cid);
    if (!binding) { ret = SVGA_EINVAL; goto done; }
    uint32_t old_bytes = batch->bytes;
    uint32_t type_index = type - SVGA3D_SHADERTYPE_VS;
    if (binding->slots[type_index] == slot) {
        SVGA3dCmdSetShader unbind_context = {(uint32_t)cid, type,
                                             SVGA3D_INVALID_ID};
        ret = svga_batch_append(batch, SVGA_3D_CMD_SET_SHADER, true,
                                &unbind_context, sizeof(unbind_context));
    }
    SVGA3dCmdBindGBShader unbind_code = {slot, SVGA3D_INVALID_ID, 0};
    SVGA3dCmdDestroyGBShader destroy_shader = {slot};
    if (!ret) ret = svga_batch_append(batch, SVGA_3D_CMD_BIND_GB_SHADER, true,
                                      &unbind_code, sizeof(unbind_code));
    if (!ret) ret = svga_batch_append(batch, SVGA_3D_CMD_DESTROY_GB_SHADER, true,
                                      &destroy_shader, sizeof(destroy_shader));
    if (!ret) ret = svga_batch_append(batch, SVGA_3D_CMD_DESTROY_GB_MOB, true,
                                      &shader->mobid, sizeof(shader->mobid));
    if (ret) {
        batch->bytes = old_bytes;
        goto done;
    }
    batch->metadata->shader_ops[batch->shader_op_count++] =
        (struct svga_batch_shader_op){.context = context,
            .kind = SVGA_BATCH_SHADER_DESTROY, .local_id = local_id,
            .type = type, .slot = slot};
    ++batch->operation_count;
    key->slot = UINT32_MAX;
    if (binding->slots[type_index] == slot)
        binding->slots[type_index] = UINT32_MAX;
    ret = 0;
done:
    svga_unlock(flags);
    return ret;
}

/** @brief Publish staged shader events and final bindings after FIFO commit.
 * @param batch Submitted batch whose sidecar passed validation.
 * @param fence Real FIFO fence that covers all staged shader commands.
 */
void svga_shader_commit_batch_locked(struct svga_batch *batch,
                                     svga_handle fence)
{
    if (!batch || !batch->metadata) return;
    struct svga_batch_metadata *metadata = batch->metadata;
    if (metadata->shader_op_capacity > metadata->operation_capacity ||
        batch->shader_op_count > metadata->shader_op_capacity ||
        (batch->shader_op_count && !metadata->shader_ops))
        return;
    for (uint32_t i = 0; i < batch->shader_op_count; ++i) {
        const struct svga_batch_shader_op *op = &metadata->shader_ops[i];
        if (op->kind == SVGA_BATCH_SHADER_BIND) continue;
        struct svga_shader *shader = &svga.shaders[op->slot];
        if (op->kind == SVGA_BATCH_SHADER_DEFINE) {
            uint32_t pages = shader->code.pages + shader->code.pt_pages;
            if (svga.shader_reserved_pages >= shader->reserved_pages)
                svga.shader_reserved_pages -= shader->reserved_pages;
            else
                svga.shader_reserved_pages = 0;
            if (metadata->reserved_pages >= shader->reserved_pages)
                metadata->reserved_pages -= shader->reserved_pages;
            else
                metadata->reserved_pages = 0;
            svga.pages += pages;
            shader->code.defined = true;
            shader->ready_fence = fence;
            shader->defined = true;
            shader->reserved = false;
            shader->reserved_by = 0;
            shader->reserved_pages = 0;
            uint32_t serial = ++svga.handle_serial;
            if (!serial) serial = ++svga.handle_serial;
            shader->serial = serial;
        } else if (op->kind == SVGA_BATCH_SHADER_DESTROY) {
            shader->retiring = true;
            shader->retire_fence = fence;
        }
    }

    for (uint32_t i = 0; i < SVGA_CONTEXTS; ++i) {
        struct svga_batch_shader_binding *binding =
            &metadata->shader_bindings[i];
        if (!binding->used) continue;
        int cid = svga_find_context(binding->context);
        if (cid < 0 || (uint32_t)cid != i) continue;
        svga.contexts[cid].shaders[0] = binding->slots[0];
        svga.contexts[cid].shaders[1] = binding->slots[1];
    }
    for (uint32_t i = 0; i < metadata->state_context_count; ++i) {
        uint32_t cid = metadata->state_context_ids[i];
        if (cid < SVGA_CONTEXTS &&
            svga.contexts[cid].batch_state_owner == metadata->transaction_id)
            svga.contexts[cid].batch_state_owner = 0;
    }
}

/** @brief Detach staged shader slots and release context owners under svga_lock.
 * @param batch Aborting batch whose staged backing will be freed after unlock.
 */
void svga_shader_abort_batch_locked(struct svga_batch *batch)
{
    if (!batch || !batch->metadata) return;
    struct svga_batch_metadata *metadata = batch->metadata;
    for (uint32_t i = 0; i < metadata->reserved_shader_count; ++i) {
        uint32_t slot = metadata->reserved_shader_slots[i];
        if (slot >= SVGA_GB_SHADER_SLOTS) continue;
        struct svga_shader *shader = &svga.shaders[slot];
        if (!shader->reserved || shader->reserved_by != metadata->transaction_id)
            continue;
        metadata->abort_backings[slot] = shader->code;
        *shader = (struct svga_shader){0};
    }
    for (uint32_t i = 0; i < metadata->state_context_count; ++i) {
        uint32_t cid = metadata->state_context_ids[i];
        if (cid < SVGA_CONTEXTS &&
            svga.contexts[cid].batch_state_owner == metadata->transaction_id)
            svga.contexts[cid].batch_state_owner = 0;
    }
}

/**
 * @brief Free shader backing records only after their real FIFO fence passes.
 * @param budget Maximum number of records to reclaim; capped to host slots.
 * @param reaped Writable count of records detached and freed.
 * @return Zero on success, SVGA_EINVAL for null output, or SVGA_EBUSY if the
 *         lock is contended before any record is detached.
 */
int svga_backend_reap_retired(uint32_t budget, uint32_t *reaped)
{
    if (!reaped) return SVGA_EINVAL;
    *reaped = 0;
    if (budget > SVGA_GB_SHADER_SLOTS) budget = SVGA_GB_SHADER_SLOTS;
    for (uint32_t attempt = 0; attempt < budget; ++attempt) {
        uint64_t flags;
        if (!svga_try_lock_irqsave(&flags))
            return *reaped ? 0 : SVGA_EBUSY;
        struct svga_mob code = {0};
        struct svga_ops allocator = {0};
        bool found = false;
        uint32_t reclaimed_slot = 0;
        for (uint32_t offset = 0; offset < SVGA_GB_SHADER_SLOTS; ++offset) {
            uint32_t slot = (shader_reap_cursor + offset) % SVGA_GB_SHADER_SLOTS;
            struct svga_shader *shader = &svga.shaders[slot];
            if (!shader->defined || !shader->retiring || shader->reserved ||
                !svga_fence_reached_locked(shader->retire_fence))
                continue;
            code = shader->code;
            allocator = shader->allocator;
            uint32_t pages = code.pages + code.pt_pages;
            if (svga.pages >= pages) svga.pages -= pages;
            else svga.pages = 0;
            *shader = (struct svga_shader){0};
            reclaimed_slot = slot;
            found = true;
            break;
        }
        if (found) shader_reap_cursor =
            (reclaimed_slot + 1u) % SVGA_GB_SHADER_SLOTS;
        svga_unlock(flags);
        if (!found) return 0;
        svga_shader_backing_free(&allocator, &code);
        ++*reaped;
    }
    return 0;
}

/**
 * @brief Translate and append one inline VGPU9 shader-constant update.
 * @param batch Open batch that owns translated output.
 * @param context Live shader context retained through the submission fence.
 * @param reg First register index.
 * @param type Vertex or pixel shader type.
 * @param const_type Float, integer, or boolean constant type.
 * @param values Trusted readable DWORD values, four per register.
 * @param registers Number of consecutive 16-byte registers.
 * @return Zero on append, SVGA_ENOMEM on output exhaustion, SVGA_EBUSY on lock
 *         contention, or SVGA_EINVAL for invalid register ranges or input.
 */
int svga_batch_shader_constants(struct svga_batch *batch, svga_handle context,
                                uint32_t reg, uint32_t type, uint32_t const_type,
                                const uint32_t *values, uint32_t registers)
{
    if (!batch || !batch->metadata || !context || !values || !registers || batch->submitted ||
        (type != SVGA3D_SHADERTYPE_VS && type != SVGA3D_SHADERTYPE_PS) ||
        const_type > SVGA3D_CONST_TYPE_BOOL)
        return SVGA_EINVAL;
    uint32_t limit = const_type == SVGA3D_CONST_TYPE_FLOAT ? 256u : 16u;
    if (reg >= limit || registers > limit - reg ||
        registers > (UINT32_MAX - 16u) / 16u)
        return SVGA_EINVAL;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int cid = svga_find_context(context), ret = SVGA_EINVAL;
    if (batch->generation != svga.generation || cid < 0 ||
        svga.contexts[cid].gb.retiring) goto done;
    struct {
        uint32_t cid, reg, type, const_type;
    } prefix = {(uint32_t)cid, reg, type, const_type};
    uint32_t old_context_count = batch->context_count;
    ret = svga_batch_retain_context(batch, context);
    if (!ret)
        ret = batch_append_two_parts(batch, SVGA_3D_CMD_SET_GB_SHADERCONSTS_INLINE,
                                     &prefix, sizeof(prefix), values,
                                     registers * 16u);
    if (ret)
        svga_batch_rollback_references(batch, old_context_count,
            batch->surface_count, batch->gmr_count);
done:
    svga_unlock(flags);
    return ret;
}
