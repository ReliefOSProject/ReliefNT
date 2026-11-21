/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include "dma.h"
#include "submit.h"
#include <reliefnt/framebuffer.h>
#include <reliefnt/mm.h>

/* Screen zero is shared in time with KMS. The private BO has no userspace
 * handle. Its GMR/MOB lease survives VT switches and is released at stop. */
static struct {
    struct svga_fb fb;
    struct framebuffer_damage pending;
    uint64_t generation;
    uint32_t fence;
    bool shown;
} console_scanout;

static struct svga_scanout console_mode(void)
{
    return (struct svga_scanout){
        .mode = {
            .hdisplay = console_scanout.fb.width,
            .vdisplay = console_scanout.fb.height,
        },
        .fb = &console_scanout.fb,
    };
}

static void copy_console_pixels(const struct framebuffer *source,
                                struct framebuffer_damage rect)
{
    bool native = source->bytes_per_pixel == 4 &&
        source->red_mask_size == 8 && source->red_field_position == 16 &&
        source->green_mask_size == 8 && source->green_field_position == 8 &&
        source->blue_mask_size == 8 && source->blue_field_position == 0;
    struct svga_fb *fb = &console_scanout.fb;
    for (uint32_t y = rect.y1; y < rect.y2; ++y) {
        uint32_t *dst = (uint32_t *)(fb->pixels + (size_t)y * fb->pitch) + rect.x1;
        if (native) {
            const uint8_t *src = (const uint8_t *)source->pixels +
                (size_t)y * source->pitch + rect.x1 * 4;
            size_t words = rect.x2 - rect.x1;
            /* Boot pixels may live in uncached MMIO. Read native words. */
            __asm__ volatile("rep movsl" : "+D"(dst), "+S"(src), "+c"(words) :: "memory");
        } else {
            for (uint32_t x = rect.x1; x < rect.x2; ++x)
                *dst++ = framebuffer_get_pixel_public(x, y);
        }
    }
}

void svga_console_update(void)
{
    if (!console_scanout.shown || svga.fifo.fault)
        return;
    const struct framebuffer *source = framebuffer_get();
    struct svga_fb *fb = &console_scanout.fb;
    if (!source->available || !source->pixels ||
        source->width != fb->width || source->height != fb->height)
        return;

    struct framebuffer_damage damage;
    if (framebuffer_take_damage(&console_scanout.generation, &damage)) {
        if (damage.x2 > fb->width)
            damage.x2 = fb->width;
        if (damage.y2 > fb->height)
            damage.y2 = fb->height;
        if (damage.x1 < damage.x2 && damage.y1 < damage.y2)
            framebuffer_damage_union(&console_scanout.pending, damage);
    }
    damage = console_scanout.pending;
    if (damage.x1 == damage.x2)
        return;
    /* Skip a tick if the host is still reading the BO. An unfenced Screen
     * Object transport uses the staging path, which already waits for idle. */
    if (console_scanout.fence && svga_submit_poll(console_scanout.fence) != 1)
        return;
    copy_console_pixels(source, damage);
    struct svga_scanout mode = console_mode();
    if (!svga_screen_present(0, fb, &mode, damage, &console_scanout.fence))
        console_scanout.pending = (struct framebuffer_damage){0};
}

int svga_console_show(void)
{
    const struct framebuffer *source = framebuffer_get();
    struct svga_fb *fb = &console_scanout.fb;
    if (!source->available || !source->pixels ||
        !svga_mode_fits(source->width, source->height))
        return -ENODEV;
    if (fb->bo && (source->width != fb->width || source->height != fb->height))
        svga_console_fini();
    if (!fb->bo) {
        struct drm_bo *bo;
        uint32_t stride = source->width * 4;
        int ret = drm_bo_alloc((uint64_t)stride * source->height, &bo);
        if (ret)
            return ret;
        *fb = (struct svga_fb){
            .bo = bo, .pixels = mm_phys_to_virt(drm_bo_page_physical(bo, 0)),
            .width = source->width, .height = source->height,
            .pitch = stride, .format = SVGA_XRGB8888,
        };
    }
    struct svga_scanout mode = console_mode();
    int ret = svga_screen_configure(0, &mode);
    if (ret)
        return ret;
    console_scanout.shown = true;
    console_scanout.pending = (struct framebuffer_damage){0, 0, fb->width, fb->height};
    svga_console_update();
    return 0;
}

void svga_console_hide(void)
{
    if (!console_scanout.shown)
        return;
    svga_screen_release(0);
    console_scanout.shown = false;
}

void svga_console_fini(void)
{
    svga_console_hide();
    svga_region_release(console_scanout.fb.mob);
    svga_region_release(console_scanout.fb.gmr);
    drm_bo_put(console_scanout.fb.bo);
    console_scanout = (__typeof__(console_scanout)){0};
}
