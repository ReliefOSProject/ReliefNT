#include "execbuf_internal.h"

static bool drm_vmwgfx_add_u64(uint64_t left, uint64_t right,
                               uint64_t *result);

static bool drm_vmwgfx_mul_u64(uint64_t left, uint64_t right,
                               uint64_t *result);

static uint32_t drm_vmwgfx_surface_element_bytes(uint32_t type);

static bool drm_vmwgfx_exec_surface_is_buffer(
    const struct drm_vmwgfx_surface_ref *surface);

static int drm_vmwgfx_exec_validate_array(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const SVGA3dArray *array, uint32_t type);

static int drm_vmwgfx_exec_surface_image(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const SVGA3dSurfaceImageId *image,
    struct drm_vmwgfx_surface_ref **surface_out,
    struct drm_vmwgfx_surface_subresource *subresource_out);

static bool drm_vmwgfx_exec_box_fits(uint32_t x, uint32_t y, uint32_t z,
                                     uint32_t width, uint32_t height,
                                     uint32_t depth,
                                     const struct drm_vmwgfx_surface_subresource *size);

static uint32_t drm_vmwgfx_exec_format_bytes(uint32_t format);

static int drm_vmwgfx_exec_validate_surface_copy(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const uint8_t *body, uint32_t bytes);

static int drm_vmwgfx_exec_validate_stretch(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const uint8_t *body);

static int drm_vmwgfx_exec_validate_dma(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const uint8_t *body, uint32_t bytes);

static uint32_t drm_vmwgfx_primitive_vertices(uint32_t type,
                                              uint32_t primitives);

static bool drm_vmwgfx_surface_has_hint(
    const struct drm_vmwgfx_surface_ref *surface, uint32_t hint);

static int drm_vmwgfx_exec_validate_draw(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const uint8_t *body);


static bool drm_vmwgfx_add_u64(uint64_t left, uint64_t right,
                               uint64_t *result)
{
    if (!result || left > UINT64_MAX - right) return false;
    *result = left + right;
    return true;
}

static bool drm_vmwgfx_mul_u64(uint64_t left, uint64_t right,
                               uint64_t *result)
{
    if (!result || (left && right > UINT64_MAX / left)) return false;
    *result = left * right;
    return true;
}

static uint32_t drm_vmwgfx_surface_element_bytes(uint32_t type)
{
    switch (type) {
    case SVGA3D_DECLTYPE_FLOAT1: return 4;
    case SVGA3D_DECLTYPE_FLOAT2: return 8;
    case SVGA3D_DECLTYPE_FLOAT3: return 12;
    case SVGA3D_DECLTYPE_FLOAT4: return 16;
    case SVGA3D_DECLTYPE_D3DCOLOR:
    case SVGA3D_DECLTYPE_UBYTE4:
    case SVGA3D_DECLTYPE_UBYTE4N:
    case SVGA3D_DECLTYPE_SHORT2:
    case SVGA3D_DECLTYPE_SHORT2N:
    case SVGA3D_DECLTYPE_USHORT2N:
    case SVGA3D_DECLTYPE_UDEC3:
    case SVGA3D_DECLTYPE_DEC3N:
    case SVGA3D_DECLTYPE_FLOAT16_2: return 4;
    case SVGA3D_DECLTYPE_SHORT4:
    case SVGA3D_DECLTYPE_SHORT4N:
    case SVGA3D_DECLTYPE_USHORT4N:
    case SVGA3D_DECLTYPE_FLOAT16_4: return 8;
    default: return 0;
    }
}

static bool drm_vmwgfx_exec_surface_is_buffer(
    const struct drm_vmwgfx_surface_ref *surface)
{
    return surface && surface->metadata.desc.format == SVGA3D_BUFFER &&
           surface->metadata.desc.width != 0;
}

static int drm_vmwgfx_exec_validate_array(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const SVGA3dArray *array, uint32_t type)
{
    struct drm_vmwgfx_surface_ref *surface;
    int ret = drm_vmwgfx_exec_hold_surface(work, file, array->surfaceId,
                                            0, 0, &surface);
    if (ret) return ret;
    if (!drm_vmwgfx_exec_surface_is_buffer(surface) || !array->stride)
        return -LINUX_EINVAL;
    uint32_t element = drm_vmwgfx_surface_element_bytes(type);
    uint64_t size = surface->metadata.desc.width;
    uint64_t end;
    if (!element || array->stride < element || array->offset > size ||
        !drm_vmwgfx_add_u64(array->offset, element, &end) || end > size)
        return -LINUX_EINVAL;
    return 0;
}

bool drm_vmwgfx_exec_has_context(uint32_t command)
{
    return command != SVGA_3D_CMD_SURFACE_COPY &&
           command != SVGA_3D_CMD_SURFACE_STRETCHBLT &&
           command != SVGA_3D_CMD_SURFACE_DMA;
}

static int drm_vmwgfx_exec_surface_image(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const SVGA3dSurfaceImageId *image,
    struct drm_vmwgfx_surface_ref **surface_out,
    struct drm_vmwgfx_surface_subresource *subresource_out)
{
    if (!image || !surface_out || !subresource_out) return -LINUX_EINVAL;
    struct drm_vmwgfx_surface_ref *surface;
    int ret = drm_vmwgfx_exec_hold_surface(work, file, image->sid,
        image->face, image->mipmap, &surface);
    if (ret) return ret;
    ret = drm_vmwgfx_surface_get_subresource(surface->object, image->face,
                                              image->mipmap, subresource_out);
    if (ret) return -LINUX_EINVAL;
    *surface_out = surface;
    return 0;
}

static bool drm_vmwgfx_exec_box_fits(uint32_t x, uint32_t y, uint32_t z,
                                     uint32_t width, uint32_t height,
                                     uint32_t depth,
                                     const struct drm_vmwgfx_surface_subresource *size)
{
    return size && width && height && depth && x <= size->width &&
           width <= size->width - x && y <= size->height &&
           height <= size->height - y && z <= size->depth &&
           depth <= size->depth - z;
}

static uint32_t drm_vmwgfx_exec_format_bytes(uint32_t format)
{
    switch (format) {
    case SVGA3D_BUFFER: return 1;
    case SVGA3D_ALPHA8: return 1;
    case SVGA3D_Z_D16: return 2;
    case SVGA3D_X8R8G8B8:
    case SVGA3D_A8R8G8B8:
    case SVGA3D_Z_D24S8: return 4;
    default: return 0;
    }
}

static int drm_vmwgfx_exec_validate_surface_copy(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const uint8_t *body, uint32_t bytes)
{
    SVGA3dCmdSurfaceCopy copy;
    __builtin_memcpy(&copy, body, sizeof(copy));
    struct drm_vmwgfx_surface_ref *source, *destination;
    struct drm_vmwgfx_surface_subresource source_size, destination_size;
    int ret = drm_vmwgfx_exec_surface_image(work, file, &copy.src,
                                             &source, &source_size);
    if (ret) return ret;
    ret = drm_vmwgfx_exec_surface_image(work, file, &copy.dest,
                                       &destination, &destination_size);
    if (ret) return ret;
    if (source->metadata.desc.format != destination->metadata.desc.format)
        return -LINUX_EINVAL;
    uint32_t count = (bytes - sizeof(copy)) / sizeof(SVGA3dCopyBox);
    const SVGA3dCopyBox *boxes =
        (const SVGA3dCopyBox *)(body + sizeof(copy));
    for (uint32_t i = 0; i < count; ++i) {
        const SVGA3dCopyBox *box = &boxes[i];
        if (!drm_vmwgfx_exec_box_fits(box->srcx, box->srcy, box->srcz,
                box->w, box->h, box->d, &source_size) ||
            !drm_vmwgfx_exec_box_fits(box->x, box->y, box->z,
                box->w, box->h, box->d, &destination_size))
            return -LINUX_EINVAL;
    }
    return 0;
}

static int drm_vmwgfx_exec_validate_stretch(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const uint8_t *body)
{
    SVGA3dCmdSurfaceStretchBlt stretch;
    __builtin_memcpy(&stretch, body, sizeof(stretch));
    if (stretch.mode >= SVGA3D_STRETCH_BLT_MAX) return -LINUX_EINVAL;
    struct drm_vmwgfx_surface_ref *source, *destination;
    struct drm_vmwgfx_surface_subresource source_size, destination_size;
    int ret = drm_vmwgfx_exec_surface_image(work, file, &stretch.src,
                                             &source, &source_size);
    if (ret) return ret;
    ret = drm_vmwgfx_exec_surface_image(work, file, &stretch.dest,
                                       &destination, &destination_size);
    if (ret) return ret;
    (void)source;
    (void)destination;
    if (!drm_vmwgfx_exec_box_fits(stretch.boxSrc.x, stretch.boxSrc.y,
            stretch.boxSrc.z, stretch.boxSrc.w, stretch.boxSrc.h,
            stretch.boxSrc.d, &source_size) ||
        !drm_vmwgfx_exec_box_fits(stretch.boxDest.x, stretch.boxDest.y,
            stretch.boxDest.z, stretch.boxDest.w, stretch.boxDest.h,
            stretch.boxDest.d, &destination_size))
        return -LINUX_EINVAL;
    return 0;
}

static int drm_vmwgfx_exec_validate_dma(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const uint8_t *body, uint32_t bytes)
{
    SVGA3dCmdSurfaceDMA dma;
    __builtin_memcpy(&dma, body, sizeof(dma));
    uint32_t boxes_bytes = bytes - sizeof(SVGA3dCmdSurfaceDMASuffix);
    SVGA3dCmdSurfaceDMASuffix suffix;
    __builtin_memcpy(&suffix, body + boxes_bytes, sizeof(suffix));
    struct drm_vmwgfx_surface_ref *host;
    struct drm_vmwgfx_surface_subresource host_size;
    int ret = drm_vmwgfx_exec_surface_image(work, file, &dma.host,
                                             &host, &host_size);
    if (ret) return ret;
    uint32_t bytes_per_pixel =
        drm_vmwgfx_exec_format_bytes(host->metadata.desc.format);
    if (!bytes_per_pixel) return -LINUX_EOPNOTSUPP;
    struct drm_vmwgfx_bo_ref *bo;
    ret = drm_vmwgfx_exec_hold_bo(work, file, dma.guest.ptr.gmrId, &bo);
    if (ret) return ret;
    uint64_t bo_size = drm_bo_size(bo->object);
    if (dma.guest.ptr.offset > bo_size) return -LINUX_EINVAL;
    uint64_t available = bo_size - dma.guest.ptr.offset;
    if (suffix.maximumOffset > available)
        suffix.maximumOffset = available > UINT32_MAX
            ? UINT32_MAX : (uint32_t)available;
    uint32_t pitch = dma.guest.pitch;
    if (!pitch) {
        uint64_t tight_pitch;
        if (!drm_vmwgfx_mul_u64(host_size.width, bytes_per_pixel,
                                &tight_pitch) || tight_pitch > UINT32_MAX)
            return -LINUX_EINVAL;
        pitch = (uint32_t)tight_pitch;
    }
    uint32_t count = (boxes_bytes - sizeof(dma)) / sizeof(SVGA3dCopyBox);
    const SVGA3dCopyBox *boxes =
        (const SVGA3dCopyBox *)(body + sizeof(dma));
    for (uint32_t i = 0; i < count; ++i) {
        const SVGA3dCopyBox *box = &boxes[i];
        if (!drm_vmwgfx_exec_box_fits(box->x, box->y, box->z,
                box->w, box->h, box->d, &host_size))
            return -LINUX_EINVAL;
        uint64_t x_end, y_end, z_end, row_bytes;
        if (!drm_vmwgfx_add_u64(box->srcx, box->w, &x_end) ||
            !drm_vmwgfx_add_u64(box->srcy, box->h, &y_end) ||
            !drm_vmwgfx_add_u64(box->srcz, box->d, &z_end) ||
            !drm_vmwgfx_mul_u64(x_end, bytes_per_pixel, &row_bytes) ||
            row_bytes > pitch)
            return -LINUX_EINVAL;
        uint64_t slice_stride, row_index, base_index, byte_end;
        if (!drm_vmwgfx_mul_u64(pitch, host_size.height, &slice_stride) ||
            !drm_vmwgfx_mul_u64(z_end - 1u,
                                slice_stride, &base_index) ||
            !drm_vmwgfx_mul_u64(y_end - 1u, pitch, &row_index) ||
            !drm_vmwgfx_add_u64(base_index, row_index, &row_index) ||
            !drm_vmwgfx_add_u64(row_index, row_bytes, &byte_end) ||
            byte_end > suffix.maximumOffset || byte_end > available)
            return -LINUX_EINVAL;
    }
    return 0;
}

static uint32_t drm_vmwgfx_primitive_vertices(uint32_t type,
                                              uint32_t primitives)
{
    uint64_t count = 0;
    switch (type) {
    case SVGA3D_PRIMITIVE_POINTLIST: count = primitives; break;
    case SVGA3D_PRIMITIVE_LINELIST: count = (uint64_t)primitives * 2u; break;
    case SVGA3D_PRIMITIVE_TRIANGLELIST:
        count = (uint64_t)primitives * 3u;
        break;
    case SVGA3D_PRIMITIVE_LINESTRIP:
        count = primitives ? (uint64_t)primitives + 1u : 0;
        break;
    case SVGA3D_PRIMITIVE_TRIANGLESTRIP:
    case SVGA3D_PRIMITIVE_TRIANGLEFAN:
        count = primitives ? (uint64_t)primitives + 2u : 0;
        break;
    default: return UINT32_MAX;
    }
    return count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
}

static bool drm_vmwgfx_surface_has_hint(
    const struct drm_vmwgfx_surface_ref *surface, uint32_t hint)
{
    return surface && (surface->metadata.desc.flags & hint) != 0;
}

static int drm_vmwgfx_exec_validate_draw(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const uint8_t *body)
{
    SVGA3dCmdDrawPrimitives header;
    __builtin_memcpy(&header, body, sizeof(header));
    const SVGA3dVertexDecl *decls =
        (const SVGA3dVertexDecl *)(body + sizeof(header));
    const SVGA3dPrimitiveRange *ranges =
        (const SVGA3dPrimitiveRange *)(decls + header.numVertexDecls);
    for (uint32_t i = 0; i < header.numRanges; ++i) {
        const SVGA3dPrimitiveRange *range = &ranges[i];
        uint32_t vertices = drm_vmwgfx_primitive_vertices(
            range->primType, range->primitiveCount);
        if (vertices == UINT32_MAX) return -LINUX_EINVAL;
        if (range->indexArray.surfaceId == SVGA3D_INVALID_ID) {
            if (range->indexWidth) return -LINUX_EINVAL;
            continue;
        }
        if (range->indexWidth != 2u && range->indexWidth != 4u)
            return -LINUX_EINVAL;
        struct drm_vmwgfx_surface_ref *index_surface;
        int ret = drm_vmwgfx_exec_hold_surface(work, file,
            range->indexArray.surfaceId, 0, 0, &index_surface);
        if (ret) return ret;
        if (!drm_vmwgfx_exec_surface_is_buffer(index_surface) ||
            !(index_surface->metadata.desc.flags &
              SVGA3D_SURFACE_HINT_INDEXBUFFER))
            return -LINUX_EINVAL;
        uint64_t index_bytes, end;
        if (!drm_vmwgfx_mul_u64(vertices, range->indexWidth, &index_bytes) ||
            !drm_vmwgfx_add_u64(range->indexArray.offset, index_bytes, &end) ||
            end > index_surface->metadata.desc.width)
            return -LINUX_EINVAL;
    }

    for (uint32_t i = 0; i < header.numVertexDecls; ++i) {
        const SVGA3dVertexDecl *decl = &decls[i];
        int ret = drm_vmwgfx_exec_validate_array(work, file, &decl->array,
                                                  decl->identity.type);
        if (ret) return ret;
        struct drm_vmwgfx_surface_ref *surface =
            drm_vmwgfx_exec_find_surface(work, decl->array.surfaceId);
        if (!drm_vmwgfx_surface_has_hint(surface,
                                         SVGA3D_SURFACE_HINT_VERTEXBUFFER))
            return -LINUX_EINVAL;
        /* For non-indexed draws the index bias is the first vertex. On
         * indexed draws the index bytes are validated above; their values
         * remain guest GPU data and are not scanned without a coherent copy. */
        for (uint32_t r = 0; r < header.numRanges; ++r) {
            const SVGA3dPrimitiveRange *range = &ranges[r];
            if (range->indexArray.surfaceId != SVGA3D_INVALID_ID ||
                !range->primitiveCount)
                continue;
            int64_t first = range->indexBias;
            uint64_t last;
            uint64_t element = drm_vmwgfx_surface_element_bytes(
                decl->identity.type);
            if (first < 0 ||
                !drm_vmwgfx_add_u64((uint64_t)first,
                    drm_vmwgfx_primitive_vertices(range->primType,
                                                   range->primitiveCount) - 1u,
                    &last) ||
                !drm_vmwgfx_mul_u64(last, decl->array.stride, &last) ||
                !drm_vmwgfx_add_u64(decl->array.offset, last, &last) ||
                !drm_vmwgfx_add_u64(last, element, &last) ||
                last > surface->metadata.desc.width)
                return -LINUX_EINVAL;
        }
    }
    return 0;
}

int drm_vmwgfx_exec_validate_packet(uint32_t command,
                                            const uint8_t *body,
                                            uint32_t body_bytes,
                                            uint32_t packet_offset,
                                            void *opaque)
{
    (void)packet_offset;
    struct drm_vmwgfx_exec_walk *walk = opaque;
    struct drm_vmwgfx_exec_work *work = walk->work;
    struct drm_file *file = walk->file;
    if (drm_vmwgfx_exec_has_context(command)) {
        uint32_t cid;
        __builtin_memcpy(&cid, body, sizeof(cid));
        int ret = drm_vmwgfx_exec_hold_context(work, file, cid, NULL);
        if (ret) return ret;
    }

    switch (command) {
    case SVGA_3D_CMD_SURFACE_COPY:
        return drm_vmwgfx_exec_validate_surface_copy(work, file, body,
                                                      body_bytes);
    case SVGA_3D_CMD_SURFACE_STRETCHBLT:
        return drm_vmwgfx_exec_validate_stretch(work, file, body);
    case SVGA_3D_CMD_SURFACE_DMA:
        return drm_vmwgfx_exec_validate_dma(work, file, body, body_bytes);
    case SVGA_3D_CMD_SETRENDERTARGET: {
        SVGA3dCmdSetRenderTarget packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        if (packet.target.sid == SVGA3D_INVALID_ID) return 0;
        struct drm_vmwgfx_surface_ref *surface;
        int ret = drm_vmwgfx_exec_hold_surface(work, file,
            packet.target.sid, packet.target.face, packet.target.mipmap,
            &surface);
        if (ret) return ret;
        uint32_t hint = packet.type == SVGA3D_RT_DEPTH ||
                        packet.type == SVGA3D_RT_STENCIL
            ? SVGA3D_SURFACE_HINT_DEPTHSTENCIL
            : SVGA3D_SURFACE_HINT_RENDERTARGET;
        if (!drm_vmwgfx_surface_has_hint(surface, hint) ||
            surface->metadata.desc.format == SVGA3D_BUFFER)
            return -LINUX_EINVAL;
        return 0;
    }
    case SVGA_3D_CMD_SETTEXTURESTATE: {
        uint32_t count = (body_bytes - sizeof(uint32_t)) /
                         sizeof(SVGA3dTextureState);
        const SVGA3dTextureState *states =
            (const SVGA3dTextureState *)(body + sizeof(uint32_t));
        for (uint32_t i = 0; i < count; ++i) {
            if (states[i].name != SVGA3D_TS_BIND_TEXTURE ||
                states[i].value == SVGA3D_INVALID_ID)
                continue;
            struct drm_vmwgfx_surface_ref *surface;
            int ret = drm_vmwgfx_exec_hold_surface(work, file,
                states[i].value, 0, 0, &surface);
            if (ret) return ret;
            if (!drm_vmwgfx_surface_has_hint(
                    surface, SVGA3D_SURFACE_HINT_TEXTURE) ||
                surface->metadata.desc.format == SVGA3D_BUFFER)
                return -LINUX_EINVAL;
        }
        return 0;
    }
    case SVGA_3D_CMD_DRAW_PRIMITIVES:
        return drm_vmwgfx_exec_validate_draw(work, file, body);
    case SVGA_3D_CMD_SHADER_DEFINE: {
        SVGA3dCmdDefineShader packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.shid < 5000u ? 0 : -LINUX_EINVAL;
    }
    case SVGA_3D_CMD_SHADER_DESTROY: {
        SVGA3dCmdDestroyShader packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.shid < 5000u ? 0 : -LINUX_EINVAL;
    }
    case SVGA_3D_CMD_SET_SHADER: {
        SVGA3dCmdSetShader packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.shid == SVGA3D_INVALID_ID || packet.shid < 5000u
            ? 0 : -LINUX_EINVAL;
    }
    case SVGA_3D_CMD_END_QUERY:
    case SVGA_3D_CMD_WAIT_FOR_QUERY: {
        SVGAGuestPtr result;
        __builtin_memcpy(&result, body + sizeof(uint32_t) * 2u,
                         sizeof(result));
        struct drm_vmwgfx_bo_ref *bo;
        int ret = drm_vmwgfx_exec_hold_bo(work, file, result.gmrId, &bo);
        if (ret) return ret;
        uint64_t end;
        if ((result.offset & 3u) ||
            !drm_vmwgfx_add_u64(result.offset, sizeof(SVGA3dQueryResult),
                                &end) || end > drm_bo_size(bo->object))
            return -LINUX_EINVAL;
        return 0;
    }
    default:
        return 0;
    }
}
