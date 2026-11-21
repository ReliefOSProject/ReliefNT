/* SPDX-License-Identifier: Apache-2.0 */
#include "render.h"
#include "kms.h"
#include "protocol/svga3d_surfacedefs.h"
#include <reliefnt/heap.h>
#include <reliefnt/device_mapping.h>
#include <reliefnt/syscall_internal.h>

#define MAX_LEGACY_IMAGES (DRM_VMW_MAX_SURFACE_FACES * DRM_VMW_MAX_MIP_LEVELS)
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const SVGA3dSurfaceDesc *format_desc(uint32_t format)
{
    if (format >= ARRAY_COUNT(g_SVGA3dSurfaceDescs))
        return NULL;
    const SVGA3dSurfaceDesc *desc = &g_SVGA3dSurfaceDescs[format];
    return desc->format == format && desc->bytesPerBlock ? desc : NULL;
}

static uint64_t blocks(uint32_t n, uint32_t block)
{
    return ((uint64_t)n + block - 1) / block;
}

/* Same packed mip/array layout as Linux vmw_surface_cache.h, with an exact
 * overflow check before a BO is attached. */
static uint64_t image_bytes(const SVGA3dSurfaceDesc *desc, SVGA3dSize image)
{
    bool planar = desc->blockDesc & SVGA3DBLOCKDESC_PLANAR_YUV;
    unsigned stride = planar ? desc->bytesPerBlock : desc->pitchBytesPerBlock;
    return blocks(image.width, desc->blockSize.width) *
        blocks(image.height, desc->blockSize.height) *
        blocks(image.depth, desc->blockSize.depth) * stride;
}

static bool valid_size(uint32_t format, SVGA3dSize size)
{
    if (!size.width || !size.height || !size.depth)
        return false;
    if (format == SVGA3D_BUFFER)
        return size.height == 1 && size.depth == 1;
    static const uint32_t fallback[] = {16384, 16384, 2048};
    uint32_t dimensions[] = {size.width, size.height, size.depth};
    for (unsigned axis = 0; axis < 3; ++axis) {
        uint32_t hw = svga_render_max_texture_dim(axis);
        uint32_t limit = hw && hw < fallback[axis] ? hw : fallback[axis];
        if (dimensions[axis] > limit)
            return false;
    }
    return true;
}

static uint32_t mip_size(uint32_t value, unsigned level)
{
    value >>= level;
    return value ? value : 1;
}

static SVGA3dSize mip_image(const struct drm_vmw_size *base, unsigned level)
{
    return (SVGA3dSize){mip_size(base->width, level), mip_size(base->height, level),
                        mip_size(base->depth, level)};
}

const SVGA3dSize *svga_surface_image(const struct svga_surface *surface, uint32_t face, uint32_t mip,
                                     SVGA3dSize *temporary)
{
    if (!surface->image_count) {
        if (face >= surface->face_count || mip >= surface->info.base.mip_levels)
            return NULL;
        *temporary = mip_image(&surface->info.base.base_size, mip);
        return temporary;
    }
    if (face >= DRM_VMW_MAX_SURFACE_FACES || mip >= surface->levels[face])
        return NULL;
    unsigned image = mip;
    for (unsigned f = 0; f < face; ++f)
        image += surface->levels[f];
    return &surface->images[image];
}

int svga_surface_dma_check(const struct svga_surface *surface, const SVGA3dSize *image,
                           const SVGA3dCopyBox *box, uint32_t pitch, uint32_t maximum_offset)
{
    const SVGA3dSurfaceDesc *desc = format_desc(surface->info.base.format);
    if (!desc || !box->w || !box->h || !box->d ||
        (uint64_t)box->x + box->w > image->width ||
        (uint64_t)box->y + box->h > image->height ||
        (uint64_t)box->z + box->d > image->depth)
        return -EINVAL;
    if (desc->blockDesc & SVGA3DBLOCKDESC_PLANAR_YUV) {
        if (box->srcx || box->srcy || box->srcz || image_bytes(desc, *image) > maximum_offset)
            return -EINVAL;
        return 0;
    }
    SVGA3dSize block = desc->blockSize;
    if (box->srcx % block.width || box->srcy % block.height || box->srcz % block.depth ||
        box->x % block.width || box->y % block.height || box->z % block.depth)
        return -EINVAL;
    uint64_t row = blocks(image->width, block.width) * desc->pitchBytesPerBlock;
    if (pitch)
        row = pitch;
    uint64_t row_count = blocks(image->height, block.height);
    uint64_t x_end = (box->srcx / block.width + blocks(box->w, block.width)) * desc->pitchBytesPerBlock;
    uint64_t y_end = box->srcy / block.height + blocks(box->h, block.height);
    uint64_t z_end = box->srcz / block.depth + blocks(box->d, block.depth);
    if (x_end > row || y_end > row_count || z_end > blocks(image->depth, block.depth))
        return -EINVAL;
    return ((z_end - 1) * row_count + y_end - 1) * row + x_end <= maximum_offset ? 0 : -EINVAL;
}

int svga_surface_transfer_check(const struct svga_surface *surface, const SVGA3dSize *image,
                                const SVGA3dBox *box, uint32_t pitch, uint32_t slice_pitch,
                                uint32_t bytes)
{
    const SVGA3dSurfaceDesc *desc = format_desc(surface->info.base.format);
    if (!desc || !box->w || !box->h || !box->d ||
        (uint64_t)box->x + box->w > image->width ||
        (uint64_t)box->y + box->h > image->height ||
        (uint64_t)box->z + box->d > image->depth)
        return -EINVAL;
    if (desc->blockDesc & SVGA3DBLOCKDESC_PLANAR_YUV)
        return !box->x && !box->y && !box->z && image_bytes(desc, *image) <= bytes ? 0 : -EINVAL;
    SVGA3dSize block = desc->blockSize;
    if (box->x % block.width || box->y % block.height || box->z % block.depth)
        return -EINVAL;
    uint64_t row = blocks(box->w, block.width) * desc->pitchBytesPerBlock;
    uint64_t rows = blocks(box->h, block.height);
    uint64_t slices = blocks(box->d, block.depth);
    uint64_t slice_bytes = (rows - 1) * pitch + row;
    if (row > pitch || (slices > 1 && slice_bytes > slice_pitch))
        return -EINVAL;
    return (slices - 1) * slice_pitch + slice_bytes <= bytes ? 0 : -EINVAL;
}

static int define_host_surface(struct svga_surface *surface)
{
    struct {
        SVGA3dCmdHeader header;
        SVGA3dCmdDefineSurface body;
        SVGA3dSize images[MAX_LEGACY_IMAGES];
    } command = {
        .header = {SVGA_3D_CMD_SURFACE_DEFINE,
            sizeof(SVGA3dCmdDefineSurface) + surface->image_count * sizeof(SVGA3dSize)},
        .body = {.sid = surface->object.id, .surfaceFlags = surface->info.base.svga3d_flags,
                 .format = surface->info.base.format},
    };
    for (unsigned f = 0; f < DRM_VMW_MAX_SURFACE_FACES; ++f)
        command.body.face[f].numMipLevels = surface->levels[f];
    __builtin_memcpy(command.images, surface->images, surface->image_count * sizeof(SVGA3dSize));
    return svga_submit(&command, sizeof(command.header) + command.header.size);
}

static int define_gb_surface(struct svga_surface *surface)
{
    const struct drm_vmw_gb_surface_create_ext_req *info = &surface->info;
    const struct drm_vmw_gb_surface_create_req *base = &info->base;
    uint32_t sid = surface->object.id;
    uint64_t flags = ((uint64_t)info->svga3d_flags_upper_32_bits << 32) | base->svga3d_flags;
    SVGA3dSize size = {base->base_size.width, base->base_size.height, base->base_size.depth};
    union {
        SVGA3dCmdDefineGBSurface v1;
        SVGA3dCmdDefineGBSurface_v2 v2;
        SVGA3dCmdDefineGBSurface_v3 v3;
        SVGA3dCmdDefineGBSurface_v4 v4;
    } body;
    SVGA3dCmdHeader header;
    unsigned model = svga_render_shader_model();
    if (model >= 50 && base->array_size) {
        header = (SVGA3dCmdHeader)SVGA_HEADER(SVGA_3D_CMD_DEFINE_GB_SURFACE_V4, SVGA3dCmdDefineGBSurface_v4);
        body.v4 = (SVGA3dCmdDefineGBSurface_v4){sid, flags, base->format, base->mip_levels,
            base->multisample_count, info->multisample_pattern, info->quality_level,
            base->autogen_filter, size, base->array_size, info->buffer_byte_stride};
    } else if (model >= 41 && base->array_size) {
        header = (SVGA3dCmdHeader)SVGA_HEADER(SVGA_3D_CMD_DEFINE_GB_SURFACE_V3, SVGA3dCmdDefineGBSurface_v3);
        body.v3 = (SVGA3dCmdDefineGBSurface_v3){sid, flags, base->format, base->mip_levels,
            base->multisample_count, info->multisample_pattern, info->quality_level,
            base->autogen_filter, size, base->array_size};
    } else if (base->array_size) {
        header = (SVGA3dCmdHeader)SVGA_HEADER(SVGA_3D_CMD_DEFINE_GB_SURFACE_V2, SVGA3dCmdDefineGBSurface_v2);
        body.v2 = (SVGA3dCmdDefineGBSurface_v2){sid, (uint32_t)flags, base->format, base->mip_levels,
            base->multisample_count, base->autogen_filter, size, base->array_size, 0};
    } else {
        header = (SVGA3dCmdHeader)SVGA_HEADER(SVGA_3D_CMD_DEFINE_GB_SURFACE, SVGA3dCmdDefineGBSurface);
        body.v1 = (SVGA3dCmdDefineGBSurface){sid, (uint32_t)flags, base->format, base->mip_levels,
            base->multisample_count, base->autogen_filter, size};
    }
    uint8_t commands[sizeof(header) + sizeof(body) + sizeof(SVGA3dCmdHeader) + sizeof(SVGA3dCmdBindGBSurface)];
    uint32_t length = sizeof(header) + header.size;
    __builtin_memcpy(commands, &header, sizeof(header));
    __builtin_memcpy(commands + sizeof(header), &body, header.size);
    if (surface->backing) {
        const uint32_t bind[] = {SVGA_3D_CMD_BIND_GB_SURFACE, sizeof(SVGA3dCmdBindGBSurface),
                                 sid, svga_region_id(surface->backing)};
        __builtin_memcpy(commands + length, bind, sizeof(bind));
        length += sizeof(bind);
    }
    return svga_submit(commands, length);
}

static int create_host_surface(struct task *task, struct drm_file *file,
                               union drm_vmw_surface_create_arg *arg)
{
    struct drm_vmw_surface_create_req *req = &arg->req;
    const SVGA3dSurfaceDesc *desc = format_desc(req->format);
    const uint32_t guest_only_flags = SVGA3D_SURFACE_MOB_PITCH | SVGA3D_SURFACE_SCREENTARGET |
        SVGA3D_SURFACE_ALIGN16 | SVGA3D_SURFACE_BIND_CONSTANT_BUFFER |
        SVGA3D_SURFACE_BIND_STREAM_OUTPUT | SVGA3D_SURFACE_STAGING_UPLOAD |
        SVGA3D_SURFACE_STAGING_DOWNLOAD | SVGA3D_SURFACE_HINT_INDIRECT_UPDATE |
        SVGA3D_SURFACE_TRANSFER_FROM_BUFFER | SVGA3D_SURFACE_RESERVED1;
    if (!desc || (req->flags & guest_only_flags))
        return -EINVAL;
    unsigned count = 0;
    for (unsigned f = 0; f < DRM_VMW_MAX_SURFACE_FACES; ++f) {
        if (req->mip_levels[f] > DRM_VMW_MAX_MIP_LEVELS)
            return -EINVAL;
        count += req->mip_levels[f];
    }
    if (!count)
        return -EINVAL;
    struct drm_vmw_size sizes[MAX_LEGACY_IMAGES];
    int ret = svga_import(task, sizes, req->size_addr, count * sizeof(*sizes));
    if (ret)
        return ret;
    const bool gb = svga_render_gb();
    const uint32_t faces = req->flags & SVGA3D_SURFACE_CUBEMAP ? 6 : 1;
    if (gb) {
        /* GB definitions encode just the base size. All supplied legacy
         * images must describe the actual layout of that GB surface. */
        unsigned i = 0;
        for (unsigned f = 0; f < DRM_VMW_MAX_SURFACE_FACES; ++f) {
            unsigned levels = f < faces ? req->mip_levels[0] : 0;
            if (req->mip_levels[f] != levels)
                return -EINVAL;
            for (unsigned m = 0; m < levels; ++m, ++i) {
                SVGA3dSize image = mip_image(&sizes[0], m);
                if (sizes[i].width != image.width || sizes[i].height != image.height ||
                    sizes[i].depth != image.depth)
                    return -EINVAL;
            }
        }
    }
    uint64_t bytes = 0;
    for (unsigned i = 0; i < count; ++i) {
        SVGA3dSize image = {sizes[i].width, sizes[i].height, sizes[i].depth};
        if (!valid_size(req->format, image))
            return -EINVAL;
        bytes += image_bytes(desc, image);
    }
    if (bytes > svga_render_max_surface_size())
        return -E2BIG;
    struct svga_surface *surface = (struct svga_surface *)svga_object_create(file, SVGA_SURFACE,
        sizeof(*surface) + count * sizeof(surface->images[0]));
    if (!surface)
        return -ENOMEM;
    surface->info.base = (struct drm_vmw_gb_surface_create_req){
        .format = req->format, .svga3d_flags = req->flags,
        .base_size = sizes[0], .mip_levels = req->mip_levels[0],
        .autogen_filter = SVGA3D_TEX_FILTER_NONE, .buffer_handle = SVGA3D_INVALID_ID,
        .drm_surface_flags = (req->scanout ? drm_vmw_surface_flag_scanout : 0) |
                             (req->shareable ? drm_vmw_surface_flag_shareable : 0),
    };
    surface->info.version = drm_vmw_gb_surface_v1;
    surface->image_count = count;
    surface->face_count = faces;
    surface->bytes = bytes;
    surface->gb = gb;
    surface->creator_master = file->is_render ? NULL : file->master;
    drm_master_get(surface->creator_master);
    __builtin_memcpy(surface->levels, req->mip_levels, sizeof(surface->levels));
    for (unsigned i = 0; i < count; ++i)
        surface->images[i] = (SVGA3dSize){sizes[i].width, sizes[i].height, sizes[i].depth};
    if (gb) {
        ret = drm_bo_alloc(bytes, &surface->bo);
        if (!ret)
            ret = svga_region_bind(SVGA_REGION_MOB, surface->bo, &surface->backing);
        if (!ret)
            ret = define_gb_surface(surface);
    } else {
        ret = define_host_surface(surface);
    }
    if (ret) {
        (void)svga_object_unref(file, SVGA_SURFACE, surface->object.handle);
        return ret;
    }
    surface->object.published = true;
    svga_surface_set_bo(surface, surface->bo);
    arg->rep = (struct drm_vmw_surface_arg){.sid = surface->object.handle};
    return 0;
}

static struct drm_vmw_gb_surface_create_rep surface_reply(const struct svga_surface *surface,
                                                           uint32_t handle)
{
    return (struct drm_vmw_gb_surface_create_rep){
        .handle = surface->object.handle, .backup_size = surface->bytes,
        .buffer_handle = handle, .buffer_size = drm_bo_size(surface->bo),
        .buffer_map_handle = drm_bo_offset(surface->bo),
    };
}

static int create_gb_surface(struct drm_file *file, const struct drm_vmw_gb_surface_create_ext_req *req,
                             struct drm_vmw_gb_surface_create_rep *reply, uint32_t *new_handle)
{
    if (!svga_render_gb())
        return -EOPNOTSUPP;
    const struct drm_vmw_gb_surface_create_req *base = &req->base;
    const SVGA3dSurfaceDesc *desc = format_desc(base->format);
    if (!desc || req->version != drm_vmw_gb_surface_v1 || req->must_be_zero ||
        !base->mip_levels || base->mip_levels > DRM_VMW_MAX_MIP_LEVELS ||
        !valid_size(base->format, (SVGA3dSize){base->base_size.width, base->base_size.height,
                                             base->base_size.depth}))
        return -EINVAL;
    unsigned model = svga_render_shader_model();
    if ((base->array_size && model < 40) ||
        base->array_size > (model >= 50 ? SVGA3D_SM5_MAX_SURFACE_ARRAYSIZE : SVGA3D_SM4_MAX_SURFACE_ARRAYSIZE) ||
        (req->buffer_byte_stride && (model < 50 || !base->array_size)) ||
        ((req->svga3d_flags_upper_32_bits || base->multisample_count || req->multisample_pattern || req->quality_level) &&
         (model < 41 || !base->array_size)) || base->multisample_count > 32 ||
        ((req->svga3d_flags_upper_32_bits & (SVGA3D_SURFACE_MULTISAMPLE >> 32)) && !base->multisample_count))
        return -EINVAL;
    if (base->drm_surface_flags & ~(drm_vmw_surface_flag_shareable | drm_vmw_surface_flag_scanout |
                                   drm_vmw_surface_flag_create_buffer | drm_vmw_surface_flag_coherent))
        return -EINVAL;
    bool coherent = base->drm_surface_flags & drm_vmw_surface_flag_coherent;
    uint32_t faces = base->array_size;
    if (!faces)
        faces = base->svga3d_flags & SVGA3D_SURFACE_CUBEMAP ? 6 : 1;
    uint64_t bytes = 0;
    for (unsigned i = 0; i < base->mip_levels; ++i)
        bytes += image_bytes(desc, mip_image(&base->base_size, i));
    bytes *= faces * (base->multisample_count ? base->multisample_count : 1);
    if (!bytes || bytes > svga_render_max_surface_size())
        return -E2BIG;
    struct svga_surface *surface = (struct svga_surface *)svga_object_create(file, SVGA_SURFACE, sizeof(*surface));
    if (!surface)
        return -ENOMEM;
    surface->info = *req;
    if ((base->drm_surface_flags & drm_vmw_surface_flag_scanout) &&
        svga_screen_type() == SVGA_DISPLAY_TARGET && svga_mode_fits(base->base_size.width, base->base_size.height))
        surface->info.base.svga3d_flags |= SVGA3D_SURFACE_SCREENTARGET;
    surface->face_count = faces;
    surface->levels[0] = base->mip_levels;
    surface->bytes = bytes;
    surface->gb = true;
    surface->creator_master = file->is_render ? NULL : file->master;
    drm_master_get(surface->creator_master);
    uint32_t handle = base->buffer_handle;
    int ret = 0;
    if (handle == SVGA3D_INVALID_ID &&
        (coherent || (base->drm_surface_flags & drm_vmw_surface_flag_create_buffer))) {
        uint64_t offset;
        ret = drm_bo_create(file, NULL, bytes, new_handle, &offset);
        if (ret)
            goto fail;
        handle = *new_handle;
    }
    if (handle != SVGA3D_INVALID_ID) {
        surface->bo = drm_bo_lookup_handle_get(file, handle);
        if (!surface->bo || drm_bo_is_dumb(surface->bo) || drm_bo_size(surface->bo) < bytes) {
            ret = -EINVAL;
            goto fail;
        }
        if (coherent) {
            ret = device_mapping_revoke_cpu(drm_bo_mapping_access(surface->bo));
            if (ret)
                goto fail;
        }
        ret = svga_region_bind(SVGA_REGION_MOB, surface->bo, &surface->backing);
        if (ret)
            goto fail;
    }
    ret = define_gb_surface(surface);
    if (ret)
        goto fail;
    surface->object.published = true;
    svga_surface_set_bo(surface, surface->bo);
    *reply = surface_reply(surface, handle);
    return 0;

fail:
    (void)svga_object_unref(file, SVGA_SURFACE, surface->object.handle);
    if (*new_handle) {
        (void)drm_bo_handle_delete(file, *new_handle);
        *new_handle = 0;
    }
    return ret;
}

#define BACKING_BUCKETS 256u

/* A BO can back several surfaces. The weak index owns neither endpoint and
 * is updated in the same execution-owned transaction as the MOB binding. */
static struct svga_surface *bo_surfaces[BACKING_BUCKETS];
static struct svga_surface *lru_head, *lru_tail;

static struct svga_surface **surface_bucket(const struct drm_bo *bo)
{
    uintptr_t hash = (uintptr_t)bo >> 4;
    return &bo_surfaces[(hash ^ (hash >> 8)) % BACKING_BUCKETS];
}

bool svga_surface_is_coherent(const struct svga_surface *surface)
{
    return surface->info.base.drm_surface_flags & drm_vmw_surface_flag_coherent;
}

static void surface_lru_remove(struct svga_surface *surface)
{
    if (surface->lru_prev)
        surface->lru_prev->lru_next = surface->lru_next;
    else if (lru_head == surface)
        lru_head = surface->lru_next;
    else
        return;
    if (surface->lru_next)
        surface->lru_next->lru_prev = surface->lru_prev;
    else
        lru_tail = surface->lru_prev;
    surface->lru_prev = surface->lru_next = NULL;
}

static bool surface_reclaimable(const struct svga_surface *surface)
{
    return surface->gb && surface->backing && svga_surface_is_coherent(surface) &&
        !(surface->info.base.svga3d_flags & SVGA3D_SURFACE_SCREENTARGET);
}

static void surface_lru_touch(struct svga_surface *surface)
{
    if (lru_tail == surface || !surface_reclaimable(surface))
        return;
    surface_lru_remove(surface);
    surface->lru_prev = lru_tail;
    if (lru_tail)
        lru_tail->lru_next = surface;
    else
        lru_head = surface;
    lru_tail = surface;
}

bool svga_bo_is_coherent(const struct drm_bo *bo)
{
    for (const struct svga_surface *surface = *surface_bucket(bo); surface;
         surface = surface->bo_next) {
        if (surface->bo == bo && svga_surface_is_coherent(surface))
            return true;
    }
    return false;
}

/* Linux vmwgfx cleans GPU-dirty resources before faulting in shared MOB
 * pages. The BO is the ownership unit here. If aliasing resources overlap,
 * preserve command order and let the most recent GPU writer update last. */
int svga_surface_cpu_access(struct drm_bo *bo)
{
    if (!svga_bo_is_coherent(bo))
        return 0;
    if (svga.fifo.fault)
        return -EIO;
    uint64_t previous = 0;
    uint32_t sequence = 0;
    for (;;) {
        struct svga_surface *dirty = NULL;
        for (struct svga_surface *surface = *surface_bucket(bo); surface;
             surface = surface->bo_next) {
            if (surface->bo == bo && surface->backing && surface->dirty_serial > previous &&
                (!dirty || surface->dirty_serial < dirty->dirty_serial))
                dirty = surface;
        }
        if (!dirty)
            break;
        previous = dirty->dirty_serial;
        /* Several surfaces can be written by one batch. */
        for (struct svga_surface *surface = dirty; surface; surface = surface->bo_next) {
            if (surface->bo != bo || !surface->backing || surface->dirty_serial != previous)
                continue;
            const uint32_t command[] = {SVGA_3D_CMD_READBACK_GB_SURFACE, sizeof(uint32_t),
                                        surface->object.id};
            int ret = svga_submit_fenced(command, sizeof(command), &sequence);
            if (ret)
                return ret;
        }
    }
    if (previous || drm_bo_gpu_busy(bo)) {
        int ret = previous ? svga_submit_wait(sequence) : svga_buffer_wait(bo);
        if (ret)
            return ret;
    }
    for (struct svga_surface *surface = *surface_bucket(bo); surface; surface = surface->bo_next) {
        if (surface->bo == bo)
            surface->dirty_serial = 0;
    }
    return 0;
}

void svga_surface_set_bo(struct svga_surface *surface, struct drm_bo *bo)
{
    surface_lru_remove(surface);
    if (surface->bo_link) {
        *surface->bo_link = surface->bo_next;
        if (surface->bo_next)
            surface->bo_next->bo_link = surface->bo_link;
        surface->bo_link = NULL;
        surface->bo_next = NULL;
    }
    surface->bo = bo;
    if (bo && surface->object.published) {
        struct svga_surface **head = surface_bucket(bo);
        surface->bo_next = *head;
        if (*head)
            (*head)->bo_link = &surface->bo_next;
        surface->bo_link = head;
        *head = surface;
        surface_lru_touch(surface);
    }
}

/* vmwgfx_gb_surface_unbind preserves dirty data before invalidating the MOB
 * binding. Keep the definition and ID live so existing context bindings still
 * resolve after the next validation. The LRU owns no references. */
static int surface_evict(struct svga_surface *surface)
{
    bool dirty = surface->dirty_serial != 0;
    uint32_t sid = surface->object.id;
    const uint32_t packets[] = {
        dirty ? SVGA_3D_CMD_READBACK_GB_SURFACE : SVGA_3D_CMD_INVALIDATE_GB_SURFACE,
        sizeof(uint32_t), sid,
        SVGA_3D_CMD_BIND_GB_SURFACE, sizeof(SVGA3dCmdBindGBSurface), sid, SVGA3D_INVALID_ID,
    };
    uint32_t fence;
    int ret = svga_submit_commands(packets, sizeof(packets), SVGA3D_INVALID_ID,
                                   dirty ? &fence : NULL);
    if (ret)
        return ret;
    if (dirty) {
        /* A later CPU fault must see the result of this readback. */
        ret = svga_submit_wait(fence);
        if (ret) {
            svga.fifo.fault = true;
            return ret;
        }
    }
    struct svga_region *region = surface->backing;
    surface->backing = NULL;
    surface->needs_rebind = true;
    surface->dirty_serial = 0;
    surface_lru_remove(surface);
    svga_region_release(region);
    return svga.fifo.fault ? -EIO : 0;
}

static uint32_t surface_mob_owners(struct drm_bo *bo, const struct svga_region *region)
{
    uint32_t count = 0;
    for (const struct svga_surface *surface = *surface_bucket(bo); surface;
         surface = surface->bo_next) {
        if (surface->bo != bo || !surface->backing)
            continue;
        if (surface->backing != region || !surface_reclaimable(surface))
            return 0;
        ++count;
    }
    return count;
}

int svga_surface_reclaim_mobs(uint32_t *pages)
{
    for (struct svga_surface *surface = lru_head, *next; surface; surface = next) {
        next = surface->lru_next;
        struct drm_bo *bo = surface->bo;
        if (drm_bo_gpu_busy(bo))
            continue;
        struct device_mapping_access *access = drm_bo_mapping_access(bo);
        if (access->exposed || access->aliases)
            continue;

        /* Linux unbinds the resources of a BO as a group. Leave the MOB
         * alone if a query, scanout or another consumer still holds it. */
        uint32_t owners = surface_mob_owners(bo, surface->backing);
        if (!owners || owners != svga_region_users(surface->backing))
            continue;
        if (owners > 1) {
            /* Overlapping coherent views write back in GPU-writer order. */
            int ret = svga_surface_cpu_access(bo);
            if (ret)
                return ret;
        }
        /* Keep the iteration cursor alive when another alias is removed. */
        while (next && next->bo == bo)
            next = next->lru_next;
        for (struct svga_surface *alias = *surface_bucket(bo); alias; alias = alias->bo_next) {
            if (alias->bo != bo || !alias->backing)
                continue;
            int ret = surface_evict(alias);
            if (ret)
                return ret;
        }
        uint32_t freed = drm_bo_size(bo) >> SVGA_PAGE_SHIFT;
        if (freed >= *pages) {
            *pages = 0;
            return 0;
        }
        *pages -= freed;
    }
    return 0;
}

int svga_surface_validate_residency(struct svga_surface *surface)
{
    if (!surface->needs_rebind) {
        surface_lru_touch(surface);
        return 0;
    }
    struct device_mapping_access *access = drm_bo_mapping_access(surface->bo);
    int ret = device_mapping_revoke_cpu(access);
    if (ret)
        return ret;
    struct svga_region *region;
    ret = svga_region_bind(SVGA_REGION_MOB, surface->bo, &region);
    if (ret)
        return ret;

    uint32_t sid = surface->object.id;
    const uint32_t packets[] = {
        SVGA_3D_CMD_BIND_GB_SURFACE, sizeof(SVGA3dCmdBindGBSurface), sid, svga_region_id(region),
        SVGA_3D_CMD_UPDATE_GB_SURFACE, sizeof(uint32_t), sid,
    };
    uint32_t fence;
    ret = svga_submit_fenced(packets, sizeof(packets), &fence);
    if (ret) {
        svga_region_release(region);
        return ret;
    }
    surface->backing = region;
    surface->needs_rebind = false;
    /* The batch may still fail validation. Wait here so the CPU cannot regain
     * access to the BO while this independent upload is outstanding. */
    ret = svga_submit_wait(fence);
    if (ret) {
        svga.fifo.fault = true;
        return ret;
    }
    surface->uploaded_epoch = access->write_epoch;
    surface_lru_touch(surface);
    return 0;
}

/* For BO handles, exporting an associated surface also requires a surface
 * lease. For a raw GEM DMA-BUF import the descriptor supplies the BO lease. */
static struct svga_surface *surface_for_bo(struct drm_bo *bo, const struct drm_file *file)
{
    if (!bo)
        return NULL;
    for (struct svga_surface *surface = *surface_bucket(bo); surface; surface = surface->bo_next) {
        if (surface->bo == bo && (!file || svga_object_lookup(file, SVGA_SURFACE, surface->object.handle)))
            return surface;
    }
    return NULL;
}

static void put_prime_surface(void *object)
{
    svga_object_put(&((struct svga_surface *)object)->object);
}

int svga_prime_export(struct task *task, struct drm_file *file, struct drm_prime_handle *arg)
{
    struct svga_surface *surface = (struct svga_surface *)svga_object_lookup(file, SVGA_SURFACE, arg->handle);
    if (!surface && arg->handle < SVGA_RENDER_HANDLE_BASE) {
        struct drm_bo *bo = drm_bo_lookup_handle_get(file, arg->handle);
        surface = surface_for_bo(bo, file);
        bool associated = surface || surface_for_bo(bo, NULL);
        drm_bo_put(bo);
        if (!surface && associated)
            return -EACCES;
    }
    if (!surface)
        return -ENOENT;
    if (!svga.ready || svga.fifo.fault)
        return -EIO;
    svga_object_get(&surface->object);
    uint64_t size = ((uint64_t)surface->bytes + 4095) & ~4095ull;
    int fd = drm_prime_export_object(task, file->device, arg->flags, size,
                                     surface, put_prime_surface, &surface->prime_description);
    if (fd < 0)
        return fd;
    arg->fd = fd;
    return 0;
}

int svga_prime_import(struct drm_file *file, void *object, uint32_t *handle)
{
    struct svga_surface *surface = object;
    if (!svga.ready || svga.fifo.fault)
        return -EIO;
    int ret = svga_surface_ref(file, surface);
    if (!ret)
        *handle = surface->object.handle;
    return ret;
}

static int refer_surface(struct task *task, struct drm_file *file, uint32_t command, void *data,
                         uint32_t *new_handle, uint32_t *surface_handle)
{
    struct drm_vmw_surface_arg req = *(const struct drm_vmw_surface_arg *)data;
    struct svga_surface *surface;
    switch (req.handle_type) {
    case DRM_VMW_HANDLE_LEGACY:
        if (!file->is_render && !file->authenticated)
            return -EACCES;
        surface = svga_surface_lookup_reference(file, req.sid);
        break;
    case DRM_VMW_HANDLE_PRIME: {
        struct task_file *source = task_file_for_fd(task, req.sid);
        if (!source)
            return -EBADF;
        surface = drm_prime_lookup_object(source, file->device);
        if (!surface)
            surface = surface_for_bo(drm_prime_lookup_bo(source, file->device), NULL);
        break;
    }
    default:
        return -EINVAL;
    }
    if (!surface)
        return -EINVAL;
    const struct drm_vmw_gb_surface_create_ext_req *info = &surface->info;
    uint64_t address = 0;
    if (command == VMW_REF_SURFACE) {
        address = ((const union drm_vmw_surface_reference_arg *)data)->rep.size_addr;
        if (address) {
            int ret = svga_prepare_output(task, address, sizeof(info->base.base_size));
            if (ret)
                return ret;
            if (user_copy_to_task(task, address, &info->base.base_size, sizeof(info->base.base_size)) < 0)
                return -EFAULT;
        }
    } else if (!surface->gb || !surface->bo) {
        return -EINVAL;
    }
    int ret = svga_surface_ref(file, surface);
    if (ret)
        return ret;
    if (command == VMW_REF_SURFACE) {
        union drm_vmw_surface_reference_arg *arg = data;
        arg->rep = (struct drm_vmw_surface_create_req){
            .flags = info->base.svga3d_flags, .format = info->base.format, .size_addr = address,
            .shareable = !!(info->base.drm_surface_flags & drm_vmw_surface_flag_shareable),
            .scanout = !!(info->base.drm_surface_flags & drm_vmw_surface_flag_scanout),
        };
        __builtin_memcpy(arg->rep.mip_levels, surface->levels, sizeof(surface->levels));
    } else {
        uint32_t handle;
        ret = drm_bo_handle_create(file, surface->bo, &handle);
        if (ret) {
            (void)svga_object_unref(file, SVGA_SURFACE, surface->object.handle);
            return ret;
        }
        *new_handle = handle;
        struct drm_vmw_gb_surface_create_rep rep = surface_reply(surface, handle);
        if (command == VMW_GB_SURFACE_REF) {
            union drm_vmw_gb_surface_reference_arg *arg = data;
            arg->rep = (struct drm_vmw_gb_surface_ref_rep){.creq = info->base, .crep = rep};
            arg->rep.creq.buffer_handle = handle;
            arg->rep.creq.drm_surface_flags = 0;
        } else {
            union drm_vmw_gb_surface_reference_ext_arg *arg = data;
            arg->rep = (struct drm_vmw_gb_surface_ref_ext_rep){.creq = *info, .crep = rep};
            arg->rep.creq.base.buffer_handle = handle;
            arg->rep.creq.base.drm_surface_flags = 0;
        }
    }
    *surface_handle = surface->object.handle;
    return 0;
}

int svga_surface_ioctl(struct task *task, struct drm_file *file, uint32_t command,
                       void *data, uint32_t *bo_handle, uint32_t *surface_handle)
{
    switch (command) {
    case VMW_CREATE_SURFACE:
        return create_host_surface(task, file, data);
    case VMW_UNREF_SURFACE:
        return svga_object_unref(file, SVGA_SURFACE, ((const struct drm_vmw_surface_arg *)data)->sid);
    case VMW_GB_SURFACE_CREATE: {
        union drm_vmw_gb_surface_create_arg *arg = data;
        const struct drm_vmw_gb_surface_create_ext_req req = {.base = arg->req};
        return create_gb_surface(file, &req, &arg->rep, bo_handle);
    }
    case VMW_GB_SURFACE_CREATE_EXT: {
        union drm_vmw_gb_surface_create_ext_arg *arg = data;
        const struct drm_vmw_gb_surface_create_ext_req req = arg->req;
        return create_gb_surface(file, &req, &arg->rep, bo_handle);
    }
    case VMW_REF_SURFACE:
    case VMW_GB_SURFACE_REF:
    case VMW_GB_SURFACE_REF_EXT:
        return refer_surface(task, file, command, data, bo_handle, surface_handle);
    default:
        return -EINVAL;
    }
}
