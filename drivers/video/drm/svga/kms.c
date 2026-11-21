/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include "submit.h"
#include "dma.h"
#include "render.h"
#include <reliefnt/console.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>
#include <reliefnt/usercopy.h>

struct svga_scanout svga_scanouts[SVGA_MAX_OUTPUTS];

struct svga_mode_state {
    uint32_t width, height, bpp, enable, traces;
};

struct svga_legacy_display {
    uint32_t primary, x, y, width, height;
};

static struct svga_mode_state firmware;
static struct svga_legacy_display firmware_outputs[SVGA_MAX_OUTPUTS];
static uint32_t firmware_output_count, firmware_display_id, legacy_pitch, legacy_offset;
static bool display_owned, firmware_saved;
static uint32_t next_fb_id = SVGA_FB_ID_BASE;

struct svga_fb *svga_fb_lookup(const struct drm_file *file, uint32_t id)
{
    for (struct svga_fb *fb = file->svga ? file->svga->fbs : NULL; fb; fb = fb->next)
        if (fb->id == id)
            return fb;
    return NULL;
}

static int add_fb(struct drm_file *file, uint32_t width, uint32_t height,
                  uint32_t pitch, uint32_t offset, uint32_t format,
                  uint32_t handle, uint32_t *id)
{
    if (!svga_mode_fits(width, height) || (pitch & 3) || (offset & 3) ||
        pitch < width * 4 || (format != SVGA_XRGB8888 && format != SVGA_ARGB8888))
        return -EINVAL;
    if (next_fb_id == SVGA_BLOB_ID_BASE || (file->svga && file->svga->fb_count >= SVGA_MAX_FBS))
        return -ENOSPC;
    struct svga_surface *surface = (struct svga_surface *)svga_object_lookup(file, SVGA_SURFACE, handle);
    struct drm_bo *bo = NULL;
    uint8_t *pixels = NULL;
    if (surface) {
        const struct drm_vmw_gb_surface_create_req *meta = &surface->info.base;
        if (svga_screen_type() == SVGA_DISPLAY_LEGACY)
            return -ENOSYS;
        if (!(meta->drm_surface_flags & drm_vmw_surface_flag_scanout) ||
            surface->face_count != 1 || (surface->image_count && surface->image_count != 1) ||
            meta->mip_levels != 1 || meta->multisample_count > 1 || meta->base_size.depth != 1 ||
            meta->base_size.width < width || meta->base_size.height < height || offset ||
            (meta->format != SVGA3D_X8R8G8B8 && meta->format != SVGA3D_A8R8G8B8 &&
             meta->format != SVGA3D_B8G8R8X8_UNORM && meta->format != SVGA3D_B8G8R8A8_UNORM))
            return -EINVAL;
    } else {
        bo = drm_bo_lookup_handle_get(file, handle);
        if (!bo)
            return -ENOENT;
        uint64_t end = offset + (uint64_t)(height - 1) * pitch + width * 4;
        pixels = mm_phys_to_virt(drm_bo_page_physical(bo, 0));
        if (!pixels || end > drm_bo_size(bo)) {
            drm_bo_put(bo);
            return -EINVAL;
        }
    }
    struct svga_fb *fb = kernel_malloc(sizeof(*fb));
    if (!fb) {
        drm_bo_put(bo);
        return -ENOMEM;
    }
    if (!file->svga) {
        file->svga = kernel_malloc(sizeof(*file->svga));
        if (!file->svga) {
            kernel_free(fb);
            drm_bo_put(bo);
            return -ENOMEM;
        }
        *file->svga = (struct svga_file){0};
    }
    *fb = (struct svga_fb){
        .next = file->svga->fbs, .bo = bo, .surface = surface, .pixels = pixels, .handle = handle,
        .id = next_fb_id++, .width = width, .height = height,
        .pitch = pitch, .offset = offset, .format = format,
    };
    if (surface)
        svga_object_get(&surface->object);
    file->svga->fbs = fb;
    ++file->svga->fb_count;
    *id = fb->id;
    return 0;
}

static struct svga_mode_state read_mode(void)
{
    return (struct svga_mode_state){
        .width = svga_read(SVGA_REG_WIDTH), .height = svga_read(SVGA_REG_HEIGHT),
        .bpp = svga_read(SVGA_REG_BITS_PER_PIXEL), .enable = svga_read(SVGA_REG_ENABLE),
        .traces = (svga.caps & SVGA_CAP_TRACES) ? svga_read(SVGA_REG_TRACES) : 0,
    };
}

static void write_mode(struct svga_mode_state state)
{
    /* Register modes are only for LDU and for final firmware handback
     * after transport shutdown. They can reset CONFIG_DONE and FENCE. */
    if (state.width && state.height) {
        svga_write(SVGA_REG_WIDTH, state.width);
        svga_write(SVGA_REG_HEIGHT, state.height);
        if (svga.caps & SVGA_CAP_8BIT_EMULATION)
            svga_write(SVGA_REG_BITS_PER_PIXEL, state.bpp);
    }
    if (svga.caps & SVGA_CAP_TRACES)
        svga_write(SVGA_REG_TRACES, state.traces);
    svga_write(SVGA_REG_ENABLE, state.enable);
}

static void legacy_write_display(unsigned id, const struct svga_legacy_display *display)
{
    svga_write(SVGA_REG_DISPLAY_ID, id);
    svga_write(SVGA_REG_DISPLAY_IS_PRIMARY, display->primary);
    svga_write(SVGA_REG_DISPLAY_POSITION_X, display->x);
    svga_write(SVGA_REG_DISPLAY_POSITION_Y, display->y);
    svga_write(SVGA_REG_DISPLAY_WIDTH, display->width);
    svga_write(SVGA_REG_DISPLAY_HEIGHT, display->height);
}

static bool legacy_has_topology(void)
{
    return svga_screen_type() == SVGA_DISPLAY_LEGACY &&
           (svga.caps & SVGA_CAP_DISPLAY_TOPOLOGY);
}

static void claim_display(void)
{
    if (display_owned)
        return;
    if (!firmware_saved || svga_screen_type() == SVGA_DISPLAY_LEGACY) {
        firmware = read_mode();
        firmware_saved = true;
    }
    svga_console_hide();
    if (legacy_has_topology()) {
        firmware_display_id = svga_read(SVGA_REG_DISPLAY_ID);
        firmware_output_count = svga_read(SVGA_REG_NUM_GUEST_DISPLAYS);
        if (firmware_output_count > SVGA_MAX_OUTPUTS)
            firmware_output_count = SVGA_MAX_OUTPUTS;
        for (unsigned i = 0; i < firmware_output_count; ++i) {
            svga_write(SVGA_REG_DISPLAY_ID, i);
            firmware_outputs[i] = (struct svga_legacy_display){
                .primary = svga_read(SVGA_REG_DISPLAY_IS_PRIMARY),
                .x = svga_read(SVGA_REG_DISPLAY_POSITION_X),
                .y = svga_read(SVGA_REG_DISPLAY_POSITION_Y),
                .width = svga_read(SVGA_REG_DISPLAY_WIDTH),
                .height = svga_read(SVGA_REG_DISPLAY_HEIGHT),
            };
        }
        svga_write(SVGA_REG_DISPLAY_ID, firmware_display_id);
    }
    display_owned = true;
}

static void restore_display(void)
{
    if (!display_owned)
        return;
    /* Do not reuse legacy VRAM until the overlay has stopped reading it. */
    if (svga_overlay_stop() || (!svga.fifo.fault && svga_submit_idle()))
        svga.fifo.fault = true;
    display_owned = false;
    if (svga.fifo.fault)
        return;
    if (svga_screen_type() != SVGA_DISPLAY_LEGACY) {
        /* Keep the render transport live across VT handoff. */
        if (svga.ready) {
            int ret = svga_console_show();
            if (ret && ret != -ENODEV)
                console_warn("[svga] console scanout unavailable: %d", ret);
        }
        return;
    }
    write_mode(firmware);
    if (legacy_has_topology()) {
        svga_write(SVGA_REG_NUM_GUEST_DISPLAYS, firmware_output_count);
        for (unsigned i = 0; i < firmware_output_count; ++i)
            legacy_write_display(i, &firmware_outputs[i]);
        svga_write(SVGA_REG_DISPLAY_ID, firmware_display_id);
    }
}

void svga_kms_restore_firmware(void)
{
    if (firmware_saved && svga_screen_type() != SVGA_DISPLAY_LEGACY)
        write_mode(firmware);
    firmware_saved = false;
}

static void legacy_write_topology(const struct svga_scanout *state)
{
    if (!legacy_has_topology())
        return;
    unsigned count = 0;
    for (unsigned i = 0; i < svga_num_outputs; ++i)
        count += svga_scanout_visible(&state[i]);
    svga_write(SVGA_REG_NUM_GUEST_DISPLAYS, count);
    unsigned id = 0;
    for (unsigned i = 0; i < svga_num_outputs; ++i) {
        const struct svga_scanout *scanout = &state[i];
        if (!svga_scanout_visible(scanout))
            continue;
        const struct svga_legacy_display display = {
            .primary = !id, .x = scanout->x, .y = scanout->y,
            .width = scanout->mode.hdisplay, .height = scanout->mode.vdisplay,
        };
        legacy_write_display(id++, &display);
    }
}

static bool any_visible(const struct svga_scanout *state)
{
    for (unsigned i = 0; i < svga_num_outputs; ++i)
        if (svga_scanout_visible(&state[i]))
            return true;
    return false;
}

bool svga_kms_active(void)
{
    return display_owned && !svga.fifo.fault;
}

static void forget_crtc(unsigned unit)
{
    svga_cursor_disable(unit);
    if (display_owned)
        svga_screen_release(unit);
    struct svga_scanout *scanout = &svga_scanouts[unit];
    svga_blob_put(scanout->mode_blob);
    svga_blob_put(scanout->damage_blob);
    *scanout = (struct svga_scanout){0};
    svga_vblank_set(unit, scanout);
}

static void disable_crtc(unsigned unit)
{
    forget_crtc(unit);
    if (!any_visible(svga_scanouts))
        restore_display();
    else if (!svga.fifo.fault)
        legacy_write_topology(svga_scanouts);
}

void svga_kms_vt_release(void)
{
    svga_atomic_cancel(NULL);
    svga_overlay_release(NULL);
    for (unsigned i = 0; i < svga_num_outputs; ++i)
        forget_crtc(i);
    restore_display();
}

void svga_kms_remove_fb(struct drm_file *file, uint32_t id)
{
    if (!file->svga)
        return;
    struct svga_fb **link = &file->svga->fbs;
    while (*link && (*link)->id != id)
        link = &(*link)->next;
    if (!*link)
        return;
    struct svga_fb *fb = *link;
    for (unsigned i = 0; i < svga_num_outputs; ++i) {
        if (svga_scanouts[i].fb == fb)
            disable_crtc(i);
        else if (svga_scanouts[i].cursor.fb == fb) {
            svga_cursor_hide(i);
            svga_scanouts[i].cursor = (struct svga_cursor_plane){0};
        }
    }
    *link = fb->next;
    --file->svga->fb_count;
    svga_region_release(fb->mob);
    svga_region_release(fb->gmr);
    drm_bo_put(fb->bo);
    if (fb->surface)
        svga_object_put(&fb->surface->object);
    kernel_free(fb);
}

void svga_kms_file_release(struct drm_file *file)
{
    svga_atomic_cancel(file);
    svga_vblank_release(file);
    svga_overlay_release(file);
    for (unsigned i = 0; i < svga_num_outputs; ++i)
        if (svga_scanouts[i].owner == file)
            disable_crtc(i);
    svga_blobs_release(file);
    if (!file->svga)
        return;
    while (file->svga->fbs)
        svga_kms_remove_fb(file, file->svga->fbs->id);
    kernel_free(file->svga);
    file->svga = NULL;
}

static inline void copy_pixels(void *dst, const void *src, size_t count)
{
    __asm__ volatile("rep movsl" : "+D"(dst), "+S"(src), "+c"(count) :: "memory");
}

static void copy_vram(uint64_t offset, void *output, const void *input, size_t bytes)
{
    for (unsigned i = 0; bytes && i < svga.vram_count; ++i) {
        const struct svga_vram_map *map = &svga.vram[i];
        if (offset >= map->end)
            continue;
        size_t count = map->end - offset;
        if (count > bytes)
            count = bytes;
        uint8_t *mmio = map->address + (offset - map->start);
        uint8_t *dest = output ? (uint8_t *)output : mmio;
        const uint8_t *src = output ? mmio : (const uint8_t *)input;
        copy_pixels(dest, src, count / 4);
        if (count & 3)
            __builtin_memcpy(dest + (count & ~3ul), src + (count & ~3ul), count & 3);
        offset += count;
        if (output)
            output = dest + count;
        else
            input = src + count;
        bytes -= count;
    }
}

void svga_copy_to_vram(uint64_t offset, const uint8_t *pixels, size_t bytes)
{
    copy_vram(offset, NULL, pixels, bytes);
}

void svga_copy_from_vram(uint8_t *pixels, uint64_t offset, size_t bytes)
{
    copy_vram(offset, pixels, NULL, bytes);
}

/* Damage is in CRTC coordinates. Only the active extent is copied into the
 * validated direct scanout slice. No BO physical address reaches the host. */
static int present(unsigned unit, const struct svga_scanout *scanout,
                   struct framebuffer_damage rect, uint32_t *fence)
{
    struct svga_fb *fb = scanout->fb;
    int ret;
    if (svga_screen_type() != SVGA_DISPLAY_LEGACY) {
        if (fb->surface) {
            const struct svga_blit blit = {
                .sx = scanout->x + rect.x1, .sy = scanout->y + rect.y1,
                .dx = rect.x1, .dy = rect.y1, .width = rect.x2 - rect.x1, .height = rect.y2 - rect.y1,
            };
            return svga_screen_blit(unit, fb->surface, &blit, 1, true, fence);
        }
        return svga_screen_present(unit, fb, scanout, rect, fence);
    }
    ret = svga_buffer_wait(fb->bo);
    if (!ret)
        ret = svga_surface_cpu_access(fb->bo);
    if (ret)
        return ret;
    uint32_t width = rect.x2 - rect.x1, height = rect.y2 - rect.y1;
    const uint8_t *from = fb->pixels + fb->offset +
        (uint64_t)(scanout->y + rect.y1) * fb->pitch + (scanout->x + rect.x1) * 4;
    uint32_t x = rect.x1, y = rect.y1;
    if (svga_num_outputs > 1 || legacy_has_topology()) {
        x += scanout->x;
        y += scanout->y;
    }
    uint64_t to = legacy_offset + (uint64_t)y * legacy_pitch + x * 4;
    if (width * 4 == fb->pitch && fb->pitch == legacy_pitch) {
        svga_copy_to_vram(to, from, (size_t)width * height * 4);
    } else {
        for (uint32_t y = 0; y < height; ++y) {
            svga_copy_to_vram(to, from, width * 4);
            if (y + 1 < height) {
                from += fb->pitch;
                to += legacy_pitch;
            }
        }
    }

    uint32_t cmd[] = {SVGA_CMD_UPDATE, x, y, width, height};
    return fence ? svga_submit_fenced(cmd, sizeof(cmd), fence) : svga_submit(cmd, sizeof(cmd));
}

static struct framebuffer_damage full_damage(const struct svga_scanout *scanout)
{
    return (struct framebuffer_damage){0, 0, scanout->mode.hdisplay, scanout->mode.vdisplay};
}

static struct framebuffer_damage scanout_damage(const struct svga_scanout *s,
                                                 const struct svga_scanout *old)
{
    if (!s->damage_blob || !svga_scanout_visible(old) || s->fb != old->fb ||
        s->x != old->x || s->y != old->y || s->mode.hdisplay != old->mode.hdisplay ||
        s->mode.vdisplay != old->mode.vdisplay)
        return full_damage(s);

    unsigned count;
    const struct drm_mode_rect *clips = svga_damage_rects(s->damage_blob, &count);
    int32_t x = (int32_t)s->x, y = (int32_t)s->y;
    int32_t right = x + s->mode.hdisplay, bottom = y + s->mode.vdisplay;
    struct framebuffer_damage bounds = {0};
    for (unsigned i = 0; i < count; ++i) {
        const struct drm_mode_rect *r = &clips[i];
        int32_t x1 = r->x1 > x ? r->x1 : x, x2 = r->x2 < right ? r->x2 : right;
        int32_t y1 = r->y1 > y ? r->y1 : y, y2 = r->y2 < bottom ? r->y2 : bottom;
        if (x1 < x2 && y1 < y2)
            framebuffer_damage_union(&bounds, (struct framebuffer_damage){x1 - x, y1 - y, x2 - x, y2 - y});
    }
    return bounds;
}

static bool fb_fits(const struct svga_fb *fb, uint32_t x, uint32_t y,
                    uint32_t width, uint32_t height)
{
    return width <= fb->width && height <= fb->height &&
        x <= fb->width - width && y <= fb->height - height;
}

/* The user clip list is in source coordinates. Translate it once per head,
 * then intersect it with the head's viewport. */
static unsigned clip_blits(const struct svga_scanout *scanout,
                           const struct drm_vmw_rect *clips, unsigned count,
                           uint32_t src_width, uint32_t src_height, int64_t dx, int64_t dy,
                           bool readback, struct svga_blit *rects)
{
    unsigned hits = 0;
    int64_t right = (int64_t)scanout->mode.hdisplay - dx;
    int64_t bottom = (int64_t)scanout->mode.vdisplay - dy;
    for (unsigned i = 0; i < count; ++i) {
        const struct drm_vmw_rect *clip = &clips[i];
        int64_t x1 = clip->x, y1 = clip->y, x2 = x1 + clip->w, y2 = y1 + clip->h;
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x1 < -dx) x1 = -dx;
        if (y1 < -dy) y1 = -dy;
        if (x2 > src_width) x2 = src_width;
        if (y2 > src_height) y2 = src_height;
        if (x2 > right) x2 = right;
        if (y2 > bottom) y2 = bottom;
        if (x2 <= x1 || y2 <= y1)
            continue;
        struct svga_blit r = {
            .sx = x1, .sy = y1, .dx = x1 + dx, .dy = y1 + dy,
            .width = x2 - x1, .height = y2 - y1,
        };
        if (readback)
            r = (struct svga_blit){r.dx, r.dy, r.sx, r.sy, r.width, r.height};
        rects[hits++] = r;
    }
    return hits;
}

struct svga_clips {
    struct svga_blit *rects;
    struct drm_vmw_rect *clips;
};

static int get_clips(struct task *task, uint64_t address, uint32_t count, struct svga_clips *out)
{
    if (!address || count > DRM_MODE_FB_DIRTY_MAX_CLIPS)
        return -EINVAL;
    struct svga_blit *rects = kernel_malloc(count * (sizeof(*rects) + sizeof(*out->clips)));
    if (!rects)
        return -ENOMEM;
    struct drm_vmw_rect *clips = (struct drm_vmw_rect *)(rects + count);
    int ret = svga_import(task, clips, address, count * sizeof(*clips));
    if (ret)
        kernel_free(rects);
    else
        *out = (struct svga_clips){.rects = rects, .clips = clips};
    return ret;
}

int svga_kms_present_ioctl(struct task *task, struct drm_file *file, const struct drm_vmw_present_arg *arg)
{
    if (!arg->num_clips)
        return 0;
    struct svga_fb *fb = svga_fb_lookup(file, arg->fb_id);
    struct svga_surface *surface = (struct svga_surface *)svga_object_lookup(file, SVGA_SURFACE, arg->sid);
    if (!fb || !surface)
        return -ENOENT;
    if (svga_screen_type() == SVGA_DISPLAY_LEGACY)
        return -ENOSYS;
    struct svga_clips clips;
    int ret = get_clips(task, arg->clips_ptr, arg->num_clips, &clips);
    if (ret)
        return ret;
    const struct drm_vmw_size *image = &surface->info.base.base_size;
    for (unsigned i = 0; !ret && i < svga_num_outputs; ++i) {
        const struct svga_scanout *scanout = &svga_scanouts[i];
        if (!scanout->active || scanout->fb != fb)
            continue;
        unsigned hits = clip_blits(scanout, clips.clips, arg->num_clips, image->width, image->height,
                                   (int64_t)arg->dest_x - scanout->x, (int64_t)arg->dest_y - scanout->y,
                                   false, clips.rects);
        if (hits)
            ret = svga_screen_blit(i, surface, clips.rects, hits, false, NULL);
    }
    kernel_free(clips.rects);
    return ret;
}

int svga_kms_readback_ioctl(struct task *task, struct drm_file *file, const struct drm_vmw_present_readback_arg *arg)
{
    if (!arg->num_clips)
        return 0;
    struct svga_fb *fb = svga_fb_lookup(file, arg->fb_id);
    if (!fb)
        return -ENOENT;
    if (!fb->bo)
        return -EINVAL;
    if (svga_screen_type() == SVGA_DISPLAY_LEGACY)
        return -ENOSYS;
    struct svga_clips clips;
    int ret = get_clips(task, arg->clips_ptr, arg->num_clips, &clips);
    if (ret)
        return ret;
    struct svga_fence *fence = NULL;
    if (arg->fence_rep) {
        ret = svga_prepare_output(task, arg->fence_rep, sizeof(struct drm_vmw_fence_rep));
        if (ret)
            goto out;
        fence = (struct svga_fence *)svga_object_create(file, SVGA_FENCE, sizeof(*fence));
        if (!fence) {
            ret = -ENOMEM;
            goto out;
        }
    }
    uint32_t sequence;
    bool submitted = false;
    for (unsigned i = 0; !ret && i < svga_num_outputs; ++i) {
        const struct svga_scanout *scanout = &svga_scanouts[i];
        if (!scanout->active || scanout->fb != fb)
            continue;
        unsigned hits = clip_blits(scanout, clips.clips, arg->num_clips, fb->width, fb->height,
                                   -(int64_t)scanout->x, -(int64_t)scanout->y, true, clips.rects);
        if (hits) {
            ret = svga_screen_readback(i, fb, clips.rects, hits, &sequence);
            submitted = true;
        }
    }
    if (!ret && !submitted && fence)
        ret = svga_submit_checkpoint(&sequence);
    if (!ret && fence) {
        fence->sequence = sequence;
        struct drm_vmw_fence_rep reply;
        svga_fence_fill(fence, &reply);
        if (user_copy_to_task(task, arg->fence_rep, &reply, sizeof(reply)) < 0) {
            (void)svga_submit_wait(sequence);
            ret = -EFAULT;
        }
    }
out:
    if (ret && fence)
        (void)svga_object_unref(file, SVGA_FENCE, fence->object.handle);
    kernel_free(clips.rects);
    return ret;
}

static int legacy_program_mode(const struct svga_scanout *state)
{
    uint32_t width = 0, height = 0;
    for (unsigned i = 0; i < svga_num_outputs; ++i) {
        const struct svga_scanout *scanout = &state[i];
        if (!svga_scanout_visible(scanout))
            continue;
        uint32_t w = scanout->mode.hdisplay, h = scanout->mode.vdisplay;
        if (legacy_has_topology()) {
            w = scanout->fb->width;
            h = scanout->fb->height;
        } else if (svga_num_outputs > 1) {
            w += scanout->x;
            h += scanout->y;
        }
        if (w > width)
            width = w;
        if (h > height)
            height = h;
    }
    if (!svga_mode_fits(width, height))
        return -EINVAL;
    int ret = svga_overlay_stop();
    if (!ret)
        ret = svga_submit_idle();
    if (ret)
        return ret;
    write_mode((struct svga_mode_state){
        .enable = SVGA_REG_ENABLE_ENABLE, .bpp = 32, .width = width, .height = height,
    });
    uint32_t offset = svga_read(SVGA_REG_FB_OFFSET);
    uint32_t pitch = svga_read(SVGA_REG_BYTES_PER_LINE);
    if (svga_read(SVGA_REG_WIDTH) != width || svga_read(SVGA_REG_HEIGHT) != height ||
        svga_read(SVGA_REG_BITS_PER_PIXEL) != 32 || svga_read(SVGA_REG_DEPTH) != 24 ||
        svga_read(SVGA_REG_RED_MASK) != 0xff0000 || svga_read(SVGA_REG_GREEN_MASK) != 0xff00 ||
        svga_read(SVGA_REG_BLUE_MASK) != 0xff || (pitch & 3) || (offset & 3) ||
        pitch < width * 4u || offset + (uint64_t)pitch * height > svga.vram_bytes)
        return -EIO;
    legacy_pitch = pitch;
    legacy_offset = offset;
    legacy_write_topology(state);
    for (unsigned i = 0; i < svga_num_outputs; ++i) {
        const struct svga_scanout *scanout = &state[i];
        if (svga_scanout_visible(scanout)) {
            ret = present(i, scanout, full_damage(scanout), NULL);
            if (ret)
                return ret;
        }
    }
    return svga_submit_idle();
}

int svga_kms_check(const struct svga_scanout *state)
{
    struct drm_vmw_rect rects[SVGA_MAX_OUTPUTS] = {0};
    bool legacy = svga_screen_type() == SVGA_DISPLAY_LEGACY;
    struct svga_fb *primary = NULL;
    for (unsigned i = 0; i < svga_num_outputs; ++i) {
        const struct svga_scanout *s = &state[i];
        bool enabled = s->mode_blob != NULL;
        if (enabled != s->connector_routed || (!enabled && (s->active || s->fb)) ||
            s->plane_routed != (s->fb != NULL))
            return -EINVAL;
        int ret = svga_cursor_plane_check(i, s);
        if (ret)
            return ret;
        if (!enabled)
            continue;
        uint32_t width = s->mode.hdisplay, height = s->mode.vdisplay;
        if (!svga_layout[i].w || !svga_mode_fits(width, height) ||
            (s->mode.flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN | DRM_MODE_FLAG_3D_MASK)))
            return -EINVAL;
        if (s->fb && (s->dst_x || s->dst_y || s->dst_w != width || s->dst_h != height ||
                      s->src_w != width << 16 || s->src_h != height << 16 ||
                      !fb_fits(s->fb, s->x, s->y, width, height)))
            return -EINVAL;
        if (legacy && svga_scanout_visible(s)) {
            if (primary && primary != s->fb)
                return -EINVAL;
            primary = s->fb;
        }
        rects[i] = (struct drm_vmw_rect){
            .x = legacy && svga_num_outputs > 1 ? s->x : s->root_x,
            .y = legacy && svga_num_outputs > 1 ? s->y : s->root_y,
            .w = width, .h = height,
        };
    }
    return svga_layout_check(rects, svga_num_outputs);
}

/* Validate the full topology before submitting any device commands. Use one
 * completion point per transaction; the device has no cross-screen latch. */
static int program_layout(const struct svga_scanout *state, const struct svga_scanout *previous,
                          unsigned changed, bool restore)
{
    if (!any_visible(state)) {
        for (unsigned i = 0; i < svga_num_outputs; ++i)
            if (changed & (1u << i))
                svga_screen_release(i);
        restore_display();
        return svga.fifo.fault ? -EIO : 0;
    }
    bool first = !display_owned;
    claim_display();
    bool legacy = svga_screen_type() == SVGA_DISPLAY_LEGACY;
    if (legacy) {
        if (first || restore)
            return legacy_program_mode(state);
        for (unsigned i = 0; i < svga_num_outputs; ++i) {
            if (!(changed & (1u << i)))
                continue;
            const struct svga_scanout *s = &state[i], *old = &previous[i];
            if (svga_scanout_visible(s) != svga_scanout_visible(old) ||
                (svga_scanout_visible(s) &&
                 (s->mode.hdisplay != old->mode.hdisplay || s->mode.vdisplay != old->mode.vdisplay ||
                  (svga_num_outputs > 1 && (s->x != old->x || s->y != old->y)) ||
                  (legacy_has_topology() && (s->x != old->x || s->y != old->y ||
                    s->fb->width != old->fb->width || s->fb->height != old->fb->height)))))
                return legacy_program_mode(state);
        }
    } else if (first) {
        svga_write(SVGA_REG_ENABLE, SVGA_REG_ENABLE_ENABLE);
        if (svga.caps & SVGA_CAP_TRACES)
            svga_write(SVGA_REG_TRACES, 0);
    }

    for (unsigned i = 0; i < svga_num_outputs; ++i)
        if ((changed & (1u << i)) && !svga_scanout_visible(&state[i]))
            svga_screen_release(i);
    if (svga.fifo.fault)
        return -EIO;
    uint32_t fence = 0;
    for (unsigned i = 0; i < svga_num_outputs; ++i) {
        const struct svga_scanout *s = &state[i];
        if (!(changed & (1u << i)) || !svga_scanout_visible(s))
            continue;
        int ret = 0;
        if (!legacy) {
            /* A definition can follow the last present, even without damage. */
            fence = 0;
            ret = svga_screen_configure(i, s);
        }
        struct framebuffer_damage damage = first || restore ? full_damage(s) : scanout_damage(s, &previous[i]);
        if (!ret && damage.x1 < damage.x2)
            ret = present(i, s, damage, legacy ? NULL : &fence);
        if (ret)
            return ret;
    }
    return svga_submit_wait(fence);
}

int svga_kms_commit(const struct svga_scanout *state, unsigned scanouts, unsigned cursors)
{
    unsigned changed = scanouts | cursors;
    if (!changed)
        return 0;
    if (svga.fifo.fault)
        return -EIO;
    unsigned hardware = 0;
    for (unsigned i = 0; i < svga_num_outputs; ++i)
        if ((scanouts & (1u << i)) && (svga_scanout_visible(&state[i]) ||
                                       svga_scanout_visible(&svga_scanouts[i])))
            hardware |= 1u << i;
    int ret = hardware ? program_layout(state, svga_scanouts, hardware, false) : 0;
    if (!ret)
        ret = svga_cursor_commit(state, cursors);
    if (ret) {
        if (svga.fifo.fault || (hardware && program_layout(svga_scanouts, state, hardware, true)))
            svga_kms_vt_release();
        return ret;
    }
    for (unsigned i = 0; i < svga_num_outputs; ++i)
        if (changed & (1u << i)) {
            svga_blob_get(state[i].mode_blob);
            svga_blob_get(state[i].damage_blob);
        }
    for (unsigned i = 0; i < svga_num_outputs; ++i) {
        if (!(changed & (1u << i)))
            continue;
        if (!state[i].mode_blob)
            svga_cursor_disable(i);
        else if (!svga_scanout_visible(&state[i]))
            svga_cursor_hide(i);
        svga_blob_put(svga_scanouts[i].mode_blob);
        svga_blob_put(svga_scanouts[i].damage_blob);
        svga_scanouts[i] = state[i];
        svga_scanouts[i].cursor.refresh = false;
        svga_vblank_set(i, &state[i]);
    }
    return 0;
}

static int set_crtc(struct task *task, struct drm_file *file, const struct drm_mode_crtc *arg)
{
    int unit = svga_output_index(arg->crtc_id, SVGA_CRTC_ID);
    if (unit < 0)
        return -ENOENT;
    if (!arg->mode_valid) {
        if (arg->count_connectors)
            return -EINVAL;
        disable_crtc(unit);
        return 0;
    }
    if (svga.fifo.fault)
        return -EIO;
    const struct drm_mode_modeinfo *mode = &arg->mode;
    if (arg->count_connectors != 1 || !svga_layout[unit].w ||
        !svga_mode_fits(mode->hdisplay, mode->vdisplay) ||
        (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN | DRM_MODE_FLAG_3D_MASK)))
        return -EINVAL;
    uint32_t connector;
    int ret = svga_import(task, &connector, arg->set_connectors_ptr, sizeof(connector));
    if (ret)
        return ret;
    if (connector != SVGA_CONNECTOR_ID + (unsigned)unit)
        return -ENOENT;
    const struct svga_scanout *old = &svga_scanouts[unit];
    struct svga_fb *fb = arg->fb_id == UINT32_MAX && old->owner == file ? old->fb : svga_fb_lookup(file, arg->fb_id);
    if (!fb)
        return -ENOENT;
    if (!fb_fits(fb, arg->x, arg->y, mode->hdisplay, mode->vdisplay))
        return -EINVAL;
    if (arg->x > UINT16_MAX || arg->y > UINT16_MAX)
        return -EINVAL;
    struct svga_blob *blob;
    ret = svga_blob_alloc(NULL, sizeof(*mode), &blob);
    if (ret)
        return ret;
    struct svga_scanout state[SVGA_MAX_OUTPUTS];
    __builtin_memcpy(state, svga_scanouts, sizeof(state));
    state[unit] = (struct svga_scanout){
        .fb = fb, .owner = file, .mode = *mode, .mode_blob = blob, .x = arg->x, .y = arg->y,
        .root_x = svga_layout[unit].x, .root_y = svga_layout[unit].y,
        .src_w = (uint32_t)mode->hdisplay << 16, .src_h = (uint32_t)mode->vdisplay << 16,
        .dst_w = mode->hdisplay, .dst_h = mode->vdisplay,
        .active = true, .connector_routed = true, .plane_routed = true,
    };
    state[unit].mode.name[sizeof(mode->name) - 1] = 0;
    if (old->owner == file)
        state[unit].cursor = old->cursor;
    __builtin_memcpy(svga_blob_data(blob), &state[unit].mode, sizeof(*mode));
    ret = svga_kms_check(state);
    if (!ret)
        ret = svga_kms_commit(state, 1u << unit, 0);
    svga_blob_put(blob);
    return ret;
}

static int dirty_fb(struct task *task, struct drm_file *file, const struct drm_mode_fb_dirty_cmd *arg)
{
    struct svga_fb *fb = svga_fb_lookup(file, arg->fb_id);
    if (!fb)
        return -ENOENT;
    if (arg->flags || arg->num_clips > DRM_MODE_FB_DIRTY_MAX_CLIPS)
        return -EINVAL;
    if (svga.fifo.fault)
        return -EIO;
    struct drm_clip_rect *clips = NULL;
    int ret = 0;
    if (arg->num_clips) {
        unsigned bytes = arg->num_clips * sizeof(*clips);
        clips = kernel_malloc(bytes);
        if (!clips)
            return -ENOMEM;
        ret = svga_import(task, clips, arg->clips_ptr, bytes);
        for (unsigned i = 0; !ret && i < arg->num_clips; ++i) {
            const struct drm_clip_rect *r = &clips[i];
            if (r->x1 >= r->x2 || r->y1 >= r->y2 || r->x2 > fb->width || r->y2 > fb->height)
                ret = -EINVAL;
        }
    }
    for (unsigned i = 0; !ret && i < svga_num_outputs; ++i) {
        const struct svga_scanout *s = &svga_scanouts[i];
        if (!s->active || s->fb != fb)
            continue;
        struct framebuffer_damage bounds = {0};
        if (!clips)
            bounds = full_damage(s);
        uint32_t right = s->x + s->mode.hdisplay, bottom = s->y + s->mode.vdisplay;
        for (unsigned j = 0; j < arg->num_clips; ++j) {
            const struct drm_clip_rect *r = &clips[j];
            if (r->x2 <= s->x || r->y2 <= s->y || r->x1 >= right || r->y1 >= bottom)
                continue;
            framebuffer_damage_union(&bounds, (struct framebuffer_damage){
                r->x1 <= s->x ? 0 : r->x1 - s->x, r->y1 <= s->y ? 0 : r->y1 - s->y,
                (r->x2 < right ? r->x2 : right) - s->x, (r->y2 < bottom ? r->y2 : bottom) - s->y,
            });
        }
        if (bounds.x1 != bounds.x2)
            ret = present(i, s, bounds, NULL);
    }
    kernel_free(clips);
    if (ret && svga.fifo.fault)
        svga_kms_vt_release();
    return ret;
}

int svga_event_reserve(struct drm_file *file)
{
    struct svga_file *state = file->svga;
    if (!state) {
        state = kernel_malloc(sizeof(*state));
        if (!state)
            return -ENOMEM;
        *state = (struct svga_file){0};
        file->svga = state;
    }
    if (state->count + state->reserved == SVGA_MAX_EVENTS)
        return -EBUSY;
    ++state->reserved;
    return 0;
}

void svga_event_cancel(struct drm_file *file)
{
    if (file->svga)
        --file->svga->reserved;
}

void svga_event_publish(struct drm_file *file, const struct drm_event *event)
{
    struct svga_file *state = file->svga;
    unsigned slot = (state->head + state->count) % SVGA_MAX_EVENTS;
    __builtin_memcpy(&state->events[slot], event, event->length);
    --state->reserved;
    ++state->count;
    (void)kernel_wait_queue_wake_all(&file->event_wait);
}

static int page_flip(struct drm_file *file, const struct drm_mode_crtc_page_flip *arg)
{
    int unit = svga_output_index(arg->crtc_id, SVGA_CRTC_ID);
    if (unit < 0)
        return -ENOENT;
    if (arg->reserved || (arg->flags & ~DRM_MODE_PAGE_FLIP_EVENT))
        return -EINVAL;
    const struct svga_scanout *scanout = &svga_scanouts[unit];
    if (!svga_kms_active() || !svga_scanout_visible(scanout) || scanout->owner != file)
        return -EINVAL;
    struct svga_fb *fb = svga_fb_lookup(file, arg->fb_id);
    if (!fb)
        return -ENOENT;
    struct svga_scanout state[SVGA_MAX_OUTPUTS];
    __builtin_memcpy(state, svga_scanouts, sizeof(state));
    state[unit].fb = fb;
    state[unit].damage_blob = NULL;
    int ret = svga_kms_check(state);
    if (ret)
        return ret;
    struct svga_vblank_event *events[SVGA_MAX_OUTPUTS] = {0};
    if (arg->flags & DRM_MODE_PAGE_FLIP_EVENT) {
        ret = svga_vblank_reserve(file, unit, DRM_EVENT_FLIP_COMPLETE, arg->user_data, &events[unit]);
        if (ret)
            return ret;
    }
    ret = svga_queue_commit(file, state, 1u << unit, 0, events);
    if (ret)
        svga_vblank_cancel(events[unit]);
    return ret;
}

bool svga_events_pending(const struct drm_file *file)
{
    return file->svga && file->svga->count;
}

int svga_events_read(struct task *task, struct drm_file *file, uint64_t address, uint32_t length)
{
    if (!svga_events_pending(file))
        return 0;
    struct svga_file *state = file->svga;
    unsigned count = 0, total = 0;
    for (; count < state->count; ++count) {
        unsigned slot = (state->head + count) % SVGA_MAX_EVENTS;
        unsigned bytes = state->events[slot].base.length;
        if (bytes > length - total)
            break;
        total += bytes;
    }
    if (!count)
        return -EINVAL;
#ifdef CONFIG_SWAP
    int ret = drm_user_prepare(task, address, total, true);
    if (ret)
        return ret;
#endif
    unsigned copied = 0;
    while (count--) {
        unsigned bytes = state->events[state->head].base.length;
        if (user_copy_to_task(task, address + copied, &state->events[state->head], bytes) < 0)
            return copied ? (int)copied : -EFAULT;
        copied += bytes;
        --state->count;
        state->head = (state->head + 1) % SVGA_MAX_EVENTS;
    }
    return copied;
}

int svga_kms_ioctl(struct task *task, struct drm_file *file, uint32_t command,
                   void *data, uint32_t *created_fb)
{
    if (command != DRM_IOCTL_MODE_ATOMIC)
        svga_atomic_drain();
    switch (command) {
    case DRM_IOCTL_WAIT_VBLANK:
    case DRM_IOCTL_CRTC_GET_SEQUENCE:
    case DRM_IOCTL_CRTC_QUEUE_SEQUENCE:
        return svga_vblank_ioctl(task, file, command, data);
    case DRM_IOCTL_MODE_ADDFB: {
        struct drm_mode_fb_cmd *arg = data;
        if (arg->bpp != 32 || (arg->depth != 24 && arg->depth != 32))
            return -EINVAL;
        int ret = add_fb(file, arg->width, arg->height, arg->pitch, 0,
                         arg->depth == 24 ? SVGA_XRGB8888 : SVGA_ARGB8888,
                         arg->handle, &arg->fb_id);
        if (!ret)
            *created_fb = arg->fb_id;
        return ret;
    }
    case DRM_IOCTL_MODE_ADDFB2: {
        struct drm_mode_fb_cmd2 *arg = data;
        if (arg->flags || arg->modifier[0])
            return -EINVAL;
        for (unsigned i = 1; i < 4; ++i)
            if (arg->handles[i] || arg->pitches[i] || arg->offsets[i] || arg->modifier[i])
                return -EINVAL;
        int ret = add_fb(file, arg->width, arg->height, arg->pitches[0], arg->offsets[0],
                         arg->pixel_format, arg->handles[0], &arg->fb_id);
        if (!ret)
            *created_fb = arg->fb_id;
        return ret;
    }
    case DRM_IOCTL_MODE_RMFB: {
        uint32_t id = *(uint32_t *)data;
        if (!svga_fb_lookup(file, id))
            return -ENOENT;
        svga_kms_remove_fb(file, id);
        return 0;
    }
    case DRM_IOCTL_MODE_SETCRTC:
        return set_crtc(task, file, data);
    case DRM_IOCTL_MODE_ATOMIC:
        return svga_atomic_ioctl(task, file, data);
    case DRM_IOCTL_MODE_CREATEPROPBLOB:
    case DRM_IOCTL_MODE_DESTROYPROPBLOB:
    case DRM_IOCTL_MODE_GETPROPBLOB:
        return svga_property_blob_ioctl(task, file, command, data);
    case DRM_IOCTL_MODE_DIRTYFB:
        return dirty_fb(task, file, data);
    case DRM_IOCTL_MODE_PAGE_FLIP:
        return page_flip(file, data);
    case DRM_IOCTL_MODE_CURSOR: {
        const struct drm_mode_cursor *arg = data;
        const struct drm_mode_cursor2 cursor = {
            .flags = arg->flags, .crtc_id = arg->crtc_id, .x = arg->x, .y = arg->y,
            .width = arg->width, .height = arg->height, .handle = arg->handle,
        };
        return svga_cursor_legacy(file, &cursor);
    }
    case DRM_IOCTL_MODE_CURSOR2:
        return svga_cursor_legacy(file, data);
    default:
        return svga_kms_query(task, file, command, data);
    }
}
