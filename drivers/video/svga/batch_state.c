#include "device.h"

static int batch_stage_binding(struct svga_batch *batch, svga_handle context,
                               uint32_t kind, uint32_t index,
                               svga_handle surface, uint32_t face,
                               uint32_t mipmap)
{
    if (!batch || !batch->metadata || batch->submitted || !context ||
        batch->operation_count >= batch->metadata->operation_capacity ||
        batch->binding_op_count >= batch->metadata->operation_capacity)
        return SVGA_EINVAL;
    int reserve = svga_batch_operation_reserve(batch,
        SVGA_BATCH_OPERATION_ARRAY_BINDING, batch->binding_op_count + 1u);
    if (reserve) return reserve;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int ret = SVGA_EINVAL;
    if (!svga.available || !svga.fifo_ready) {
        ret = SVGA_ENODEV;
        goto done;
    }
    if (batch->generation != svga.generation) {
        ret = SVGA_EIO;
        goto done;
    }
    int cid = svga_find_context(context);
    if (cid < 0 || svga.contexts[cid].gb.retiring) goto done;
    if ((kind == SVGA_BATCH_BINDING_RENDER_TARGET && index >= SVGA3D_RT_MAX) ||
        (kind == SVGA_BATCH_BINDING_TEXTURE && index >= 32u) ||
        (kind != SVGA_BATCH_BINDING_RENDER_TARGET &&
         kind != SVGA_BATCH_BINDING_TEXTURE)) goto done;

    int sid = -1;
    if (surface) {
        sid = svga_find_surface(surface);
        if (sid < 0 || svga.surfaces[sid].retiring) goto done;
        uint32_t mip_levels = svga.surfaces[sid].desc.mip_levels;
        if (!mip_levels) mip_levels = 1;
        if (kind == SVGA_BATCH_BINDING_RENDER_TARGET &&
            (face >= svga.surfaces[sid].faces || mipmap >= mip_levels))
            goto done;
    } else if (face || mipmap) {
        goto done;
    }

    ret = svga_batch_reserve_context_locked(batch, (uint32_t)cid);
    if (ret) goto done;
    if (surface) {
        ret = svga_batch_retain_surface(batch, surface);
        if (ret) goto done;
    }
    batch->metadata->binding_ops[batch->binding_op_count++] =
        (struct svga_batch_binding_op){.context = context, .surface = surface,
            .kind = kind, .index = index, .face = face, .mipmap = mipmap};
    ++batch->operation_count;
    ret = 0;
done:
    svga_unlock(flags);
    return ret;
}

int svga_batch_stage_render_target(struct svga_batch *batch,
                                   svga_handle context, uint32_t target,
                                   svga_handle surface, uint32_t face,
                                   uint32_t mipmap)
{
    return batch_stage_binding(batch, context,
        SVGA_BATCH_BINDING_RENDER_TARGET, target, surface, face, mipmap);
}

int svga_batch_stage_texture(struct svga_batch *batch, svga_handle context,
                             uint32_t stage, svga_handle surface)
{
    return batch_stage_binding(batch, context, SVGA_BATCH_BINDING_TEXTURE,
                               stage, surface, 0, 0);
}

int svga_batch_state_validate_locked(struct svga_batch *batch)
{
    if (!batch) return SVGA_EINVAL;
    if (!batch->metadata) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_BINDING,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_BINDING_METADATA, 0, 1);
        return SVGA_EINVAL;
    }
    if (batch->metadata->binding_op_capacity >
            batch->metadata->operation_capacity ||
        batch->binding_op_count > batch->metadata->binding_op_capacity ||
        (batch->binding_op_count && !batch->metadata->binding_ops)) {
        svga_batch_submit_failure_set(batch,
            SVGA_BATCH_SUBMIT_REASON_BINDING,
            SVGA_BATCH_SUBMIT_INDEX_NONE,
            SVGA_BATCH_SUBMIT_DETAIL_BINDING_METADATA,
            batch->binding_op_count,
            batch->metadata->binding_op_capacity);
        return SVGA_EINVAL;
    }
    struct svga_batch_metadata *metadata = batch->metadata;
    for (uint32_t i = 0; i < batch->binding_op_count; ++i) {
        const struct svga_batch_binding_op *op = &metadata->binding_ops[i];
        int cid = svga_find_context(op->context);
        if (cid < 0) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_BINDING, i,
                SVGA_BATCH_SUBMIT_DETAIL_BINDING_CONTEXT_MISSING,
                op->context, 0);
            return SVGA_EINVAL;
        }
        if (!metadata->transaction_id) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_BINDING, i,
                SVGA_BATCH_SUBMIT_DETAIL_BINDING_TRANSACTION,
                metadata->transaction_id, 1);
            return SVGA_EINVAL;
        }
        if (svga.contexts[cid].batch_state_owner != metadata->transaction_id) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_BINDING, i,
                SVGA_BATCH_SUBMIT_DETAIL_BINDING_CONTEXT_OWNER,
                metadata->transaction_id,
                svga.contexts[cid].batch_state_owner);
            return SVGA_EINVAL;
        }
        if (svga.contexts[cid].gb.retiring) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_BINDING, i,
                SVGA_BATCH_SUBMIT_DETAIL_BINDING_CONTEXT_RETIRING,
                op->context, 1);
            return SVGA_EINVAL;
        }
        if (op->kind == SVGA_BATCH_BINDING_RENDER_TARGET &&
            op->index >= SVGA3D_RT_MAX) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_BINDING, i,
                SVGA_BATCH_SUBMIT_DETAIL_BINDING_INDEX,
                op->index, SVGA3D_RT_MAX);
            return SVGA_EINVAL;
        }
        if (op->kind == SVGA_BATCH_BINDING_TEXTURE && op->index >= 32u) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_BINDING, i,
                SVGA_BATCH_SUBMIT_DETAIL_BINDING_INDEX, op->index, 32u);
            return SVGA_EINVAL;
        }
        if (op->kind != SVGA_BATCH_BINDING_RENDER_TARGET &&
            op->kind != SVGA_BATCH_BINDING_TEXTURE) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_BINDING, i,
                SVGA_BATCH_SUBMIT_DETAIL_BINDING_KIND, op->kind,
                SVGA_BATCH_BINDING_TEXTURE);
            return SVGA_EINVAL;
        }
        if (!op->surface) {
            if (op->face || op->mipmap) {
                svga_batch_submit_failure_set(batch,
                    SVGA_BATCH_SUBMIT_REASON_BINDING, i,
                    SVGA_BATCH_SUBMIT_DETAIL_BINDING_CLEAR_FIELDS,
                    op->face, op->mipmap);
                return SVGA_EINVAL;
            }
            continue;
        }
        int sid = svga_find_surface(op->surface);
        if (sid < 0) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_BINDING, i,
                SVGA_BATCH_SUBMIT_DETAIL_BINDING_SURFACE_MISSING,
                op->surface, 0);
            return SVGA_EINVAL;
        }
        if (svga.surfaces[sid].retiring) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_BINDING, i,
                SVGA_BATCH_SUBMIT_DETAIL_BINDING_SURFACE_RETIRING,
                op->surface, 1);
            return SVGA_EINVAL;
        }
        uint32_t mip_levels = svga.surfaces[sid].desc.mip_levels;
        if (!mip_levels) mip_levels = 1;
        if (op->kind == SVGA_BATCH_BINDING_RENDER_TARGET &&
            op->face >= svga.surfaces[sid].faces) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_BINDING, i,
                SVGA_BATCH_SUBMIT_DETAIL_BINDING_FACE_RANGE,
                op->face, svga.surfaces[sid].faces);
            return SVGA_EINVAL;
        }
        if (op->kind == SVGA_BATCH_BINDING_RENDER_TARGET &&
            op->mipmap >= mip_levels) {
            svga_batch_submit_failure_set(batch,
                SVGA_BATCH_SUBMIT_REASON_BINDING, i,
                SVGA_BATCH_SUBMIT_DETAIL_BINDING_MIP_RANGE,
                op->mipmap, mip_levels);
            return SVGA_EINVAL;
        }
    }
    return 0;
}

void svga_batch_state_commit_locked(const struct svga_batch *batch)
{
    if (!batch || !batch->metadata) return;
    const struct svga_batch_metadata *metadata = batch->metadata;
    if (metadata->binding_op_capacity > metadata->operation_capacity ||
        batch->binding_op_count > metadata->binding_op_capacity ||
        (batch->binding_op_count && !metadata->binding_ops))
        return;
    for (uint32_t i = 0; i < batch->binding_op_count; ++i) {
        const struct svga_batch_binding_op *op = &metadata->binding_ops[i];
        int cid = svga_find_context(op->context);
        if (cid < 0) continue;
        uint32_t sid = SVGA3D_INVALID_ID;
        if (op->surface) {
            int found = svga_find_surface(op->surface);
            if (found >= 0) sid = (uint32_t)found;
        }
        if (op->kind == SVGA_BATCH_BINDING_RENDER_TARGET)
            svga.contexts[cid].targets[op->index] = sid;
        else if (op->kind == SVGA_BATCH_BINDING_TEXTURE)
            svga.contexts[cid].textures[op->index] = sid;
    }
}
