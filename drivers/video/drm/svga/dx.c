/* SPDX-License-Identifier: Apache-2.0 */
#include "context.h"

/* The list follows the userspace DX entries in Linux vmwgfx_execbuf.c.
 * COTable allocation, context definition and raw MOB management are private. */
#define DX_FIXED(command, type) [SVGA_3D_CMD_DX_ ## command - SVGA_3D_CMD_DX_MIN] = {sizeof(type), 0, 0, 40}
#define DX_SM5(command, type) [SVGA_3D_CMD_DX_ ## command - SVGA_3D_CMD_DX_MIN] = {sizeof(type), 0, 0, 50}
#define DX_ARRAY(command, type, elem, count, sm) \
    [SVGA_3D_CMD_DX_ ## command - SVGA_3D_CMD_DX_MIN] = {sizeof(type), sizeof(elem), count, sm}

static const struct dx_shape {
    uint16_t prefix, stride, count;
    uint8_t shader_model;
} dx_shapes[SVGA_3D_CMD_MAX - SVGA_3D_CMD_DX_MIN] = {
    DX_FIXED(SET_SINGLE_CONSTANT_BUFFER, SVGA3dCmdDXSetSingleConstantBuffer),
    DX_ARRAY(SET_SHADER_RESOURCES, SVGA3dCmdDXSetShaderResources, uint32_t, SVGA3D_DX_MAX_SRVIEWS, 40),
    DX_FIXED(SET_SHADER, SVGA3dCmdDXSetShader),
    DX_ARRAY(SET_SAMPLERS, SVGA3dCmdDXSetSamplers, uint32_t, SVGA3D_DX_MAX_SAMPLERS, 40),
    DX_FIXED(DRAW, SVGA3dCmdDXDraw),
    DX_FIXED(DRAW_INDEXED, SVGA3dCmdDXDrawIndexed),
    DX_FIXED(DRAW_INSTANCED, SVGA3dCmdDXDrawInstanced),
    DX_FIXED(DRAW_INDEXED_INSTANCED, SVGA3dCmdDXDrawIndexedInstanced),
    DX_FIXED(DRAW_AUTO, SVGA3dCmdDXDrawAuto),
    DX_ARRAY(SET_VERTEX_BUFFERS, SVGA3dCmdDXSetVertexBuffers, SVGA3dVertexBuffer, SVGA3D_DX_MAX_VERTEXBUFFERS, 40),
    DX_FIXED(SET_INDEX_BUFFER, SVGA3dCmdDXSetIndexBuffer),
    DX_ARRAY(SET_RENDERTARGETS, SVGA3dCmdDXSetRenderTargets, uint32_t, SVGA3D_DX_MAX_RENDER_TARGETS, 40),
    DX_FIXED(SET_BLEND_STATE, SVGA3dCmdDXSetBlendState),
    DX_FIXED(SET_DEPTHSTENCIL_STATE, SVGA3dCmdDXSetDepthStencilState),
    DX_FIXED(SET_RASTERIZER_STATE, SVGA3dCmdDXSetRasterizerState),
    DX_FIXED(DEFINE_QUERY, SVGA3dCmdDXDefineQuery),
    DX_FIXED(DESTROY_QUERY, SVGA3dCmdDXDestroyQuery),
    DX_FIXED(BIND_QUERY, SVGA3dCmdDXBindQuery),
    DX_FIXED(SET_QUERY_OFFSET, SVGA3dCmdDXSetQueryOffset),
    DX_FIXED(BEGIN_QUERY, SVGA3dCmdDXBeginQuery),
    DX_FIXED(END_QUERY, SVGA3dCmdDXEndQuery),
    DX_FIXED(SET_PREDICATION, SVGA3dCmdDXSetPredication),
    DX_ARRAY(SET_VIEWPORTS, SVGA3dCmdDXSetViewports, SVGA3dViewport, SVGA3D_DX_MAX_VIEWPORTS, 40),
    DX_ARRAY(SET_SCISSORRECTS, SVGA3dCmdDXSetScissorRects, SVGASignedRect, SVGA3D_DX_MAX_SCISSORRECTS, 40),
    DX_FIXED(CLEAR_RENDERTARGET_VIEW, SVGA3dCmdDXClearRenderTargetView),
    DX_FIXED(CLEAR_DEPTHSTENCIL_VIEW, SVGA3dCmdDXClearDepthStencilView),
    DX_FIXED(GENMIPS, SVGA3dCmdDXGenMips),
    DX_FIXED(UPDATE_SUBRESOURCE, SVGA3dCmdDXUpdateSubResource),
    DX_FIXED(READBACK_SUBRESOURCE, SVGA3dCmdDXReadbackSubResource),
    DX_FIXED(INVALIDATE_SUBRESOURCE, SVGA3dCmdDXInvalidateSubResource),
    DX_FIXED(DEFINE_SHADERRESOURCE_VIEW, SVGA3dCmdDXDefineShaderResourceView),
    DX_FIXED(DESTROY_SHADERRESOURCE_VIEW, SVGA3dCmdDXDestroyShaderResourceView),
    DX_FIXED(DEFINE_RENDERTARGET_VIEW, SVGA3dCmdDXDefineRenderTargetView),
    DX_FIXED(DESTROY_RENDERTARGET_VIEW, SVGA3dCmdDXDestroyRenderTargetView),
    DX_FIXED(DEFINE_DEPTHSTENCIL_VIEW, SVGA3dCmdDXDefineDepthStencilView),
    DX_FIXED(DESTROY_DEPTHSTENCIL_VIEW, SVGA3dCmdDXDestroyDepthStencilView),
    DX_ARRAY(DEFINE_ELEMENTLAYOUT, SVGA3dCmdDXDefineElementLayout, SVGA3dInputElementDesc, 32, 40),
    DX_FIXED(DESTROY_ELEMENTLAYOUT, SVGA3dCmdDXDestroyElementLayout),
    DX_FIXED(DEFINE_BLEND_STATE, SVGA3dCmdDXDefineBlendState),
    DX_FIXED(DESTROY_BLEND_STATE, SVGA3dCmdDXDestroyBlendState),
    DX_FIXED(DEFINE_DEPTHSTENCIL_STATE, SVGA3dCmdDXDefineDepthStencilState),
    DX_FIXED(DESTROY_DEPTHSTENCIL_STATE, SVGA3dCmdDXDestroyDepthStencilState),
    DX_FIXED(DEFINE_RASTERIZER_STATE, SVGA3dCmdDXDefineRasterizerState),
    DX_FIXED(DESTROY_RASTERIZER_STATE, SVGA3dCmdDXDestroyRasterizerState),
    DX_FIXED(DEFINE_SAMPLER_STATE, SVGA3dCmdDXDefineSamplerState),
    DX_FIXED(DESTROY_SAMPLER_STATE, SVGA3dCmdDXDestroySamplerState),
    DX_FIXED(DEFINE_SHADER, SVGA3dCmdDXDefineShader),
    DX_FIXED(DESTROY_SHADER, SVGA3dCmdDXDestroyShader),
    DX_FIXED(BIND_SHADER, SVGA3dCmdDXBindShader),
    DX_FIXED(DEFINE_STREAMOUTPUT, SVGA3dCmdDXDefineStreamOutput),
    DX_FIXED(DESTROY_STREAMOUTPUT, SVGA3dCmdDXDestroyStreamOutput),
    DX_FIXED(SET_STREAMOUTPUT, SVGA3dCmdDXSetStreamOutput),
    DX_ARRAY(SET_SOTARGETS, SVGA3dCmdDXSetSOTargets, SVGA3dSoTarget, SVGA3D_DX_MAX_SOTARGETS, 40),
    DX_FIXED(SET_INPUT_LAYOUT, SVGA3dCmdDXSetInputLayout),
    DX_FIXED(SET_TOPOLOGY, SVGA3dCmdDXSetTopology),
    DX_FIXED(BUFFER_COPY, SVGA3dCmdDXBufferCopy),
    DX_FIXED(PRED_COPY_REGION, SVGA3dCmdDXPredCopyRegion),
    DX_FIXED(TRANSFER_FROM_BUFFER, SVGA3dCmdDXTransferFromBuffer),
    DX_FIXED(SET_VS_CONSTANT_BUFFER_OFFSET, SVGA3dCmdDXSetConstantBufferOffset),
    DX_FIXED(SET_PS_CONSTANT_BUFFER_OFFSET, SVGA3dCmdDXSetConstantBufferOffset),
    DX_FIXED(SET_GS_CONSTANT_BUFFER_OFFSET, SVGA3dCmdDXSetConstantBufferOffset),
    DX_SM5(SET_HS_CONSTANT_BUFFER_OFFSET, SVGA3dCmdDXSetConstantBufferOffset),
    DX_SM5(SET_DS_CONSTANT_BUFFER_OFFSET, SVGA3dCmdDXSetConstantBufferOffset),
    DX_SM5(SET_CS_CONSTANT_BUFFER_OFFSET, SVGA3dCmdDXSetConstantBufferOffset),
    DX_SM5(DEFINE_UA_VIEW, SVGA3dCmdDXDefineUAView),
    DX_SM5(DESTROY_UA_VIEW, SVGA3dCmdDXDestroyUAView),
    DX_SM5(CLEAR_UA_VIEW_UINT, SVGA3dCmdDXClearUAViewUint),
    DX_SM5(CLEAR_UA_VIEW_FLOAT, SVGA3dCmdDXClearUAViewFloat),
    DX_ARRAY(SET_UA_VIEWS, SVGA3dCmdDXSetUAViews, uint32_t, SVGA3D_MAX_UAVIEWS, 50),
    DX_SM5(DRAW_INDEXED_INSTANCED_INDIRECT, SVGA3dCmdDXDrawIndexedInstancedIndirect),
    DX_SM5(DRAW_INSTANCED_INDIRECT, SVGA3dCmdDXDrawInstancedIndirect),
    DX_SM5(DISPATCH, SVGA3dCmdDXDispatch),
    DX_SM5(DISPATCH_INDIRECT, SVGA3dCmdDXDispatchIndirect),
    DX_ARRAY(SET_CS_UA_VIEWS, SVGA3dCmdDXSetCSUAViews, uint32_t, SVGA3D_MAX_UAVIEWS, 50),
    DX_SM5(DEFINE_DEPTHSTENCIL_VIEW_V2, SVGA3dCmdDXDefineDepthStencilView_v2),
    DX_SM5(DEFINE_STREAMOUTPUT_WITH_MOB, SVGA3dCmdDXDefineStreamOutputWithMob),
    DX_SM5(BIND_STREAMOUTPUT, SVGA3dCmdDXBindStreamOutput),
    DX_SM5(DEFINE_RASTERIZER_STATE_V2, SVGA3dCmdDXDefineRasterizerState_v2),
};

static bool shader_type(uint32_t type)
{
    unsigned limit = svga_render_shader_model() >= 50 ? SVGA3D_SHADERTYPE_MAX : SVGA3D_SHADERTYPE_DX10_MAX;
    return type >= SVGA3D_SHADERTYPE_MIN && type < limit;
}

static int define(struct svga_batch *batch, unsigned table, uint32_t id,
                  struct svga_context_value value)
{
    if (svga_context_lookup(batch->dx, table, id))
        return -EINVAL;
    int ret = svga_context_require_table(batch, table, id);
    if (ret)
        return ret;
    value.defined = true;
    return svga_context_stage(batch, batch->dx, table, id, value);
}

static int destroy(struct svga_batch *batch, unsigned table, uint32_t id)
{
    if (!svga_context_lookup(batch->dx, table, id))
        return -EINVAL;
    int ret = 0;
    if (table == SVGA_COTABLE_DXQUERY) {
        const struct svga_context_value *value = svga_context_lookup(batch->dx, table, id);
        if (value->buffer)
            ret = svga_batch_bo(batch, svga_buffer_bo(value->buffer), NULL);
    }
    if (!ret)
        ret = svga_context_remove_bindings(batch, batch->dx, table, id, SVGA_BIND_RT, SVGA_BIND_DX_SO);
    if (!ret)
        ret = svga_context_stage(batch, batch->dx, table, id, (struct svga_context_value){0});
    return ret;
}

static int lookup_active(struct svga_batch *batch, unsigned table, uint32_t id,
                          struct svga_context_value *out)
{
    const struct svga_context_value *value = svga_context_lookup(batch->dx, table, id);
    if (!value)
        return -EINVAL;
    int ret = value->object ? svga_batch_object(batch, value->object) : 0;
    if (!ret && value->buffer)
        ret = svga_batch_bo(batch, svga_buffer_bo(value->buffer), NULL);
    if (!ret)
        ret = svga_context_replay_resource(batch, table, id);
    if (!ret && out)
        *out = *value;
    return ret;
}

static int write_view(struct svga_batch *batch, unsigned table, uint32_t id)
{
    struct svga_context_value value;
    int ret = lookup_active(batch, table, id, &value);
    if (!ret)
        svga_batch_surface_written((struct svga_surface *)value.object);
    return ret;
}

static int bind_table_entry(struct svga_batch *batch, unsigned table, uint32_t id,
                             unsigned key, uint32_t slot)
{
    struct svga_context_value value = {0};
    if (id != SVGA3D_INVALID_ID) {
        int ret = lookup_active(batch, table, id, &value);
        if (ret)
            return ret;
        value.type = table;
        value.id = id;
        value.view = NULL;
        value.mob = NULL;
    }
    return svga_context_stage_binding(batch, batch->dx, key, slot, value);
}

static int bind_entries(struct svga_batch *batch, unsigned table, unsigned key,
                         uint32_t start, const uint32_t *ids, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        int ret = bind_table_entry(batch, table, ids[i], key, start + i);
        if (ret)
            return ret;
    }
    return 0;
}

static int simple_id(struct svga_batch *batch, unsigned table, uint32_t id)
{
    return id == SVGA3D_INVALID_ID || svga_context_lookup(batch->dx, table, id) ? 0 : -EINVAL;
}

static int define_view(struct svga_batch *batch, unsigned table, const struct svga_packet *packet)
{
    uint32_t *words = packet->body;
    struct svga_surface *surface;
    int ret = svga_batch_surface(batch, &words[1], false, &surface);
    if (ret)
        return ret;
    if (!surface->gb)
        return -EINVAL;
    return svga_context_stage_view(batch, table, packet, surface);
}

static int destroy_view(struct svga_batch *batch, unsigned table, const struct svga_packet *packet)
{
    uint32_t id = *(const uint32_t *)packet->body;
    bool detached = svga_context_view_scrubbed(batch, table, id);
    int ret = destroy(batch, table, id);
    if (!ret && detached) {
        /* The logical definition exists but the device already destroyed
         * it during eviction. Keep the original packet size for parsing. */
        uint32_t *header = (uint32_t *)packet->body - 2;
        *header = SVGA_3D_CMD_NOP;
    }
    return ret;
}

static int get_buffer(struct svga_batch *batch, uint32_t *id, bool optional,
                       uint32_t offset, uint32_t bytes, struct svga_surface **out)
{
    struct svga_surface *surface;
    int ret = svga_batch_surface(batch, id, optional, &surface);
    if (ret)
        return ret;
    if (surface && (surface->info.base.format != SVGA3D_BUFFER ||
        !svga_buffer_fits(surface->info.base.base_size.width, offset, bytes)))
        return -EINVAL;
    if (out)
        *out = surface;
    return 0;
}

static int bind_buffer(struct svga_batch *batch, unsigned key, uint32_t slot,
                        uint32_t *id, uint32_t offset, uint32_t bytes, uint32_t type)
{
    struct svga_surface *surface;
    uint32_t checked_offset = key == SVGA_BIND_SO_TARGET && offset == SVGA3D_DX_SO_OFFSET_APPEND ? 0 : offset;
    int ret = get_buffer(batch, id, true, checked_offset, bytes, &surface);
    if (ret)
        return ret;
    return svga_context_stage_binding(batch, batch->dx, key, slot,
        (struct svga_context_value){.object = surface ? &surface->object : NULL,
            .defined = surface != NULL, .offset = offset, .size = bytes, .type = type});
}

static int set_constant_buffer(struct svga_batch *batch, uint32_t *words)
{
    const SVGA3dCmdDXSetSingleConstantBuffer *cmd = (const void *)words;
    if (!shader_type(cmd->type) || cmd->slot >= SVGA3D_DX_MAX_CONSTBUFFERS ||
        cmd->sizeInBytes > SVGA3D_DX_MAX_CONSTBUF_BINDING_SIZE || (cmd->offsetInBytes & 15))
        return -EINVAL;
    unsigned slot = cmd->type * SVGA3D_DX_MAX_CONSTBUFFERS + cmd->slot;
    return bind_buffer(batch, SVGA_BIND_CONSTANT, slot, &words[2], cmd->offsetInBytes, cmd->sizeInBytes, 0);
}

static int set_constant_offset(struct svga_batch *batch, unsigned type, const uint32_t *words)
{
    if (!shader_type(type) || words[0] >= SVGA3D_DX_MAX_CONSTBUFFERS || (words[1] & 15))
        return -EINVAL;
    unsigned slot = type * SVGA3D_DX_MAX_CONSTBUFFERS + words[0];
    const struct svga_context_value *old = svga_context_lookup(batch->dx, SVGA_BIND_CONSTANT, slot);
    if (!old)
        return 0;
    struct svga_context_value value = *old;
    const struct svga_surface *surface = (const struct svga_surface *)value.object;
    if (!svga_buffer_fits(surface->info.base.base_size.width, words[1], value.size))
        return -EINVAL;
    value.offset = words[1];
    return svga_context_stage_binding(batch, batch->dx, SVGA_BIND_CONSTANT, slot, value);
}

static int set_shader(struct svga_batch *batch, const SVGA3dCmdDXSetShader *cmd)
{
    if (!shader_type(cmd->type))
        return -EINVAL;
    if (cmd->shaderId != SVGA3D_INVALID_ID) {
        const struct svga_context_value *shader = svga_context_lookup(batch->dx, SVGA_COTABLE_DXSHADER, cmd->shaderId);
        if (!shader || shader->type != cmd->type || !shader->buffer)
            return -EINVAL;
    }
    return bind_table_entry(batch, SVGA_COTABLE_DXSHADER, cmd->shaderId, SVGA_BIND_DX_SHADER, cmd->type);
}

static int bind_mob(struct svga_batch *batch, struct svga_context *context,
                     unsigned table, uint32_t id, uint32_t *handle, uint32_t offset, uint32_t bytes)
{
    const struct svga_context_value *old = svga_context_lookup(context, table, id);
    if (!old || (offset & 3))
        return -EINVAL;
    struct svga_context_value value = *old;
    struct svga_buffer *buffer;
    int ret = svga_batch_handle(batch, *handle, &buffer);
    if (ret)
        return ret;
    if (!svga_buffer_fits(drm_bo_size(svga_buffer_bo(buffer)), offset, bytes))
        return -EINVAL;
    struct svga_region *mob;
    ret = svga_batch_map_mob(batch, buffer, &mob);
    if (ret)
        return ret;
    if (table == SVGA_COTABLE_DXQUERY) {
        const struct svga_context_value *query = svga_context_lookup(context, SVGA_DX_QUERY_MOB, 0);
        if (query && query->buffer != buffer)
            return -EINVAL;
        if (!query) {
            ret = svga_context_stage_binding(batch, context, SVGA_DX_QUERY_MOB, 0,
                (struct svga_context_value){.defined = true, .buffer = buffer, .mob = mob});
            if (ret)
                return ret;
        }
    } else {
        value.mob = mob;
    }
    *handle = svga_region_id(mob);
    value.buffer = buffer;
    value.offset = offset;
    value.size = bytes;
    ret = svga_context_stage(batch, context, table, id, value);
    if (!ret && table == SVGA_COTABLE_DXSHADER) {
        const struct svga_context_value *bound = svga_context_lookup(context, SVGA_BIND_DX_SHADER, value.type);
        if (bound && bound->id == id) {
            struct svga_context_value binding = *bound;
            binding.buffer = buffer;
            ret = svga_context_stage_binding(batch, context, SVGA_BIND_DX_SHADER, value.type, binding);
        }
    } else if (!ret && table == SVGA_COTABLE_STREAMOUTPUT) {
        const struct svga_context_value *bound = svga_context_lookup(context, SVGA_BIND_DX_SO, 0);
        if (bound && bound->id == id) {
            struct svga_context_value binding = *bound;
            binding.buffer = buffer;
            ret = svga_context_stage_binding(batch, context, SVGA_BIND_DX_SO, 0, binding);
        }
    }
    return ret;
}

static int bind_shader(struct svga_batch *batch, uint32_t *words)
{
    struct svga_context *context = batch->dx;
    if (words[0] != SVGA3D_INVALID_ID) {
        int ret = svga_batch_context(batch, words, &context);
        if (ret)
            return ret;
    }
    if (!context || context->type != SVGA_CONTEXT_DX)
        return -EINVAL;
    const struct svga_context_value *shader = svga_context_lookup(context, SVGA_COTABLE_DXSHADER, words[1]);
    return shader ? bind_mob(batch, context, SVGA_COTABLE_DXSHADER, words[1], &words[2], words[3], shader->size) : -EINVAL;
}

static uint32_t query_bytes(uint32_t type)
{
    static const uint8_t size[SVGA3D_QUERYTYPE_MAX] = {
        [SVGA3D_QUERYTYPE_OCCLUSION] = sizeof(SVGADXOcclusionQueryResult),
        [SVGA3D_QUERYTYPE_TIMESTAMP] = sizeof(SVGADXTimestampQueryResult),
        [SVGA3D_QUERYTYPE_TIMESTAMPDISJOINT] = sizeof(SVGADXTimestampDisjointQueryResult),
        [SVGA3D_QUERYTYPE_PIPELINESTATS] = sizeof(SVGADXPipelineStatisticsQueryResult),
        [SVGA3D_QUERYTYPE_OCCLUSIONPREDICATE] = sizeof(SVGADXOcclusionPredicateQueryResult),
        [SVGA3D_QUERYTYPE_STREAMOUTPUTSTATS] = sizeof(SVGADXStreamOutStatisticsQueryResult),
        [SVGA3D_QUERYTYPE_STREAMOVERFLOWPREDICATE] = sizeof(SVGADXStreamOutPredicateQueryResult),
        [SVGA3D_QUERYTYPE_OCCLUSION64] = sizeof(SVGADXOcclusion64QueryResult),
        [SVGA3D_QUERYTYPE_SOSTATS_STREAM0 ... SVGA3D_QUERYTYPE_SOSTATS_STREAM3] = sizeof(SVGADXStreamOutStatisticsQueryResult),
        [SVGA3D_QUERYTYPE_SOP_STREAM0 ... SVGA3D_QUERYTYPE_SOP_STREAM3] = sizeof(SVGADXStreamOutPredicateQueryResult),
    };
    return type < SVGA3D_QUERYTYPE_MAX && size[type] ? size[type] + sizeof(uint32_t) : 0;
}

static int dx_query(struct svga_batch *batch, uint32_t command, uint32_t *words)
{
    if (command == SVGA_3D_CMD_DX_DEFINE_QUERY) {
        uint32_t bytes = query_bytes(words[1]);
        return bytes ? define(batch, SVGA_COTABLE_DXQUERY, words[0],
                               (struct svga_context_value){.size = bytes}) : -EINVAL;
    }
    if (command == SVGA_3D_CMD_DX_DESTROY_QUERY)
        return destroy(batch, SVGA_COTABLE_DXQUERY, words[0]);
    if (command == SVGA_3D_CMD_DX_SET_PREDICATION && words[0] == SVGA3D_INVALID_ID)
        return 0;
    const struct svga_context_value *old = svga_context_lookup(batch->dx, SVGA_COTABLE_DXQUERY, words[0]);
    if (!old)
        return -EINVAL;
    struct svga_context_value value = *old;
    switch (command) {
    case SVGA_3D_CMD_DX_BIND_QUERY:
        return bind_mob(batch, batch->dx, SVGA_COTABLE_DXQUERY, words[0], &words[1], value.offset, value.size);
    case SVGA_3D_CMD_DX_SET_QUERY_OFFSET:
        if ((words[1] & 3) || (value.buffer &&
            !svga_buffer_fits(drm_bo_size(svga_buffer_bo(value.buffer)), words[1], value.size)))
            return -EINVAL;
        value.offset = words[1];
        return svga_context_stage(batch, batch->dx, SVGA_COTABLE_DXQUERY, words[0], value);
    default:
        return value.buffer ? svga_batch_bo(batch, svga_buffer_bo(value.buffer), NULL) : -EINVAL;
    }
}

static int subresource(struct svga_batch *batch, uint32_t *id, uint32_t index,
                        struct svga_surface **out, SVGA3dSize *size)
{
    struct svga_surface *surface;
    int ret = svga_batch_surface(batch, id, false, &surface);
    if (ret)
        return ret;
    if (!surface->gb)
        return -EINVAL;
    unsigned levels = surface->info.base.mip_levels;
    if (!svga_surface_image(surface, index / levels, index % levels, size))
        return -EINVAL;
    if (out)
        *out = surface;
    return 0;
}

static bool box_fits(const SVGA3dSize *size, const SVGA3dBox *box)
{
    return box->w && box->h && box->d && (uint64_t)box->x + box->w <= size->width &&
        (uint64_t)box->y + box->h <= size->height && (uint64_t)box->z + box->d <= size->depth;
}

static int dx_copy(struct svga_batch *batch, uint32_t command, uint32_t *words)
{
    struct svga_surface *surface;
    if (command == SVGA_3D_CMD_DX_BUFFER_COPY) {
        const SVGA3dCmdDXBufferCopy *cmd = (const void *)words;
        int ret = get_buffer(batch, words, false, cmd->destX, cmd->width, &surface);
        if (ret)
            return ret;
        svga_batch_surface_written(surface);
        return get_buffer(batch, &words[1], false, cmd->srcX, cmd->width, NULL);
    }
    SVGA3dSize dst;
    if (command == SVGA_3D_CMD_DX_PRED_COPY_REGION) {
        const SVGA3dCmdDXPredCopyRegion *cmd = (const void *)words;
        SVGA3dSize src;
        int ret = subresource(batch, words, cmd->dstSubResource, &surface, &dst);
        if (!ret)
            ret = subresource(batch, &words[2], cmd->srcSubResource, NULL, &src);
        if (ret)
            return ret;
        const SVGA3dCopyBox b = cmd->box;
        const SVGA3dBox a = {b.srcx, b.srcy, b.srcz, b.w, b.h, b.d};
        const SVGA3dBox d = {b.x, b.y, b.z, b.w, b.h, b.d};
        if (!box_fits(&src, &a) || !box_fits(&dst, &d))
            return -EINVAL;
        svga_batch_surface_written(surface);
        return 0;
    }
    const SVGA3dCmdDXTransferFromBuffer *cmd = (const void *)words;
    struct svga_surface *src;
    int ret = get_buffer(batch, words, false, cmd->srcOffset, 0, &src);
    if (!ret)
        ret = subresource(batch, &words[4], cmd->destSubResource, &surface, &dst);
    if (ret)
        return ret;
    svga_batch_surface_written(surface);
    return svga_surface_transfer_check(surface, &dst, &cmd->destBox, cmd->srcPitch, cmd->srcSlicePitch,
                                       src->info.base.base_size.width - cmd->srcOffset);
}

int svga_validate_dx(struct svga_batch *batch, const struct svga_packet *p)
{
    if (p->command < SVGA_3D_CMD_DX_MIN || p->command >= SVGA_3D_CMD_MAX)
        return -ENOTTY;
    const struct dx_shape *shape = &dx_shapes[p->command - SVGA_3D_CMD_DX_MIN];
    if (!shape->shader_model)
        return -ENOTTY;
    if (svga_render_shader_model() < shape->shader_model)
        return -EINVAL;
    int count = shape->stride ? svga_array_count(p->bytes, shape->prefix, shape->stride, shape->count) :
        p->bytes == shape->prefix ? 0 : -1;
    if (count < 0)
        return -EINVAL;
    uint32_t *w = p->body;
    if (p->command == SVGA_3D_CMD_DX_BIND_SHADER)
        return bind_shader(batch, w);
    if (!batch->dx)
        return -EINVAL;
    switch (p->command) {
    case SVGA_3D_CMD_DX_SET_SINGLE_CONSTANT_BUFFER: return set_constant_buffer(batch, w);
    case SVGA_3D_CMD_DX_SET_VS_CONSTANT_BUFFER_OFFSET ... SVGA_3D_CMD_DX_SET_CS_CONSTANT_BUFFER_OFFSET:
        return set_constant_offset(batch, SVGA3D_SHADERTYPE_VS + p->command - SVGA_3D_CMD_DX_SET_VS_CONSTANT_BUFFER_OFFSET, w);
    case SVGA_3D_CMD_DX_SET_SHADER_RESOURCES:
        if (!shader_type(w[1]) || w[0] > SVGA3D_DX_MAX_SRVIEWS - (uint32_t)count)
            return -EINVAL;
        return bind_entries(batch, SVGA_COTABLE_SRVIEW, SVGA_BIND_SRV,
                            w[1] * SVGA3D_DX_MAX_SRVIEWS + w[0], w + 2, count);
    case SVGA_3D_CMD_DX_SET_SHADER: return set_shader(batch, p->body);
    case SVGA_3D_CMD_DX_SET_SAMPLERS:
        if (!shader_type(w[1]) || w[0] > SVGA3D_DX_MAX_SAMPLERS - (uint32_t)count)
            return -EINVAL;
        for (int i = 0; i < count; ++i)
            if (simple_id(batch, SVGA_COTABLE_SAMPLER, w[i + 2]))
                return -EINVAL;
        return 0;
    case SVGA_3D_CMD_DX_SET_VERTEX_BUFFERS:
        if (w[0] > SVGA3D_DX_MAX_VERTEXBUFFERS - (uint32_t)count)
            return -EINVAL;
        for (int i = 0; i < count; ++i) {
            uint32_t *entry = w + 1 + i * 3;
            int ret = bind_buffer(batch, SVGA_BIND_VERTEX, w[0] + i, entry, entry[2], 0, entry[1]);
            if (ret)
                return ret;
        }
        return 0;
    case SVGA_3D_CMD_DX_SET_INDEX_BUFFER:
        if (w[0] != SVGA3D_INVALID_ID && w[1] != SVGA3D_R16_UINT && w[1] != SVGA3D_R32_UINT)
            return -EINVAL;
        return bind_buffer(batch, SVGA_BIND_INDEX, 0, w, w[2], 0, w[1]);
    case SVGA_3D_CMD_DX_SET_RENDERTARGETS: {
        int ret = bind_table_entry(batch, SVGA_COTABLE_DSVIEW, w[0], SVGA_BIND_DS, 0);
        if (!ret)
            ret = bind_entries(batch, SVGA_COTABLE_RTVIEW, SVGA_BIND_RT, 0, w + 1, count);
        for (unsigned i = count; !ret && i < SVGA3D_DX_MAX_RENDER_TARGETS; ++i)
            ret = bind_table_entry(batch, SVGA_COTABLE_RTVIEW, SVGA3D_INVALID_ID, SVGA_BIND_RT, i);
        return ret;
    }
    case SVGA_3D_CMD_DX_SET_SOTARGETS:
        for (unsigned i = 0; i < SVGA3D_DX_MAX_SOTARGETS; ++i) {
            uint32_t *entry = w + 1 + i * 3;
            int ret = i < (unsigned)count ? bind_buffer(batch, SVGA_BIND_SO_TARGET, i, entry, entry[1], entry[2], 0) :
                svga_context_bind_object(batch, batch->dx, SVGA_BIND_SO_TARGET, i, NULL);
            if (ret)
                return ret;
        }
        return 0;
    case SVGA_3D_CMD_DX_SET_UA_VIEWS:
    case SVGA_3D_CMD_DX_SET_CS_UA_VIEWS: {
        bool compute = p->command == SVGA_3D_CMD_DX_SET_CS_UA_VIEWS;
        const unsigned limit = SVGA3D_MAX_UAVIEWS;
        if (w[0] > limit)
            return -EINVAL;
        if (compute) {
            if ((unsigned)count > limit - w[0])
                return -EINVAL;
            return bind_entries(batch, SVGA_COTABLE_UAVIEW, SVGA_BIND_CS_UAV, w[0], w + 1, count);
        }
        /* The graphics splice control is separate from the array length.
         * Mesa sends the full table with INVALID_ID in unused entries. */
        int ret = bind_entries(batch, SVGA_COTABLE_UAVIEW, SVGA_BIND_UAV, 0, w + 1, count);
        for (unsigned i = count; !ret && i < limit; ++i)
            ret = bind_table_entry(batch, SVGA_COTABLE_UAVIEW, SVGA3D_INVALID_ID, SVGA_BIND_UAV, i);
        if (!ret)
            ret = svga_context_stage(batch, batch->dx, SVGA_DX_UAV_SPLICE, 0,
                (struct svga_context_value){.defined = true, .id = w[0]});
        return ret;
    }
    case SVGA_3D_CMD_DX_SET_BLEND_STATE: return simple_id(batch, SVGA_COTABLE_BLENDSTATE, w[0]);
    case SVGA_3D_CMD_DX_SET_DEPTHSTENCIL_STATE: return simple_id(batch, SVGA_COTABLE_DEPTHSTENCIL, w[0]);
    case SVGA_3D_CMD_DX_SET_RASTERIZER_STATE: return simple_id(batch, SVGA_COTABLE_RASTERIZERSTATE, w[0]);
    case SVGA_3D_CMD_DX_SET_INPUT_LAYOUT: return simple_id(batch, SVGA_COTABLE_ELEMENTLAYOUT, w[0]);
    case SVGA_3D_CMD_DX_DEFINE_QUERY:
    case SVGA_3D_CMD_DX_DESTROY_QUERY:
    case SVGA_3D_CMD_DX_BIND_QUERY:
    case SVGA_3D_CMD_DX_SET_QUERY_OFFSET:
    case SVGA_3D_CMD_DX_BEGIN_QUERY:
    case SVGA_3D_CMD_DX_END_QUERY:
    case SVGA_3D_CMD_DX_SET_PREDICATION: return dx_query(batch, p->command, w);
    case SVGA_3D_CMD_DX_CLEAR_RENDERTARGET_VIEW: return write_view(batch, SVGA_COTABLE_RTVIEW, w[0]);
    case SVGA_3D_CMD_DX_CLEAR_DEPTHSTENCIL_VIEW: return write_view(batch, SVGA_COTABLE_DSVIEW, w[1]);
    case SVGA_3D_CMD_DX_CLEAR_UA_VIEW_UINT:
    case SVGA_3D_CMD_DX_CLEAR_UA_VIEW_FLOAT: return write_view(batch, SVGA_COTABLE_UAVIEW, w[0]);
    case SVGA_3D_CMD_DX_GENMIPS: return write_view(batch, SVGA_COTABLE_SRVIEW, w[0]);
    case SVGA_3D_CMD_DX_UPDATE_SUBRESOURCE:
    case SVGA_3D_CMD_DX_READBACK_SUBRESOURCE:
    case SVGA_3D_CMD_DX_INVALIDATE_SUBRESOURCE: {
        SVGA3dSize size;
        int ret = subresource(batch, w, w[1], NULL, &size);
        if (!ret && p->command == SVGA_3D_CMD_DX_UPDATE_SUBRESOURCE &&
            !box_fits(&size, &((const SVGA3dCmdDXUpdateSubResource *)p->body)->box))
            ret = -EINVAL;
        return ret;
    }
    case SVGA_3D_CMD_DX_DEFINE_SHADERRESOURCE_VIEW: return define_view(batch, SVGA_COTABLE_SRVIEW, p);
    case SVGA_3D_CMD_DX_DEFINE_RENDERTARGET_VIEW: return define_view(batch, SVGA_COTABLE_RTVIEW, p);
    case SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_VIEW:
    case SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_VIEW_V2: return define_view(batch, SVGA_COTABLE_DSVIEW, p);
    case SVGA_3D_CMD_DX_DEFINE_UA_VIEW: return define_view(batch, SVGA_COTABLE_UAVIEW, p);
    case SVGA_3D_CMD_DX_DESTROY_SHADERRESOURCE_VIEW: return destroy_view(batch, SVGA_COTABLE_SRVIEW, p);
    case SVGA_3D_CMD_DX_DESTROY_RENDERTARGET_VIEW: return destroy_view(batch, SVGA_COTABLE_RTVIEW, p);
    case SVGA_3D_CMD_DX_DESTROY_DEPTHSTENCIL_VIEW: return destroy_view(batch, SVGA_COTABLE_DSVIEW, p);
    case SVGA_3D_CMD_DX_DESTROY_UA_VIEW: return destroy_view(batch, SVGA_COTABLE_UAVIEW, p);
    case SVGA_3D_CMD_DX_DEFINE_ELEMENTLAYOUT: return define(batch, SVGA_COTABLE_ELEMENTLAYOUT, w[0], (struct svga_context_value){0});
    case SVGA_3D_CMD_DX_DEFINE_BLEND_STATE: return define(batch, SVGA_COTABLE_BLENDSTATE, w[0], (struct svga_context_value){0});
    case SVGA_3D_CMD_DX_DEFINE_DEPTHSTENCIL_STATE: return define(batch, SVGA_COTABLE_DEPTHSTENCIL, w[0], (struct svga_context_value){0});
    case SVGA_3D_CMD_DX_DEFINE_RASTERIZER_STATE:
    case SVGA_3D_CMD_DX_DEFINE_RASTERIZER_STATE_V2: return define(batch, SVGA_COTABLE_RASTERIZERSTATE, w[0], (struct svga_context_value){0});
    case SVGA_3D_CMD_DX_DEFINE_SAMPLER_STATE: return define(batch, SVGA_COTABLE_SAMPLER, w[0], (struct svga_context_value){0});
    case SVGA_3D_CMD_DX_DESTROY_ELEMENTLAYOUT: return destroy(batch, SVGA_COTABLE_ELEMENTLAYOUT, w[0]);
    case SVGA_3D_CMD_DX_DESTROY_BLEND_STATE: return destroy(batch, SVGA_COTABLE_BLENDSTATE, w[0]);
    case SVGA_3D_CMD_DX_DESTROY_DEPTHSTENCIL_STATE: return destroy(batch, SVGA_COTABLE_DEPTHSTENCIL, w[0]);
    case SVGA_3D_CMD_DX_DESTROY_RASTERIZER_STATE: return destroy(batch, SVGA_COTABLE_RASTERIZERSTATE, w[0]);
    case SVGA_3D_CMD_DX_DESTROY_SAMPLER_STATE: return destroy(batch, SVGA_COTABLE_SAMPLER, w[0]);
    case SVGA_3D_CMD_DX_DEFINE_SHADER:
        if (!shader_type(w[1]) || !w[2] || (w[2] & 3) || w[2] > SVGA3D_MAX_SHADER_MEMORY_BYTES)
            return -EINVAL;
        return define(batch, SVGA_COTABLE_DXSHADER, w[0], (struct svga_context_value){.type = w[1], .size = w[2]});
    case SVGA_3D_CMD_DX_DESTROY_SHADER: return destroy(batch, SVGA_COTABLE_DXSHADER, w[0]);
    case SVGA_3D_CMD_DX_DEFINE_STREAMOUTPUT:
        if (w[1] > SVGA3D_MAX_DX10_STREAMOUT_DECLS)
            return -EINVAL;
        return define(batch, SVGA_COTABLE_STREAMOUTPUT, w[0], (struct svga_context_value){0});
    case SVGA_3D_CMD_DX_DEFINE_STREAMOUTPUT_WITH_MOB:
        if (w[2] > SVGA3D_DX_MAX_SOTARGETS || w[1] > svga_dma_max_mob_size() / sizeof(SVGA3dStreamOutputDeclarationEntry))
            return -EINVAL;
        return define(batch, SVGA_COTABLE_STREAMOUTPUT, w[0],
                      (struct svga_context_value){.size = w[1] * sizeof(SVGA3dStreamOutputDeclarationEntry)});
    case SVGA_3D_CMD_DX_BIND_STREAMOUTPUT: {
        const struct svga_context_value *so = svga_context_lookup(batch->dx, SVGA_COTABLE_STREAMOUTPUT, w[0]);
        if (!so || !so->size || w[3] < so->size)
            return -EINVAL;
        return bind_mob(batch, batch->dx, SVGA_COTABLE_STREAMOUTPUT, w[0], &w[1], w[2], w[3]);
    }
    case SVGA_3D_CMD_DX_DESTROY_STREAMOUTPUT: return destroy(batch, SVGA_COTABLE_STREAMOUTPUT, w[0]);
    case SVGA_3D_CMD_DX_SET_STREAMOUTPUT: return bind_table_entry(batch, SVGA_COTABLE_STREAMOUTPUT, w[0], SVGA_BIND_DX_SO, 0);
    case SVGA_3D_CMD_DX_BUFFER_COPY:
    case SVGA_3D_CMD_DX_PRED_COPY_REGION:
    case SVGA_3D_CMD_DX_TRANSFER_FROM_BUFFER: return dx_copy(batch, p->command, w);
    case SVGA_3D_CMD_DX_DRAW_INDEXED_INSTANCED_INDIRECT:
    case SVGA_3D_CMD_DX_DRAW_INSTANCED_INDIRECT:
    case SVGA_3D_CMD_DX_DISPATCH_INDIRECT: {
        unsigned words = p->command == SVGA_3D_CMD_DX_DISPATCH_INDIRECT ? 3 :
            p->command == SVGA_3D_CMD_DX_DRAW_INSTANCED_INDIRECT ? 4 : 5;
        return (w[1] & 3) ? -EINVAL : get_buffer(batch, w, false, w[1], words * sizeof(uint32_t), NULL);
    }
    case SVGA_3D_CMD_DX_DISPATCH:
        for (unsigned i = 0; i < 3; ++i)
            if (w[i] > SVGA3D_MAX_SHADER_THREAD_GROUPS)
                return -EINVAL;
        return 0;
    case SVGA_3D_CMD_DX_SET_TOPOLOGY: return w[0] < SVGA3D_PRIMITIVE_MAX ? 0 : -EINVAL;
    case SVGA_3D_CMD_DX_SET_VIEWPORTS:
    case SVGA_3D_CMD_DX_SET_SCISSORRECTS:
    case SVGA_3D_CMD_DX_DRAW:
    case SVGA_3D_CMD_DX_DRAW_INDEXED:
    case SVGA_3D_CMD_DX_DRAW_INSTANCED:
    case SVGA_3D_CMD_DX_DRAW_INDEXED_INSTANCED:
    case SVGA_3D_CMD_DX_DRAW_AUTO: return 0;
    default: return -ENOTTY;
    }
}
