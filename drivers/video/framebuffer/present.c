#include <reliefnt/export.h>
#include "internal.h"

/** @brief Present a region, optionally dropping timer work under backpressure. */
static void framebuffer_present_region_internal(uint32_t x, uint32_t y,
                                                uint32_t width, uint32_t height,
                                                bool timer_refresh);

static int framebuffer_drm_publish_locked(uint32_t x1, uint32_t y1,
                                          uint32_t x2, uint32_t y2);

static uint32_t framebuffer_drm_pending_x1;
static uint32_t framebuffer_drm_pending_y1;
static uint32_t framebuffer_drm_pending_x2;
static uint32_t framebuffer_drm_pending_y2;
bool framebuffer_drm_owned(void)
{
    return __atomic_load_n(&framebuffer_drm_scanout_owned, __ATOMIC_ACQUIRE);
}

/** @brief Present a region, optionally dropping timer work under backpressure. */
static void framebuffer_present_region_internal(uint32_t x, uint32_t y,
                                                uint32_t width, uint32_t height,
                                                bool timer_refresh)
{
    uint64_t flags;
    if (timer_refresh) {
        if (!svga_try_lock_irqsave(&flags)) return;
    } else {
        flags = svga_lock();
    }
    if (fb.backend != FRAMEBUFFER_BACKEND_VMWARE_SVGA || !fb.available) {
        svga_unlock(flags);
        return;
    }
    if (timer_refresh && svga.traces_enabled) {
        svga_unlock(flags);
        return;
    }
    if (framebuffer_drm_owned()) {
        if (timer_refresh && framebuffer_drm_damage_pending) {
            uint32_t x1 = framebuffer_drm_pending_x1;
            uint32_t y1 = framebuffer_drm_pending_y1;
            uint32_t x2 = framebuffer_drm_pending_x2;
            uint32_t y2 = framebuffer_drm_pending_y2;
            int published;
            if (svga.available) {
                published = svga_update_try_locked(x1, y1, x2 - x1, y2 - y1) == 0;
            } else {
                published = framebuffer_vmware_fifo_update(x1, y1,
                                                            x2 - x1, y2 - y1);
                if (published) framebuffer_vmware_kick();
            }
            if (published) framebuffer_drm_damage_pending = false;
        }
        svga_unlock(flags);
        return;
    }
    if (width == UINT32_MAX && height == UINT32_MAX) {
        width = fb.width;
        height = fb.height;
    }
    if (x >= fb.width || y >= fb.height) {
        svga_unlock(flags);
        return;
    }
    if (width > fb.width - x) width = fb.width - x;
    if (height > fb.height - y) height = fb.height - y;
    if (!width || !height) {
        svga_unlock(flags);
        return;
    }
    if (svga.available) {
        if (timer_refresh) {
            (void)svga_update_try_locked(x, y, width, height);
        } else {
            (void)svga_update_locked(x, y, width, height);
        }
        svga_unlock(flags);
        return;
    }
    if (!framebuffer_vmware_fifo_update(x, y, width, height)) {
        if (timer_refresh) {
            /* One BUSY read lets legacy VMware hosts advance STOP; the next
             * periodic full-frame refresh retries after host progress. */
            (void)vmware_svga_read(VMWARE_SVGA_REG_BUSY);
            svga_unlock(flags);
            return;
        }
        /* A legacy host may only advance STOP while REG_BUSY is read.  A
         * doorbell alone can therefore leave a full ring stuck forever and
         * drop every later UPDATE, which presents as a black Xorg desktop.
         * Drain with the bounded SVGA poll, rebuild an invalid ring, and
         * retry this exact region before returning. */
        framebuffer_vmware_sync(false);
        if (!vmware_svga.fifo_present) {
            framebuffer_vmware_fifo_init(false, true);
        }
        if (framebuffer_vmware_fifo_update(x, y, width, height)) {
            framebuffer_vmware_kick();
        }
        svga_unlock(flags);
        return;
    }
    /* Publish asynchronously. Draining after every update (the previous
     * behaviour) forced a synchronous SVGA_REG_BUSY wait inside the global
     * execution transaction on each frame, which pinned one core spinning and
     * serialised every other core's syscall behind it; that is exactly what a
     * compositor/Doom present loop must not do. We still ring the doorbell so
     * the host processes the queued update; the newer SVGA path already kicks
     * its own doorbell at packet publish, so this only affects the legacy
     * ring. Backpressure is handled on the failure path above: if NEXT_CMD has
     * filled the ring, the next update fails, we drain there, and the region
     * is retried, so the ring cannot silently overrun STOP and freeze the
     * pointer. */
    framebuffer_vmware_kick();
    svga_unlock(flags);
}

void framebuffer_present_region(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    framebuffer_present_region_internal(x, y, width, height, false);
}

void framebuffer_present(void)
{
    framebuffer_present_region(0, 0, UINT32_MAX, UINT32_MAX);
}

void framebuffer_display_tick(void)
{
    framebuffer_present_region_internal(0, 0, UINT32_MAX, UINT32_MAX, true);
}

/**
 * @brief Transfer a validated SVGA scanout from console tracing to DRM presentation.
 * @return Zero, including already-owned scanout, or legacy internal -1 for unsupported framebuffer state.
 * Execution-owned task context; takes the SVGA lock internally and disables traces only on success. No heap allocation or provider self-reference.
 */
int framebuffer_drm_scanout_begin(void)
{
    if (!fb.available || fb.backend != FRAMEBUFFER_BACKEND_VMWARE_SVGA ||
        !vmware_svga.present || fb.bpp != 32u || fb.bytes_per_pixel != 4u ||
        !fb.width || !fb.height || fb.pitch < (uint64_t)fb.width * 4u)
        return -1;
    uint64_t flags = svga_lock();
    if (framebuffer_drm_owned()) {
        svga_unlock(flags);
        return 0;
    }
    __atomic_store_n(&framebuffer_drm_scanout_owned, true, __ATOMIC_RELEASE);
    framebuffer_drm_damage_pending = false;
    if (svga.bound && svga.traces_enabled) {
        svga.ops.write(SVGA_REG_TRACES, 0);
        svga.traces_enabled = false;
    }
    svga_unlock(flags);
    return 0;
}

/**
 * @brief Return the permanent SVGA scanout to ordinary framebuffer tracing.
 * Execution-owned task context after dependent jobs drain. Clears pending DRM damage and restores tracing under the SVGA lock; does not unbind the boot console or free provider-owned backing.
 */
void framebuffer_drm_scanout_release(void)
{
    uint64_t flags = svga_lock();
    __atomic_store_n(&framebuffer_drm_scanout_owned, false, __ATOMIC_RELEASE);
    framebuffer_drm_damage_pending = false;
    framebuffer_drm_pending_x1 = framebuffer_drm_pending_y1 = 0;
    framebuffer_drm_pending_x2 = framebuffer_drm_pending_y2 = 0;
    if (vmware_svga.fifo_present && svga.bound)
        (void)svga_restore_traces_locked();
    svga_unlock(flags);
}

static int framebuffer_drm_publish_locked(uint32_t x1, uint32_t y1,
                                          uint32_t x2, uint32_t y2)
{
    if (x1 >= x2 || y1 >= y2) return 1;
    if (svga.available)
        return svga_update_try_locked(x1, y1, x2 - x1, y2 - y1) == 0;
    if (!framebuffer_vmware_fifo_update(x1, y1, x2 - x1, y2 - y1)) return 0;
    framebuffer_vmware_kick();
    return 1;
}

/**
 * @brief Copy validated XRGB8888 damage into the retained DRM-owned scanout.
 * @param pixels Readable kernel source covering every requested row.
 * @param pitch Source byte stride, at least the display width times four.
 * @param format Supported XRGB8888 format code.
 * @param x1 Inclusive left pixel.
 * @param y1 Inclusive top pixel.
 * @param x2 Exclusive right pixel, within display width.
 * @param y2 Exclusive bottom pixel, within display height.
 * @return Zero after copying/queuing damage, or legacy internal -1 for invalid or unavailable scanout.
 * Execution-owned task context; source remains borrowed only during the copy. Coalesces FIFO pressure into permanent damage state under the SVGA lock; no module/data pointer is queued.
 */
int framebuffer_drm_present_region(const void *pixels, uint32_t pitch,
                                   uint32_t format, uint32_t x1, uint32_t y1,
                                   uint32_t x2, uint32_t y2)
{
    const uint32_t drm_format_xrgb8888 = 0x34325258u;
    if (!pixels || !framebuffer_drm_owned() ||
        fb.backend != FRAMEBUFFER_BACKEND_VMWARE_SVGA || !fb.available ||
        format != drm_format_xrgb8888 || fb.bpp != 32u ||
        fb.bytes_per_pixel != 4u || pitch < (uint64_t)fb.width * 4u ||
        x1 >= x2 || y1 >= y2 || x2 > fb.width || y2 > fb.height)
        return -1;
    uint64_t last_source = (uint64_t)(y2 - 1u) * pitch + (uint64_t)x2 * 4u;
    if (y2 > 1u && (last_source - (uint64_t)x2 * 4u) / (y2 - 1u) != pitch)
        return -1;
    const uint8_t *source = pixels;
    for (uint32_t y = y1; y < y2; ++y) {
        uint8_t *destination = framebuffer_pixel_ptr(x1, y);
        const uint8_t *row = source + (uint64_t)y * pitch + (uint64_t)x1 * 4u;
        __builtin_memcpy(destination, row, (uint64_t)(x2 - x1) * 4u);
    }

    uint64_t flags = svga_lock();
    if (!framebuffer_drm_owned()) {
        svga_unlock(flags);
        return -1;
    }
    uint32_t send_x1 = x1, send_y1 = y1, send_x2 = x2, send_y2 = y2;
    if (framebuffer_drm_damage_pending) {
        if (framebuffer_drm_pending_x1 < send_x1) send_x1 = framebuffer_drm_pending_x1;
        if (framebuffer_drm_pending_y1 < send_y1) send_y1 = framebuffer_drm_pending_y1;
        if (framebuffer_drm_pending_x2 > send_x2) send_x2 = framebuffer_drm_pending_x2;
        if (framebuffer_drm_pending_y2 > send_y2) send_y2 = framebuffer_drm_pending_y2;
    }
    if (framebuffer_drm_publish_locked(send_x1, send_y1, send_x2, send_y2)) {
        framebuffer_drm_damage_pending = false;
    } else {
        framebuffer_drm_damage_pending = true;
        framebuffer_drm_pending_x1 = send_x1;
        framebuffer_drm_pending_y1 = send_y1;
        framebuffer_drm_pending_x2 = send_x2;
        framebuffer_drm_pending_y2 = send_y2;
    }
    svga_unlock(flags);
    /* FIFO-full damage has already been copied into GFB and is kernel-owned
     * until a nonblocking display tick publishes its coalesced rectangle. */
    return 0;
}

EXPORT_SYMBOL(framebuffer_drm_present_region);
EXPORT_SYMBOL(framebuffer_drm_scanout_begin);
EXPORT_SYMBOL(framebuffer_drm_scanout_release);
