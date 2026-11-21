/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include "submit.h"
#include "dma.h"
#include "render.h"
#include <reliefnt/framebuffer.h>
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>

#define SVGA_3D_PACKET(type) struct { SVGA3dCmdHeader header; type body; }

static enum svga_display_type display_type;
static uint32_t primary_bytes, target_width, target_height, screen_bytes;

/* The console's BAR1 range is outside the allocator. SO2 backing is used
 * exclusively by the host. Scratch, if needed, is separate from that backing. */
static struct {
    uint32_t start, end, staging_bytes;
    bool backed;
} object_vram;

/* Indexed by host screen ID. The internal GB surface uses the same index
 * in the separately reserved prefix of the surface ID table. */
struct svga_screen {
    struct svga_region *backing, *bound_mob;
    struct svga_surface *bound_surface;
    uint32_t width, height, x, y, format;
    uint32_t staging_fence;
    uint32_t vram_offset, vram_bytes;
    bool defined, staging_pending;
};

static struct svga_screen screens[SVGA_MAX_OUTPUTS];

static void retain_display_surface(struct svga_screen *screen, struct svga_surface *surface)
{
    if (surface == screen->bound_surface)
        return;
    if (surface)
        svga_object_get(&surface->object);
    if (screen->bound_surface)
        svga_object_put(&screen->bound_surface->object);
    screen->bound_surface = surface;
}

void svga_screen_init(uint32_t boot_vram_end)
{
    display_type = SVGA_DISPLAY_LEGACY;
    screen_bytes = primary_bytes = svga.vram_bytes;
    target_width = svga.max_width;
    target_height = svga.max_height;
    if (svga_dma_supported(SVGA_REGION_MOB)) {
        uint32_t width = svga_read(SVGA_REG_SCREENTARGET_MAX_WIDTH);
        uint32_t height = svga_read(SVGA_REG_SCREENTARGET_MAX_HEIGHT);
        uint32_t bytes = svga_read(SVGA_REG_MAX_PRIMARY_MEM);
        if (width && height && bytes) {
            display_type = SVGA_DISPLAY_TARGET;
            if (width < target_width)
                target_width = width;
            if (height < target_height)
                target_height = height;
            primary_bytes = bytes;
            screen_bytes = svga_dma_max_mob_size();
            if (screen_bytes > primary_bytes)
                screen_bytes = primary_bytes;
            return;
        }
    }
    bool backed = (svga.caps & SVGA_CAP_SCREEN_OBJECT_2) ||
        (svga.fifo.caps & SVGA_FIFO_CAP_SCREEN_OBJECT_2);
    bool unbacked = svga.fifo.caps & SVGA_FIFO_CAP_SCREEN_OBJECT;
    if (!backed && !unbacked)
        return;

    uint64_t firmware_end = svga_read(SVGA_REG_FB_OFFSET) +
        (uint64_t)svga_read(SVGA_REG_BYTES_PER_LINE) * svga_read(SVGA_REG_HEIGHT);
    if (firmware_end < boot_vram_end)
        firmware_end = boot_vram_end;
    uint32_t end = svga.vram_bytes & ~(SVGA_PAGE_SIZE - 1);
    uint32_t start = firmware_end < end ?
        (firmware_end + SVGA_PAGE_SIZE - 1) & ~(SVGA_PAGE_SIZE - 1) : end;
    uint32_t staging = 0;
    if (!svga_dma_supported(SVGA_REGION_GMR)) {
        if (start == end)
            return;
        staging = ((end - start) / 8) & ~(SVGA_PAGE_SIZE - 1);
        if (staging < SVGA_PAGE_SIZE)
            staging = SVGA_PAGE_SIZE;
        if (staging > 256 * 1024)
            staging = 256 * 1024;
        end -= staging;
    }
    if (backed && start < end) {
        display_type = SVGA_DISPLAY_OBJECT;
        primary_bytes = screen_bytes = end - start;
    } else if (unbacked) {
        display_type = SVGA_DISPLAY_OBJECT;
        backed = false;
    }
    object_vram.start = start;
    object_vram.end = end;
    object_vram.staging_bytes = staging;
    object_vram.backed = backed;
}

uint32_t svga_screen_max_primary(void)
{
    return primary_bytes;
}

bool svga_mode_fits(uint32_t width, uint32_t height)
{
    return width && height && width <= target_width && height <= target_height &&
        (uint64_t)width * height * 4 <= screen_bytes;
}

enum svga_display_type svga_screen_type(void)
{
    return display_type;
}

static int define_screen_object(unsigned unit, uint32_t offset,
                                uint32_t width, uint32_t height, uint32_t x, uint32_t y)
{
    uint32_t size = object_vram.backed ? sizeof(SVGAScreenObject) :
        offsetof(SVGAScreenObject, backingStore);
    const struct {
        uint32_t define;
        SVGAScreenObject body;
    } packet = {
        .define = SVGA_CMD_DEFINE_SCREEN,
        .body = {
            .structSize = size,
            .id = unit,
            .flags = SVGA_SCREEN_HAS_ROOT | (!unit ? SVGA_SCREEN_IS_PRIMARY : 0),
            .size = {width, height}, .root = {x, y},
            .backingStore = {.ptr = {SVGA_GMR_FRAMEBUFFER, offset}, .pitch = width * 4},
        },
    };
    return svga_submit(&packet, sizeof(packet.define) + size);
}

/* Caller may replace the specified screen, after its destroy completes.
 * This scan runs only on modeset and considers at most eight live extents. */
static int screen_vram_find(unsigned unit, uint32_t bytes, uint32_t *out)
{
    uint32_t offset = object_vram.start;
    while (bytes <= object_vram.end - offset) {
        uint32_t next = offset;
        for (unsigned i = 0; i < SVGA_MAX_OUTPUTS; ++i) {
            const struct svga_screen *other = &screens[i];
            if (i == unit || !other->vram_bytes || offset + bytes <= other->vram_offset)
                continue;
            uint32_t limit = other->vram_offset + other->vram_bytes;
            if (next < limit)
                next = limit;
        }
        if (next == offset) {
            *out = offset;
            return 0;
        }
        offset = next;
    }
    return -ENOSPC;
}

static int destroy_screen_object(unsigned unit)
{
    const uint32_t packet[] = {SVGA_CMD_DESTROY_SCREEN, unit};
    if (!object_vram.backed)
        return svga_submit(packet, sizeof(packet));

    uint32_t fence;
    int ret = svga_submit_fenced(packet, sizeof(packet), &fence);
    if (!ret)
        ret = svga_submit_wait(fence);
    return ret;
}

static int configure_screen_object(unsigned unit, uint32_t width, uint32_t height,
                                   uint32_t x, uint32_t y)
{
    struct svga_screen *screen = &screens[unit];
    uint32_t offset = screen->vram_offset, bytes = screen->vram_bytes;
    if (object_vram.backed && (width != screen->width || height != screen->height)) {
        bytes = (width * height * 4 + SVGA_PAGE_SIZE - 1) & ~(SVGA_PAGE_SIZE - 1);
        int ret = screen_vram_find(unit, bytes, &offset);
        if (ret)
            return ret;
        if (screen->defined) {
            ret = destroy_screen_object(unit);
            if (ret) {
                svga.fifo.fault = true;
                return ret;
            }
            *screen = (struct svga_screen){0};
        }
    }

    int ret = define_screen_object(unit, offset, width, height, x, y);
    if (ret)
        return ret;
    *screen = (struct svga_screen){
        .defined = true, .width = width, .height = height, .x = x, .y = y,
        .vram_offset = offset, .vram_bytes = bytes,
    };
    return 0;
}

struct target_destroy {
    SVGA_3D_PACKET(SVGA3dCmdDestroyGBScreenTarget) target;
    SVGA_3D_PACKET(SVGA3dCmdDestroyGBSurface) surface;
};

static struct target_destroy destroy_target(unsigned unit)
{
    return (struct target_destroy){
        .target = {
            SVGA_HEADER(SVGA_3D_CMD_DESTROY_GB_SCREENTARGET, SVGA3dCmdDestroyGBScreenTarget),
            {.stid = unit},
        },
        .surface = {
            SVGA_HEADER(SVGA_3D_CMD_DESTROY_GB_SURFACE, SVGA3dCmdDestroyGBSurface),
            {.sid = unit},
        },
    };
}

static int define_screen_target(unsigned unit, struct svga_region *backing,
                                uint32_t width, uint32_t height, uint32_t x, uint32_t y,
                                uint32_t format)
{
    const struct {
        struct target_destroy destroy;
        SVGA_3D_PACKET(SVGA3dCmdDefineGBSurface) surface;
        SVGA_3D_PACKET(SVGA3dCmdBindGBSurface) mob;
        SVGA_3D_PACKET(SVGA3dCmdDefineGBScreenTarget) target;
        SVGA_3D_PACKET(SVGA3dCmdBindGBScreenTarget) image;
    } packets = {
        .destroy = destroy_target(unit),
        .surface = {
            SVGA_HEADER(SVGA_3D_CMD_DEFINE_GB_SURFACE, SVGA3dCmdDefineGBSurface),
            {.sid = unit, .surfaceFlags = SVGA3D_SURFACE_SCREENTARGET,
             .format = format, .numMipLevels = 1,
             .size = {width, height, 1}},
        },
        .mob = {
            SVGA_HEADER(SVGA_3D_CMD_BIND_GB_SURFACE, SVGA3dCmdBindGBSurface),
            {unit, svga_region_id(backing)},
        },
        .target = {
            SVGA_HEADER(SVGA_3D_CMD_DEFINE_GB_SCREENTARGET, SVGA3dCmdDefineGBScreenTarget),
            {.stid = unit, .width = width, .height = height,
             .flags = !unit ? SVGA_STFLAG_PRIMARY : 0, .xRoot = x, .yRoot = y},
        },
        .image = {
            SVGA_HEADER(SVGA_3D_CMD_BIND_GB_SCREENTARGET, SVGA3dCmdBindGBScreenTarget),
            {.stid = unit, .image = {.sid = unit}},
        },
    };
    unsigned skip = screens[unit].defined ? 0 : sizeof(packets.destroy);
    return svga_submit((const uint8_t *)&packets + skip,
                            sizeof(packets) - skip);
}

int svga_screen_configure(unsigned unit, const struct svga_scanout *scanout)
{
    if (svga.fifo.fault)
        return -EIO;
    struct svga_screen *screen = &screens[unit];
    uint32_t width = scanout->mode.hdisplay, height = scanout->mode.vdisplay;
    uint32_t x = scanout->root_x, y = scanout->root_y;
    if (display_type == SVGA_DISPLAY_LEGACY || !svga_mode_fits(width, height))
        return -EINVAL;
    uint32_t format = scanout->fb->surface && display_type == SVGA_DISPLAY_TARGET ?
        scanout->fb->surface->info.base.format : SVGA3D_X8R8G8B8;
    if (screen->defined && width == screen->width && height == screen->height &&
        x == screen->x && y == screen->y &&
        (display_type == SVGA_DISPLAY_OBJECT || format == screen->format))
        return 0;
    if (display_type == SVGA_DISPLAY_OBJECT)
        return configure_screen_object(unit, width, height, x, y);
    struct svga_region *backing;
    int ret = svga_region_create(SVGA_REGION_MOB, width * height * 4, &backing);
    if (ret)
        return ret;
    ret = define_screen_target(unit, backing, width, height, x, y, format);
    if (ret) {
        svga_region_release(backing);
        return ret;
    }
    /* Destruction and replacement are queued together. The old backing
     * can be detached in queue order without waiting for global idle. */
    svga_region_release(screen->backing);
    retain_display_surface(screen, NULL);
    *screen = (struct svga_screen){
        .defined = true, .width = width, .height = height, .x = x, .y = y,
        .format = format, .backing = backing, .bound_mob = backing,
    };
    return svga.fifo.fault ? -EIO : 0;
}

void svga_screen_release(unsigned unit)
{
    struct svga_screen *screen = &screens[unit];
    if (!screen->defined)
        return;
    if (display_type == SVGA_DISPLAY_OBJECT) {
        if (svga.fifo.fault || destroy_screen_object(unit)) {
            svga.fifo.fault = true;
            return;
        }
        *screen = (struct svga_screen){0};
        return;
    }
    const struct target_destroy target = destroy_target(unit);
    if (svga_submit(&target, sizeof(target)))
        svga.fifo.fault = true;
    svga_region_release(screen->backing);
    retain_display_surface(screen, NULL);
    *screen = (struct svga_screen){0};
}

struct object_gmrfb {
    uint32_t command;
    SVGAFifoCmdDefineGMRFB body;
};

static struct object_gmrfb object_framebuffer(uint32_t id, uint32_t offset, uint32_t pitch)
{
    return (struct object_gmrfb){
        SVGA_CMD_DEFINE_GMRFB,
        {.ptr = {id, offset}, .bytesPerLine = pitch,
         .format = {.bitsPerPixel = 32, .colorDepth = 24}},
    };
}

/* Without GMR, transfer a bounded tile through one BAR1 scratch range.
 * DRM execution ownership serializes users of scratch. Each blit retires
 * before either the CPU or another blit can read or overwrite the tile. */
static int object_staging_blit(unsigned unit, const struct svga_fb *fb,
                               const struct svga_blit *rects, unsigned count,
                               bool readback, uint32_t *sequence)
{
    if (svga.fifo.fault)
        return -EIO;
    int ret = svga_buffer_wait(fb->bo);
    if (!ret)
        ret = svga_surface_cpu_access(fb->bo);
    if (ret)
        return ret;

    for (unsigned i = 0; i < count; ++i) {
        const struct svga_blit *rect = &rects[i];
        uint32_t fb_x = readback ? rect->dx : rect->sx;
        uint32_t fb_y = readback ? rect->dy : rect->sy;
        uint32_t screen_x = readback ? rect->sx : rect->dx;
        uint32_t screen_y = readback ? rect->sy : rect->dy;
        for (uint32_t x = 0; x < rect->width;) {
            uint32_t width = rect->width - x;
            if (width > object_vram.staging_bytes / 4)
                width = object_vram.staging_bytes / 4;
            uint32_t pitch = width * 4;
            for (uint32_t y = 0; y < rect->height;) {
                uint32_t height = rect->height - y;
                if (height > object_vram.staging_bytes / pitch)
                    height = object_vram.staging_bytes / pitch;
                uint8_t *pixels = fb->pixels + fb->offset +
                    (uint64_t)(fb_y + y) * fb->pitch + (fb_x + x) * 4;
                SVGASignedRect area = {
                    screen_x + x, screen_y + y, screen_x + x + width, screen_y + y + height,
                };
                struct {
                    struct object_gmrfb fb;
                    uint32_t blit;
                    union {
                        SVGAFifoCmdBlitScreenToGMRFB download;
                        SVGAFifoCmdBlitGMRFBToScreen upload;
                    } tile;
                } packet = {
                    .fb = object_framebuffer(SVGA_GMR_FRAMEBUFFER, object_vram.end, pitch),
                };
                if (readback) {
                    packet.blit = SVGA_CMD_BLIT_SCREEN_TO_GMRFB;
                    packet.tile.download = (SVGAFifoCmdBlitScreenToGMRFB){
                        .srcRect = area, .srcScreenId = unit,
                    };
                } else {
                    for (uint32_t line = 0; line < height; ++line)
                        svga_copy_to_vram(object_vram.end + line * pitch,
                                          pixels + (uint64_t)line * fb->pitch, pitch);
                    /* BAR1 may be write-combined. Publish after its stores. */
                    __asm__ volatile("sfence" ::: "memory");
                    packet.blit = SVGA_CMD_BLIT_GMRFB_TO_SCREEN;
                    packet.tile.upload = (SVGAFifoCmdBlitGMRFBToScreen){
                        .destRect = area, .destScreenId = unit,
                    };
                }
                uint32_t fence;
                ret = svga_submit_fenced(&packet, sizeof(packet), &fence);
                if (ret)
                    return ret;
                ret = svga_submit_wait(fence);
                if (ret) {
                    svga.fifo.fault = true;
                    return ret;
                }
                if (readback)
                    for (uint32_t line = 0; line < height; ++line)
                        svga_copy_from_vram(pixels + (uint64_t)line * fb->pitch,
                                            object_vram.end + line * pitch, pitch);
                if (sequence)
                    *sequence = fence;
                y += height;
            }
            x += width;
        }
    }
    return 0;
}

static int present_object(unsigned unit, struct svga_fb *fb, const struct svga_scanout *scanout,
                          struct framebuffer_damage rect, uint32_t *fence)
{
    if (object_vram.staging_bytes) {
        const struct svga_blit blit = {
            .sx = scanout->x + rect.x1, .sy = scanout->y + rect.y1,
            .dx = rect.x1, .dy = rect.y1, .width = rect.x2 - rect.x1, .height = rect.y2 - rect.y1,
        };
        return object_staging_blit(unit, fb, &blit, 1, false, fence);
    }
    if (!fb->gmr) {
        int ret = svga_region_bind(SVGA_REGION_GMR, fb->bo, &fb->gmr);
        if (ret)
            return ret;
    }
    const struct {
        struct object_gmrfb gmr;
        uint32_t blit;
        SVGAFifoCmdBlitGMRFBToScreen clip;
    } cmds = {
        .gmr = object_framebuffer(svga_region_id(fb->gmr), fb->offset, fb->pitch),
        .blit = SVGA_CMD_BLIT_GMRFB_TO_SCREEN,
        .clip = {.srcOrigin = {scanout->x + rect.x1, scanout->y + rect.y1},
                 .destRect = {rect.x1, rect.y1, rect.x2, rect.y2},
                 .destScreenId = unit},
    };
    return svga_display_submit(&cmds, sizeof(cmds), NULL, NULL, fb->bo, fence);
}

static int present_target(unsigned unit, struct svga_fb *fb, const struct svga_scanout *scanout,
                          struct framebuffer_damage rect, uint32_t *fence)
{
    struct svga_screen *screen = &screens[unit];
    struct svga_region *pixels = screen->backing;
    uint32_t pitch = screen->width * 4;
    uint32_t width = rect.x2 - rect.x1, height = rect.y2 - rect.y1;

    if (!fb->offset && !scanout->x && !scanout->y && fb->pitch == pitch &&
        fb->width == screen->width && fb->height == screen->height) {
        if (!fb->mob) {
            int ret = svga_region_bind(SVGA_REGION_MOB, fb->bo, &fb->mob);
            if (ret && ret != -ENOSPC)
                return ret;
        }
        if (fb->mob)
            pixels = fb->mob;
    }
    if (screen->bound_mob != pixels) {
        /* A new backing defines the contents of the full image. */
        rect = (struct framebuffer_damage){0, 0, screen->width, screen->height};
        width = screen->width;
        height = screen->height;
    }
    bool staging = pixels == screen->backing;
    if (staging) {
        int ret = svga_buffer_wait(fb->bo);
        if (!ret)
            ret = svga_surface_cpu_access(fb->bo);
        if (ret)
            return ret;
        if (screen->staging_pending) {
            ret = svga_submit_wait(screen->staging_fence);
            if (ret)
                return ret;
            screen->staging_pending = false;
        }
        uint8_t *destination = svga_region_pixels(pixels);
        const uint8_t *source = fb->pixels + fb->offset +
            (uint64_t)(scanout->y + rect.y1) * fb->pitch + (scanout->x + rect.x1) * 4;
        destination += (uint64_t)rect.y1 * pitch + rect.x1 * 4;
        for (uint32_t y = 0; y < height; ++y)
            __builtin_memcpy(destination + (uint64_t)y * pitch,
                             source + (uint64_t)y * fb->pitch, width * 4);
    }
    const struct {
        SVGA_3D_PACKET(SVGA3dCmdBindGBScreenTarget) screen;
        SVGA_3D_PACKET(SVGA3dCmdBindGBSurface) bind;
        SVGA_3D_PACKET(SVGA3dCmdUpdateGBImage) image;
        SVGA_3D_PACKET(SVGA3dCmdUpdateGBScreenTarget) target;
    } packets = {
        .screen = {SVGA_HEADER(SVGA_3D_CMD_BIND_GB_SCREENTARGET, SVGA3dCmdBindGBScreenTarget),
                   {.stid = unit, .image = {.sid = unit}}},
        .bind = {SVGA_HEADER(SVGA_3D_CMD_BIND_GB_SURFACE, SVGA3dCmdBindGBSurface),
                 {unit, svga_region_id(pixels)}},
        .image = {SVGA_HEADER(SVGA_3D_CMD_UPDATE_GB_IMAGE, SVGA3dCmdUpdateGBImage),
                  {.image = {.sid = unit},
                   .box = {.x = rect.x1, .y = rect.y1, .w = width, .h = height, .d = 1}}},
        .target = {SVGA_HEADER(SVGA_3D_CMD_UPDATE_GB_SCREENTARGET, SVGA3dCmdUpdateGBScreenTarget),
                   {.stid = unit, .rect = {rect.x1, rect.y1, width, height}}},
    };
    unsigned skip = screen->bound_surface ? 0 : sizeof(packets.screen);
    if (!screen->bound_surface && screen->bound_mob == pixels)
        skip += sizeof(packets.bind);
    uint32_t sequence;
    int ret = svga_display_submit((const uint8_t *)&packets + skip, sizeof(packets) - skip,
                                  NULL, NULL, fb->bo, &sequence);
    if (ret)
        return ret;
    screen->bound_mob = pixels;
    retain_display_surface(screen, NULL);
    if (staging) {
        screen->staging_fence = sequence;
        screen->staging_pending = true;
    }
    if (fence)
        *fence = sequence;
    return 0;
}

int svga_screen_present(unsigned unit, struct svga_fb *fb, const struct svga_scanout *scanout,
                        struct framebuffer_damage rect, uint32_t *fence)
{
    return display_type == SVGA_DISPLAY_TARGET ? present_target(unit, fb, scanout, rect, fence) :
                                                 present_object(unit, fb, scanout, rect, fence);
}

static struct framebuffer_damage blit_bounds(const struct svga_blit *rects, unsigned count)
{
    struct framebuffer_damage bounds = {0};
    for (unsigned i = 0; i < count; ++i) {
        const struct svga_blit *r = &rects[i];
        framebuffer_damage_union(&bounds, (struct framebuffer_damage){
            r->dx, r->dy, r->dx + r->width, r->dy + r->height,
        });
    }
    return bounds;
}

/* All clipping is done in KMS before building packets. One blit with a
 * destination-relative clip list avoids a queue kick per damaged rectangle. */
static int blit_to_object(unsigned unit, struct svga_surface *surface, const struct svga_blit *rects,
                          unsigned count, uint32_t *sequence)
{
    struct screen_blit {
        SVGA3dCmdHeader header;
        SVGA3dCmdBlitSurfaceToScreen body;
        SVGASignedRect clips[];
    } *cmd;
    uint32_t bytes = sizeof(*cmd) + count * sizeof(cmd->clips[0]);
    cmd = kernel_malloc(bytes);
    if (!cmd)
        return -ENOMEM;
    struct framebuffer_damage b = blit_bounds(rects, count);
    int32_t tx = (int32_t)rects[0].sx - (int32_t)rects[0].dx;
    int32_t ty = (int32_t)rects[0].sy - (int32_t)rects[0].dy;
    cmd->header = (SVGA3dCmdHeader){SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN, bytes - sizeof(cmd->header)};
    cmd->body = (SVGA3dCmdBlitSurfaceToScreen){
        .srcImage = {.sid = surface->object.id}, .destScreenId = unit,
        .srcRect = {b.x1 + tx, b.y1 + ty, b.x2 + tx, b.y2 + ty},
        .destRect = {b.x1, b.y1, b.x2, b.y2},
    };
    for (unsigned i = 0; i < count; ++i) {
        const struct svga_blit *r = &rects[i];
        cmd->clips[i] = (SVGASignedRect){r->dx - b.x1, r->dy - b.y1,
                                       r->dx - b.x1 + r->width, r->dy - b.y1 + r->height};
    }
    int ret = svga_display_submit(cmd, bytes, surface, NULL, NULL, sequence);
    kernel_free(cmd);
    return ret;
}

static int blit_to_target(unsigned unit, struct svga_surface *surface, const struct svga_blit *rects,
                          unsigned count, bool scanout, uint32_t *sequence)
{
    struct svga_screen *screen = &screens[unit];
    bool direct = scanout && rects[0].sx == rects[0].dx && rects[0].sy == rects[0].dy &&
                  surface->info.base.base_size.width == screen->width &&
                  surface->info.base.base_size.height == screen->height &&
                  (surface->info.base.svga3d_flags & SVGA3D_SURFACE_SCREENTARGET);
    bool restore_backing = scanout && screen->bound_mob != screen->backing;
    struct svga_region *pixels = restore_backing ? screen->backing : screen->bound_mob;
    struct svga_surface *target = scanout ? (direct ? surface : NULL) : screen->bound_surface;
    bool rebind = target != screen->bound_surface;
    uint32_t sid = target ? target->object.id : unit;
    bool copy = !direct && surface != target;
    if (!direct && surface == target) {
        for (unsigned i = 0; i < count; ++i)
            if (rects[i].sx != rects[i].dx || rects[i].sy != rects[i].dy)
                return -EINVAL;
    }
    if (copy && surface->info.base.format != (target ? target->info.base.format : screen->format))
        return -EINVAL;
    SVGA_3D_PACKET(SVGA3dCmdBindGBScreenTarget) bind = {
        SVGA_HEADER(SVGA_3D_CMD_BIND_GB_SCREENTARGET, SVGA3dCmdBindGBScreenTarget),
        {.stid = unit, .image = {.sid = sid}},
    };
    const SVGA_3D_PACKET(SVGA3dCmdBindGBSurface) backing = {
        SVGA_HEADER(SVGA_3D_CMD_BIND_GB_SURFACE, SVGA3dCmdBindGBSurface),
        {unit, svga_region_id(pixels)},
    };
    struct framebuffer_damage b = blit_bounds(rects, count);
    SVGA_3D_PACKET(SVGA3dCmdUpdateGBScreenTarget) update = {
        SVGA_HEADER(SVGA_3D_CMD_UPDATE_GB_SCREENTARGET, SVGA3dCmdUpdateGBScreenTarget),
        {.stid = unit, .rect = {b.x1, b.y1, b.x2 - b.x1, b.y2 - b.y1}},
    };
    const SVGA_3D_PACKET(SVGA3dCmdSurfaceCopy) header = {
        {SVGA_3D_CMD_SURFACE_COPY, sizeof(SVGA3dCmdSurfaceCopy) + count * sizeof(SVGA3dCopyBox)},
        {.src = {.sid = surface->object.id}, .dest = {.sid = sid}},
    };
    uint32_t bytes = sizeof(update) + (restore_backing ? sizeof(backing) : 0) + (rebind ? sizeof(bind) : 0) +
        (copy ? sizeof(header) + count * sizeof(SVGA3dCopyBox) : 0);
    uint8_t small[sizeof(backing) + sizeof(bind) + sizeof(header) + sizeof(SVGA3dCopyBox) + sizeof(update)];
    uint8_t *commands = bytes <= sizeof(small) ? small : kernel_malloc(bytes);
    if (!commands)
        return -ENOMEM;
    uint8_t *p = commands;
    if (restore_backing) {
        __builtin_memcpy(p, &backing, sizeof(backing));
        p += sizeof(backing);
    }
    if (rebind) {
        __builtin_memcpy(p, &bind, sizeof(bind));
        p += sizeof(bind);
    }
    if (copy) {
        __builtin_memcpy(p, &header, sizeof(header));
        p += sizeof(header);
        for (unsigned i = 0; i < count; ++i) {
            const struct svga_blit *r = &rects[i];
            const SVGA3dCopyBox box = {.x = r->dx, .y = r->dy, .w = r->width,
                                     .h = r->height, .d = 1, .srcx = r->sx, .srcy = r->sy};
            __builtin_memcpy(p, &box, sizeof(box));
            p += sizeof(box);
        }
    }
    __builtin_memcpy(p, &update, sizeof(update));
    uint32_t submitted;
    int ret = svga_display_submit(commands, bytes, surface, copy ? target : NULL,
                                  target ? target->bo : svga_region_bo(pixels), &submitted);
    if (commands != small)
        kernel_free(commands);
    if (!ret) {
        screen->bound_mob = pixels;
        retain_display_surface(screen, target);
        if (!target && pixels == screen->backing) {
            /* A subsequent CPU upload must not overwrite an in-flight copy. */
            screen->staging_fence = submitted;
            screen->staging_pending = true;
        }
        if (sequence)
            *sequence = submitted;
    }
    return ret;
}

int svga_screen_blit(unsigned unit, struct svga_surface *surface, const struct svga_blit *rects,
                     unsigned count, bool scanout, uint32_t *sequence)
{
    if (display_type == SVGA_DISPLAY_TARGET)
        return surface->gb ? blit_to_target(unit, surface, rects, count, scanout, sequence) : -EINVAL;
    if (display_type == SVGA_DISPLAY_OBJECT)
        return blit_to_object(unit, surface, rects, count, sequence);
    return -ENOSYS;
}

static int readback_object(unsigned unit, struct svga_fb *fb, const struct svga_blit *rects,
                           unsigned count, uint32_t *sequence)
{
    if (object_vram.staging_bytes)
        return object_staging_blit(unit, fb, rects, count, true, sequence);
    if (!fb->gmr) {
        int ret = svga_region_bind(SVGA_REGION_GMR, fb->bo, &fb->gmr);
        if (ret)
            return ret;
    }
    struct readback_blit { uint32_t id; SVGAFifoCmdBlitScreenToGMRFB body; };
    struct readback_cmds {
        struct object_gmrfb gmr;
        struct readback_blit blit[];
    } *cmd;
    uint32_t bytes = sizeof(*cmd) + count * sizeof(cmd->blit[0]);
    cmd = kernel_malloc(bytes);
    if (!cmd)
        return -ENOMEM;
    cmd->gmr = object_framebuffer(svga_region_id(fb->gmr), fb->offset, fb->pitch);
    for (unsigned i = 0; i < count; ++i) {
        const struct svga_blit *r = &rects[i];
        cmd->blit[i] = (struct readback_blit){
            SVGA_CMD_BLIT_SCREEN_TO_GMRFB,
            {.srcScreenId = unit, .destOrigin = {r->dx, r->dy},
             .srcRect = {r->sx, r->sy, r->sx + r->width, r->sy + r->height}},
        };
    }
    int ret = svga_display_submit(cmd, bytes, NULL, NULL, fb->bo, sequence);
    kernel_free(cmd);
    return ret;
}

int svga_screen_readback(unsigned unit, struct svga_fb *fb, const struct svga_blit *rects,
                         unsigned count, uint32_t *sequence)
{
    struct svga_screen *screen = &screens[unit];
    if (display_type == SVGA_DISPLAY_OBJECT)
        return readback_object(unit, fb, rects, count, sequence);
    if (display_type != SVGA_DISPLAY_TARGET)
        return -ENOSYS;
    /* The target owns a linear backing. Download once, then scatter only
     * the requested rectangles into a potentially differently pitched FB. */
    struct svga_region *region = screen->bound_surface ? screen->bound_surface->backing : screen->bound_mob;
    if (!region)
        return -EINVAL;
    uint32_t offset = screen->bound_surface ? screen->bound_surface->offset : 0;
    const uint8_t *pixels = (const uint8_t *)svga_region_pixels(region) + offset;
    uint32_t pitch = (screen->bound_surface ? screen->bound_surface->info.base.base_size.width : screen->width) * 4;
    bool same_backing = svga_region_bo(region) == fb->bo;
    if (same_backing) {
        if (fb->pitch != pitch || fb->offset != offset)
            return -EINVAL;
        for (unsigned i = 0; i < count; ++i)
            if (rects[i].sx != rects[i].dx || rects[i].sy != rects[i].dy)
                return -EINVAL;
    }
    const SVGA_3D_PACKET(SVGA3dCmdReadbackGBImage) command = {
        SVGA_HEADER(SVGA_3D_CMD_READBACK_GB_IMAGE, SVGA3dCmdReadbackGBImage),
        {.image = {.sid = screen->bound_surface ? screen->bound_surface->object.id : unit}},
    };
    int ret = svga_display_submit(&command, sizeof(command), screen->bound_surface, NULL, fb->bo, sequence);
    if (!ret)
        ret = svga_submit_wait(*sequence);
    if (ret || same_backing)
        return ret;
    for (unsigned i = 0; i < count; ++i) {
        const struct svga_blit *r = &rects[i];
        uint8_t *dst = fb->pixels + fb->offset + (uint64_t)r->dy * fb->pitch + r->dx * 4;
        const uint8_t *src = pixels + (uint64_t)r->sy * pitch + r->sx * 4;
        for (uint32_t y = 0; y < r->height; ++y)
            __builtin_memcpy(dst + (uint64_t)y * fb->pitch, src + (uint64_t)y * pitch, r->width * 4);
    }
    return 0;
}
