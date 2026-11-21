/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include "dma.h"
#include "render.h"
#include "submit.h"
#include "protocol/svga_escape.h"
#include "protocol/svga_overlay.h"
#include <reliefnt/mm.h>

static struct {
    struct drm_file *owner;
    struct svga_region *region;
    uint32_t handle;
    bool enabled;
} overlay;
static uint32_t next_handle = 1;

bool svga_overlay_supported(void)
{
    const uint32_t caps = SVGA_FIFO_CAP_VIDEO | SVGA_FIFO_CAP_ESCAPE;
    return (svga.fifo.caps & caps) == caps &&
        (svga_screen_type() == SVGA_DISPLAY_LEGACY || svga_dma_supported(SVGA_REGION_GMR));
}

unsigned svga_overlay_free(void)
{
    return svga_overlay_supported() && !overlay.owner;
}

int svga_overlay_claim(struct drm_file *file, struct drm_vmw_stream_arg *arg)
{
    if (!svga_overlay_supported())
        return -ENOSYS;
    if (overlay.owner || !next_handle)
        return -ENOSPC;
    overlay.owner = file;
    overlay.handle = next_handle++;
    *arg = (struct drm_vmw_stream_arg){.stream_id = overlay.handle};
    return 0;
}

struct video_packet {
    uint32_t escape;
    SVGAFifoCmdEscape header;
    uint32_t command, stream;
};

struct video_register {
    uint32_t index, value;
};

static struct video_packet video_packet(uint32_t command, unsigned regs)
{
    return (struct video_packet){
        .escape = SVGA_CMD_ESCAPE,
        .header = {.nsid = SVGA_ESCAPE_NSID_VMWARE, .size = (2 + 2 * regs) * sizeof(uint32_t)},
        .command = command, .stream = 0,
    };
}

int svga_overlay_stop(void)
{
    if (!overlay.enabled)
        return 0;
    const struct {
        struct video_packet set;
        struct video_register disable;
        struct video_packet flush;
    } packet = {
        .set = video_packet(SVGA_ESCAPE_VMWARE_VIDEO_SET_REGS, 1),
        .disable = {SVGA_VIDEO_ENABLED, 0},
        .flush = video_packet(SVGA_ESCAPE_VMWARE_VIDEO_FLUSH, 0),
    };
    int ret = svga_submit(&packet, sizeof(packet));
    if (ret)
        return ret;
    /* The region remains retained until the disable has been queued. */
    svga_region_release(overlay.region);
    overlay.region = NULL;
    overlay.enabled = false;
    return 0;
}

void svga_overlay_release(struct drm_file *file)
{
    if (!overlay.owner || (file && file != overlay.owner))
        return;
    if (svga_overlay_stop()) {
        /* If the host could still scan out of this region, quarantine it. */
        svga.fifo.fault = true;
        svga_region_release(overlay.region);
    }
    __builtin_memset(&overlay, 0, sizeof(overlay));
}

int svga_overlay_unref(struct drm_file *file, const struct drm_vmw_stream_arg *arg)
{
    if (overlay.owner != file || overlay.handle != arg->stream_id)
        return -ENOENT;
    svga_overlay_release(file);
    return 0;
}

static bool video_layout_valid(const struct drm_vmw_control_stream_arg *arg, uint64_t bo_bytes)
{
    if (!arg->size || arg->offset > bo_bytes || arg->size > bo_bytes - arg->offset ||
        !arg->width || !arg->height || arg->width > svga.max_width || arg->height > svga.max_height ||
        (arg->flags & ~SVGA_VIDEO_FLAG_COLORKEY) || !arg->src.w || !arg->src.h ||
        arg->src.x < 0 || arg->src.y < 0 ||
        (uint64_t)arg->src.x + arg->src.w > arg->width ||
        (uint64_t)arg->src.y + arg->src.h > arg->height ||
        !arg->dst.w || !arg->dst.h || arg->dst.w > svga.max_width || arg->dst.h > svga.max_height ||
        (int64_t)arg->dst.x + arg->dst.w > INT32_MAX ||
        (int64_t)arg->dst.y + arg->dst.h > INT32_MAX)
        return false;
    uint64_t required;
    switch (arg->format) {
    case VMWARE_FOURCC_YV12:
        if ((arg->width | arg->height) & 1 || arg->pitch[0] < arg->width ||
            arg->pitch[1] < arg->width / 2 || arg->pitch[2] < arg->width / 2)
            return false;
        required = (uint64_t)arg->pitch[0] * arg->height +
            ((uint64_t)arg->pitch[1] + arg->pitch[2]) * (arg->height / 2);
        break;
    case VMWARE_FOURCC_YUY2:
    case VMWARE_FOURCC_UYVY:
        if ((arg->width & 1) || arg->pitch[0] < arg->width * 2)
            return false;
        required = (uint64_t)arg->pitch[0] * arg->height;
        break;
    default:
        return false;
    }
    return required <= arg->size;
}

static int upload_vram(const struct drm_vmw_control_stream_arg *arg, struct drm_bo *bo,
                       uint32_t *offset)
{
    /* Legacy hosts require VRAM backing. Allocate one slice at the top of
     * BAR1, beyond the current firmware or KMS scanout. Modeset stops it
     * before changing scanout placement. */
    if (arg->size > svga.vram_bytes)
        return -ENOSPC;
    *offset = (svga.vram_bytes - arg->size) & ~3u;
    uint64_t end = svga_read(SVGA_REG_FB_OFFSET) +
        (uint64_t)svga_read(SVGA_REG_BYTES_PER_LINE) * svga_read(SVGA_REG_HEIGHT);
    if (*offset < end)
        return -ENOSPC;
    int ret = svga_buffer_wait(bo);
    if (!ret)
        ret = svga_surface_cpu_access(bo);
    if (!ret && overlay.enabled) {
        ret = svga_overlay_stop();
        if (!ret)
            ret = svga_submit_idle();
    }
    if (ret)
        return ret;
    const uint8_t *pixels = mm_phys_to_virt(drm_bo_page_physical(bo, 0));
    if (!pixels)
        return -EINVAL;
    svga_copy_to_vram(*offset, pixels + arg->offset, arg->size);
    return 0;
}

int svga_overlay_control(struct drm_file *file, const struct drm_vmw_control_stream_arg *arg)
{
    if (!svga_overlay_supported())
        return -ENOSYS;
    if (overlay.owner != file || overlay.handle != arg->stream_id)
        return -ENOENT;
    if (!arg->enabled)
        return svga_overlay_stop();
    struct drm_bo *bo = drm_bo_lookup_handle_get(file, arg->handle);
    if (!bo)
        return -ENOENT;
    int ret = -EINVAL;
    if (!video_layout_valid(arg, drm_bo_size(bo)))
        goto put_bo;

    bool gmr = svga_screen_type() != SVGA_DISPLAY_LEGACY;
    uint32_t offset = arg->offset, id = SVGA_GMR_FRAMEBUFFER;
    struct svga_region *region = NULL;
    if (gmr) {
        if (overlay.region && svga_region_bo(overlay.region) == bo)
            region = overlay.region;
        else {
            ret = svga_region_bind(SVGA_REGION_GMR, bo, &region);
            if (ret)
                goto put_bo;
        }
        id = svga_region_id(region);
    } else {
        ret = upload_vram(arg, bo, &offset);
        if (ret)
            goto put_bo;
    }
    const uint32_t values[SVGA_VIDEO_NUM_REGS] = {
        [SVGA_VIDEO_ENABLED] = 1, [SVGA_VIDEO_FLAGS] = arg->flags,
        [SVGA_VIDEO_DATA_OFFSET] = offset, [SVGA_VIDEO_FORMAT] = arg->format,
        [SVGA_VIDEO_COLORKEY] = arg->color_key & SVGA_VIDEO_COLORKEY_MASK,
        [SVGA_VIDEO_SIZE] = arg->size, [SVGA_VIDEO_WIDTH] = arg->width, [SVGA_VIDEO_HEIGHT] = arg->height,
        [SVGA_VIDEO_SRC_X] = arg->src.x, [SVGA_VIDEO_SRC_Y] = arg->src.y,
        [SVGA_VIDEO_SRC_WIDTH] = arg->src.w, [SVGA_VIDEO_SRC_HEIGHT] = arg->src.h,
        [SVGA_VIDEO_DST_X] = arg->dst.x, [SVGA_VIDEO_DST_Y] = arg->dst.y,
        [SVGA_VIDEO_DST_WIDTH] = arg->dst.w, [SVGA_VIDEO_DST_HEIGHT] = arg->dst.h,
        [SVGA_VIDEO_PITCH_1] = arg->pitch[0], [SVGA_VIDEO_PITCH_2] = arg->pitch[1],
        [SVGA_VIDEO_PITCH_3] = arg->pitch[2], [SVGA_VIDEO_DATA_GMRID] = id,
        [SVGA_VIDEO_DST_SCREEN_ID] = SVGA3D_INVALID_ID,
    };
    unsigned count = gmr ? SVGA_VIDEO_NUM_REGS : SVGA_VIDEO_PITCH_3 + 1;
    struct {
        struct video_packet set;
        struct video_register regs[SVGA_VIDEO_NUM_REGS];
        struct video_packet flush;
    } batch = {.set = video_packet(SVGA_ESCAPE_VMWARE_VIDEO_SET_REGS, count)};
    for (unsigned i = 0; i < count; ++i)
        batch.regs[i] = (struct video_register){i, values[i]};
    struct video_packet flush = video_packet(SVGA_ESCAPE_VMWARE_VIDEO_FLUSH, 0);
    uint32_t bytes = sizeof(batch.set) + count * sizeof(batch.regs[0]);
    __builtin_memcpy((uint8_t *)&batch + bytes, &flush, sizeof(flush));
    ret = svga_display_submit(&batch, bytes + sizeof(flush), NULL, NULL, gmr ? bo : NULL, NULL);
    if (ret) {
        if (region != overlay.region)
            svga_region_release(region);
        goto put_bo;
    }
    if (overlay.region != region)
        svga_region_release(overlay.region);
    overlay.region = region;
    overlay.enabled = true;
put_bo:
    drm_bo_put(bo);
    return ret;
}
