#include "execbuf_internal.h"

static struct drm_vmwgfx_context_ref *drm_vmwgfx_exec_find_context(
    struct drm_vmwgfx_exec_work *work, uint32_t drm_id);

static struct drm_vmwgfx_bo_ref *drm_vmwgfx_exec_find_bo(
    struct drm_vmwgfx_exec_work *work, uint32_t drm_id);


static struct drm_vmwgfx_context_ref *drm_vmwgfx_exec_find_context(
    struct drm_vmwgfx_exec_work *work, uint32_t drm_id)
{
    for (uint32_t i = 0; i < work->context_count; ++i)
        if (work->contexts[i].drm_id == drm_id) return &work->contexts[i];
    return NULL;
}

struct drm_vmwgfx_surface_ref *drm_vmwgfx_exec_find_surface(
    struct drm_vmwgfx_exec_work *work, uint32_t drm_id)
{
    for (uint32_t i = 0; i < work->surface_count; ++i)
        if (work->surfaces[i].drm_id == drm_id) return &work->surfaces[i];
    return NULL;
}

static struct drm_vmwgfx_bo_ref *drm_vmwgfx_exec_find_bo(
    struct drm_vmwgfx_exec_work *work, uint32_t drm_id)
{
    for (uint32_t i = 0; i < work->bo_count; ++i)
        if (work->bos[i].drm_id == drm_id) return &work->bos[i];
    return NULL;
}

int drm_vmwgfx_exec_hold_context(struct drm_vmwgfx_exec_work *work,
                                         struct drm_file *file,
                                         uint32_t drm_id,
                                         struct drm_vmwgfx_context_ref **out)
{
    if (!drm_id || drm_id == SVGA3D_INVALID_ID) return -LINUX_EINVAL;
    struct drm_vmwgfx_context_ref *entry =
        drm_vmwgfx_exec_find_context(work, drm_id);
    if (entry) { if (out) *out = entry; return 0; }
    if (work->context_count >= DRM_VMW_EXEC_CONTEXTS_MAX)
        return -LINUX_ENOMEM;
    struct drm_vmwgfx_context *object =
        drm_vmwgfx_context_lookup_get(file, drm_id);
    if (!object) return -LINUX_EINVAL;
    svga_handle backend = drm_vmwgfx_context_backend_handle(object);
    uint32_t backend_id = drm_vmwgfx_context_backend_id(object);
    if (!backend || backend_id == SVGA3D_INVALID_ID) {
        drm_vmwgfx_context_put(object);
        return -LINUX_EINVAL;
    }
    entry = &work->contexts[work->context_count++];
    *entry = (struct drm_vmwgfx_context_ref){
        .object = object, .drm_id = drm_id,
        .backend_id = backend_id, .backend = backend,
    };
    if (out) *out = entry;
    return 0;
}

int drm_vmwgfx_exec_hold_surface(struct drm_vmwgfx_exec_work *work,
                                         struct drm_file *file,
                                         uint32_t drm_id,
                                         uint32_t face, uint32_t mip,
                                         struct drm_vmwgfx_surface_ref **out)
{
    if (!drm_id || drm_id == SVGA3D_INVALID_ID) return -LINUX_EINVAL;
    struct drm_vmwgfx_surface_ref *entry =
        drm_vmwgfx_exec_find_surface(work, drm_id);
    if (!entry) {
        if (work->surface_count >= DRM_VMW_EXEC_SURFACES_MAX)
            return -LINUX_ENOMEM;
        struct drm_vmwgfx_surface *object =
            drm_vmwgfx_surface_lookup_get(file, drm_id);
        if (!object) return -LINUX_EINVAL;
        entry = &work->surfaces[work->surface_count++];
        *entry = (struct drm_vmwgfx_surface_ref){.object = object,
            .drm_id = drm_id,
            .backend = drm_vmwgfx_surface_backend_handle(object),
            .backend_id = drm_vmwgfx_surface_backend_id(object)};
        if (!entry->backend || entry->backend_id == SVGA3D_INVALID_ID ||
            drm_vmwgfx_surface_get_metadata(object, &entry->metadata) != 0) {
            drm_vmwgfx_surface_put(object);
            *entry = (struct drm_vmwgfx_surface_ref){0};
            --work->surface_count;
            return -LINUX_EINVAL;
        }
    }
    struct drm_vmwgfx_surface_subresource subresource;
    if (drm_vmwgfx_surface_get_subresource(entry->object, face, mip,
                                           &subresource) != 0)
        return -LINUX_EINVAL;
    if (out) *out = entry;
    return 0;
}

int drm_vmwgfx_exec_hold_bo(struct drm_vmwgfx_exec_work *work,
                                    struct drm_file *file, uint32_t drm_id,
                                    struct drm_vmwgfx_bo_ref **out)
{
    if (!drm_id || drm_id >= 0x01000000u) return -LINUX_EINVAL;
    struct drm_vmwgfx_bo_ref *entry =
        drm_vmwgfx_exec_find_bo(work, drm_id);
    if (entry) { if (out) *out = entry; return 0; }
    if (work->bo_count >= DRM_VMW_EXEC_BOS_MAX) return -LINUX_ENOMEM;
    struct drm_bo *object = drm_bo_lookup_handle_get(file, drm_id);
    if (!object) return -LINUX_EINVAL;
    int ret = drm_bo_gpu_reserve(object);
    if (ret) {
        drm_bo_put(object);
        return ret;
    }
    entry = &work->bos[work->bo_count++];
    *entry = (struct drm_vmwgfx_bo_ref){.object = object, .drm_id = drm_id};
    if (out) *out = entry;
    return 0;
}

int drm_vmwgfx_exec_translate_packet(uint32_t command,
                                              const uint8_t *body,
                                              uint32_t body_bytes,
                                              uint32_t packet_offset,
                                              void *opaque)
{
    struct drm_vmwgfx_exec_walk *walk = opaque;
    struct drm_vmwgfx_exec_work *work = walk->work;
    uint8_t *translated = work->scratch + packet_offset + 8u;
    const uint8_t *source = body;
    if (body_bytes) __builtin_memcpy(translated, source, body_bytes);
    uint32_t context_id = 0;
    struct drm_vmwgfx_context_ref *context = NULL;
    if (drm_vmwgfx_exec_has_context(command)) {
        __builtin_memcpy(&context_id, translated, sizeof(context_id));
        context = drm_vmwgfx_exec_find_context(work, context_id);
        if (!context) return -LINUX_EINVAL;
        __builtin_memcpy(translated, &context->backend_id,
                         sizeof(context->backend_id));
    }

    switch (command) {
    case SVGA_3D_CMD_SURFACE_COPY: {
        SVGA3dSurfaceImageId images[2];
        __builtin_memcpy(&images[0], translated, sizeof(images[0]));
        __builtin_memcpy(&images[1], translated + sizeof(images[0]),
                         sizeof(images[1]));
        struct drm_vmwgfx_surface_ref *a =
            drm_vmwgfx_exec_find_surface(work, images[0].sid);
        struct drm_vmwgfx_surface_ref *b =
            drm_vmwgfx_exec_find_surface(work, images[1].sid);
        if (!a || !b) return -LINUX_EINVAL;
        images[0].sid = a->backend_id;
        images[1].sid = b->backend_id;
        __builtin_memcpy(translated, images, sizeof(images));
        break;
    }
    case SVGA_3D_CMD_SURFACE_STRETCHBLT: {
        SVGA3dCmdSurfaceStretchBlt packet;
        __builtin_memcpy(&packet, translated, sizeof(packet));
        struct drm_vmwgfx_surface_ref *a =
            drm_vmwgfx_exec_find_surface(work, packet.src.sid);
        struct drm_vmwgfx_surface_ref *b =
            drm_vmwgfx_exec_find_surface(work, packet.dest.sid);
        if (!a || !b) return -LINUX_EINVAL;
        packet.src.sid = a->backend_id;
        packet.dest.sid = b->backend_id;
        __builtin_memcpy(translated, &packet, sizeof(packet));
        break;
    }
    case SVGA_3D_CMD_SURFACE_DMA: {
        SVGA3dCmdSurfaceDMA packet;
        __builtin_memcpy(&packet, translated, sizeof(packet));
        uint32_t boxes_bytes = body_bytes -
                              sizeof(SVGA3dCmdSurfaceDMASuffix);
        SVGA3dCmdSurfaceDMASuffix suffix;
        __builtin_memcpy(&suffix, translated + boxes_bytes,
                         sizeof(suffix));
        struct drm_vmwgfx_surface_ref *host =
            drm_vmwgfx_exec_find_surface(work, packet.host.sid);
        struct drm_vmwgfx_bo_ref *bo =
            drm_vmwgfx_exec_find_bo(work, packet.guest.ptr.gmrId);
        if (!host || !bo || !bo->attachment || !bo->attachment->ready)
            return -LINUX_EINVAL;
        packet.host.sid = host->backend_id;
        packet.guest.ptr.gmrId = svga_resource_id(bo->attachment->gmr);
        if (packet.guest.ptr.gmrId == SVGA3D_INVALID_ID)
            return -LINUX_EINVAL;
        uint64_t bo_size = drm_bo_size(bo->object);
        if (packet.guest.ptr.offset > bo_size) return -LINUX_EINVAL;
        uint64_t available = bo_size - packet.guest.ptr.offset;
        if (suffix.maximumOffset > available)
            suffix.maximumOffset = available > UINT32_MAX
                ? UINT32_MAX : (uint32_t)available;
        __builtin_memcpy(translated, &packet, sizeof(packet));
        __builtin_memcpy(translated + boxes_bytes, &suffix, sizeof(suffix));
        return drm_vmwgfx_map_svga_status(svga_batch_append(&work->batch,
            command, true, translated, body_bytes));
    }
    case SVGA_3D_CMD_SETRENDERTARGET: {
        SVGA3dCmdSetRenderTarget packet;
        __builtin_memcpy(&packet, translated, sizeof(packet));
        struct drm_vmwgfx_surface_ref *surface = NULL;
        svga_handle surface_handle = SVGA_INVALID_HANDLE;
        if (packet.target.sid != SVGA3D_INVALID_ID) {
            surface = drm_vmwgfx_exec_find_surface(work, packet.target.sid);
            if (!surface) return -LINUX_EINVAL;
            surface_handle = surface->backend;
            packet.target.sid = surface->backend_id;
        }
        int ret = drm_vmwgfx_map_svga_status(svga_batch_stage_render_target(
            &work->batch, context->backend, packet.type, surface_handle,
            packet.target.face, packet.target.mipmap));
        if (ret) return ret;
        __builtin_memcpy(translated, &packet, sizeof(packet));
        break;
    }
    case SVGA_3D_CMD_SETTEXTURESTATE: {
        uint32_t count = (body_bytes - sizeof(uint32_t)) /
                         sizeof(SVGA3dTextureState);
        SVGA3dTextureState *states =
            (SVGA3dTextureState *)(translated + sizeof(uint32_t));
        for (uint32_t i = 0; i < count; ++i) {
            if (states[i].name != SVGA3D_TS_BIND_TEXTURE) continue;
            svga_handle surface_handle = SVGA_INVALID_HANDLE;
            if (states[i].value != SVGA3D_INVALID_ID) {
                struct drm_vmwgfx_surface_ref *surface =
                    drm_vmwgfx_exec_find_surface(work, states[i].value);
                if (!surface) return -LINUX_EINVAL;
                states[i].value = surface->backend_id;
                surface_handle = surface->backend;
            }
            int ret = drm_vmwgfx_map_svga_status(svga_batch_stage_texture(
                &work->batch, context->backend, states[i].stage,
                surface_handle));
            if (ret) return ret;
        }
        break;
    }
    case SVGA_3D_CMD_DRAW_PRIMITIVES: {
        SVGA3dCmdDrawPrimitives header;
        __builtin_memcpy(&header, translated, sizeof(header));
        SVGA3dVertexDecl *decls =
            (SVGA3dVertexDecl *)(translated + sizeof(header));
        SVGA3dPrimitiveRange *ranges =
            (SVGA3dPrimitiveRange *)(decls + header.numVertexDecls);
        for (uint32_t i = 0; i < header.numVertexDecls; ++i) {
            struct drm_vmwgfx_surface_ref *surface =
                drm_vmwgfx_exec_find_surface(work,
                    decls[i].array.surfaceId);
            if (!surface) return -LINUX_EINVAL;
            decls[i].array.surfaceId = surface->backend_id;
        }
        for (uint32_t i = 0; i < header.numRanges; ++i) {
            if (ranges[i].indexArray.surfaceId == SVGA3D_INVALID_ID) continue;
            struct drm_vmwgfx_surface_ref *surface =
                drm_vmwgfx_exec_find_surface(work,
                    ranges[i].indexArray.surfaceId);
            if (!surface) return -LINUX_EINVAL;
            ranges[i].indexArray.surfaceId = surface->backend_id;
        }
        break;
    }
    case SVGA_3D_CMD_SHADER_DEFINE: {
        SVGA3dCmdDefineShader packet;
        __builtin_memcpy(&packet, translated, sizeof(packet));
        return drm_vmwgfx_map_svga_status(svga_batch_shader_define(
            &work->batch, context->backend, packet.shid, packet.type,
            translated + sizeof(packet), body_bytes - sizeof(packet)));
    }
    case SVGA_3D_CMD_SHADER_DESTROY: {
        SVGA3dCmdDestroyShader packet;
        __builtin_memcpy(&packet, translated, sizeof(packet));
        return drm_vmwgfx_map_svga_status(svga_batch_shader_destroy(
            &work->batch, context->backend, packet.shid, packet.type));
    }
    case SVGA_3D_CMD_SET_SHADER: {
        SVGA3dCmdSetShader packet;
        __builtin_memcpy(&packet, translated, sizeof(packet));
        return drm_vmwgfx_map_svga_status(svga_batch_bind_shader(
            &work->batch, context->backend, packet.type, packet.shid));
    }
    case SVGA_3D_CMD_SET_SHADER_CONST: {
        SVGA3dCmdSetShaderConst packet;
        __builtin_memcpy(&packet, translated, sizeof(packet));
        uint32_t registers = 1u + (body_bytes - sizeof(packet)) / 16u;
        /* The batch helper copies all vectors synchronously from the snapshot. */
        const uint32_t *values = (const uint32_t *)(translated +
            __builtin_offsetof(SVGA3dCmdSetShaderConst, values));
        return drm_vmwgfx_map_svga_status(svga_batch_shader_constants(
            &work->batch, context->backend, packet.reg, packet.type,
            packet.ctype, values, registers));
    }
    case SVGA_3D_CMD_BEGIN_QUERY:
    case SVGA_3D_CMD_END_QUERY:
    case SVGA_3D_CMD_WAIT_FOR_QUERY: {
        SVGA3dQueryType type;
        __builtin_memcpy(&type, translated + sizeof(uint32_t), sizeof(type));
        if (command == SVGA_3D_CMD_BEGIN_QUERY)
            return drm_vmwgfx_map_svga_status(svga_batch_query_begin(
                &work->batch, context->backend, type));
        SVGAGuestPtr result;
        __builtin_memcpy(&result, translated + 2u * sizeof(uint32_t),
                         sizeof(result));
        struct drm_vmwgfx_bo_ref *bo =
            drm_vmwgfx_exec_find_bo(work, result.gmrId);
        if (!bo || !bo->attachment || !bo->attachment->ready)
            return -LINUX_EINVAL;
        int ret = command == SVGA_3D_CMD_END_QUERY
            ? svga_batch_query_end(&work->batch, context->backend, type,
                                   bo->attachment->gmr, result.offset)
            : svga_batch_query_wait(&work->batch, context->backend, type,
                                    bo->attachment->gmr, result.offset);
        return drm_vmwgfx_map_svga_status(ret);
    }
    default:
        break;
    }
    return drm_vmwgfx_map_svga_status(svga_batch_append(&work->batch,
        command, true, translated, body_bytes));
}
