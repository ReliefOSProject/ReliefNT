#include <reliefnt/export.h>
/* SPDX-License-Identifier: Apache-2.0 */
#include "device.h"

/**
 * @brief Queue only a PRESENT packet and fence, with no generic batch sidecar.
 * @param surface Caller-pinned, live source surface.
 * @param rects Trusted kernel rectangles, each fitting the source and scanout.
 * @param count Number of rectangles, between one and 64.
 * @param fence Output token, cleared before validation.
 * @return Zero after publication, SVGA_EBUSY for transient contention, or an
 *         unchanged negative SVGA status without publishing any packet.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga3d_present_try(svga_handle surface, const struct svga_present_rect *rects,
                      uint32_t count, svga_handle *fence)
{
    if (!fence) return SVGA_EINVAL;
    *fence = 0;
    if (!rects || !count || count > 64u) return SVGA_EINVAL;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int ret = SVGA_ENODEV;
    if (!svga.available || !svga.fifo_ready) goto done;
    int id = svga_find_surface(surface);
    ret = SVGA_EINVAL;
    if (id < 0 || !svga_surface_usable((uint32_t)id)) goto done;
    const struct svga_surface_desc *desc = &svga.surfaces[id].desc;
    for (uint32_t i = 0; i < count; ++i) {
        const struct svga_present_rect *r = &rects[i];
        if (!r->w || !r->h || r->x >= desc->width || r->y >= desc->height ||
            r->w > desc->width - r->x || r->h > desc->height - r->y ||
            r->srcx >= desc->width || r->srcy >= desc->height ||
            r->w > desc->width - r->srcx || r->h > desc->height - r->srcy)
            goto done;
    }
    uint32_t words[3u + 64u * 6u];
    words[0] = SVGA_3D_CMD_PRESENT;
    words[1] = sizeof(SVGA3dCmdPresent) + count * sizeof(SVGA3dCopyRect);
    words[2] = (uint32_t)id;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = 3u + i * 6u;
        words[at] = rects[i].x;
        words[at + 1u] = rects[i].y;
        words[at + 2u] = rects[i].w;
        words[at + 3u] = rects[i].h;
        words[at + 4u] = rects[i].srcx;
        words[at + 5u] = rects[i].srcy;
    }
    ret = svga_fifo_batch_try_locked(words, (3u + count * 6u) * sizeof(uint32_t),
                                     fence);
    if (!ret) svga.surfaces[id].last_use_fence = *fence;
done:
    svga_unlock(flags);
    return ret;
}

EXPORT_SYMBOL(svga3d_present_try);
