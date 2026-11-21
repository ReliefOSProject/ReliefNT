/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include "dma.h"
#include "render.h"
#include "submit.h"
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>

#define SVGA_LEGACY_CURSOR_MAX 64u
#define SVGA_CURSOR_DIMENSION_LIMIT 256u

struct svga_cursor_position {
    int32_t x, y, hot_x, hot_y, bypass_x, bypass_y;
};

static struct svga_cursor_position positions[SVGA_MAX_OUTPUTS];
static struct svga_region *cursor_mob;
static int visible_unit = -1;
static bool atomic_cursor;

static bool cursor_uses_mob(void)
{
    return (svga.caps2 & SVGA_CAP2_CURSOR_MOB) && svga_dma_supported(SVGA_REGION_MOB);
}

uint32_t svga_cursor_limit(void)
{
    if (cursor_uses_mob()) {
        uint32_t max = svga_read(SVGA_REG_CURSOR_MAX_DIMENSION);
        return max < SVGA_CURSOR_DIMENSION_LIMIT ? max : SVGA_CURSOR_DIMENSION_LIMIT;
    }
    return (svga.caps & (SVGA_CAP_CURSOR | SVGA_CAP_ALPHA_CURSOR)) ? SVGA_LEGACY_CURSOR_MAX : 0;
}

bool svga_cursor_plane_available(const struct drm_file *file)
{
    return file->atomic_modeset && file->cursor_hotspot && svga_cursor_limit();
}

static void cursor_position(bool show, int32_t x, int32_t y)
{
    uint32_t on = show ? SVGA_CURSOR_ON_SHOW : SVGA_CURSOR_ON_HIDE;
    uint32_t count;
    if (svga.caps2 & SVGA_CAP2_EXTRA_REGS) {
        svga_write(SVGA_REG_CURSOR4_X, x);
        svga_write(SVGA_REG_CURSOR4_Y, y);
        svga_write(SVGA_REG_CURSOR4_SCREEN_ID, SVGA3D_INVALID_ID);
        svga_write(SVGA_REG_CURSOR4_ON, on);
        svga_write(SVGA_REG_CURSOR4_SUBMIT, 1);
    } else if ((svga.fifo.caps & SVGA_FIFO_CAP_CURSOR_BYPASS_3) &&
               svga_fifo_read_reg(&svga.fifo, SVGA_FIFO_CURSOR_COUNT, &count)) {
        volatile uint32_t *fifo = svga.fifo.mem;
        fifo[SVGA_FIFO_CURSOR_X] = x;
        fifo[SVGA_FIFO_CURSOR_Y] = y;
        fifo[SVGA_FIFO_CURSOR_ON] = on;
        __asm__ volatile("sfence" ::: "memory");
        fifo[SVGA_FIFO_CURSOR_COUNT] = count + 1;
    } else {
        svga_write(SVGA_REG_CURSOR_X, x);
        svga_write(SVGA_REG_CURSOR_Y, y);
        svga_write(SVGA_REG_CURSOR_ON, on);
    }
}

void svga_cursor_hide(unsigned unit)
{
    if (visible_unit != (int)unit)
        return;
    cursor_position(false, 0, 0);
    if (cursor_mob) {
        svga_write(SVGA_REG_CURSOR_MOBID, SVGA3D_INVALID_ID);
        svga_region_release(cursor_mob);
        cursor_mob = NULL;
    }
    visible_unit = -1;
    atomic_cursor = false;
}

void svga_cursor_disable(unsigned unit)
{
    svga_cursor_hide(unit);
    positions[unit] = (struct svga_cursor_position){0};
}

static void copy_cursor_pixels(uint32_t *dst, const uint32_t *src, uint32_t pitch,
                               uint32_t width, uint32_t height)
{
    if (pitch == width) {
        __builtin_memcpy(dst, src, (size_t)width * height * sizeof(*src));
        return;
    }
    for (uint32_t y = 0; y < height; ++y)
        __builtin_memcpy(dst + y * width, src + (size_t)y * pitch, width * sizeof(*src));
}

static int define_mob_cursor(const uint32_t *pixels, uint32_t pitch, uint32_t width, uint32_t height,
                             uint32_t hot_x, uint32_t hot_y)
{
    uint32_t image_bytes = width * height * sizeof(*pixels);
    uint32_t bytes = sizeof(SVGAGBCursorHeader) + image_bytes;
    uint32_t limit = svga_read(SVGA_REG_CURSOR_MAX_BYTE_SIZE);
    if ((limit && bytes > limit) || bytes > svga_dma_max_mob_size())
        return -EINVAL;
    struct svga_region *mob;
    int ret = svga_region_create(SVGA_REGION_MOB, bytes, &mob);
    if (ret)
        return ret;
    SVGAGBCursorHeader *header = svga_region_pixels(mob);
    *header = (SVGAGBCursorHeader){
        .type = SVGA_ALPHA_CURSOR, .sizeInBytes = image_bytes,
        .header.alphaHeader = {.hotspotX = hot_x, .hotspotY = hot_y, .width = width, .height = height},
    };
    copy_cursor_pixels((uint32_t *)(header + 1), pixels, pitch, width, height);
    uint32_t fence;
    ret = svga_submit_checkpoint(&fence);
    if (!ret)
        ret = svga_submit_wait(fence);
    if (ret) {
        svga_region_release(mob);
        return ret;
    }
    __asm__ volatile("sfence" ::: "memory");
    svga_write(SVGA_REG_CURSOR_MOBID, svga_region_id(mob));
    svga_region_release(cursor_mob);
    cursor_mob = mob;
    return 0;
}

static int define_fifo_cursor(const uint32_t *pixels, uint32_t pitch, uint32_t width, uint32_t height,
                              uint32_t hot_x, uint32_t hot_y)
{
    struct alpha_command { uint32_t id; SVGAFifoCmdDefineAlphaCursor body; };
    struct color_command { uint32_t id; SVGAFifoCmdDefineCursor body; };
    bool alpha = svga.caps & SVGA_CAP_ALPHA_CURSOR;
    uint32_t stride = (width + 31) / 32;
    uint32_t mask_words = stride * height;
    uint32_t bytes = width * height * sizeof(*pixels) +
        (alpha ? sizeof(struct alpha_command) : sizeof(struct color_command) + mask_words * 4);
    uint32_t *packet = kernel_malloc(bytes);
    if (!packet)
        return -ENOMEM;
    if (alpha) {
        const struct alpha_command header = {
            SVGA_CMD_DEFINE_ALPHA_CURSOR, {0, hot_x, hot_y, width, height},
        };
        __builtin_memcpy(packet, &header, sizeof(header));
        copy_cursor_pixels(packet + sizeof(header) / 4, pixels, pitch, width, height);
    } else {
        const struct color_command header = {
            SVGA_CMD_DEFINE_CURSOR, {0, hot_x, hot_y, width, height, 1, 32},
        };
        __builtin_memcpy(packet, &header, sizeof(header));
        uint32_t *mask = (uint32_t *)((uint8_t *)packet + sizeof(header));
        uint32_t *image = mask + mask_words;
        __builtin_memset(mask, 0, mask_words * sizeof(*mask));
        /* Classic cursors have one transparency bit per pixel. Threshold alpha
         * and clear the XOR color under transparent pixels. */
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t pixel = pixels[(size_t)y * pitch + x];
                bool transparent = (pixel >> 24) < 128;
                image[y * width + x] = transparent ? 0 : pixel & 0x00ffffff;
                if (transparent) {
                    uint8_t *row = (uint8_t *)(mask + y * stride);
                    row[x / 8] |= 0x80u >> (x % 8);
                }
            }
        }
    }
    uint32_t fence;
    int ret = svga_submit_fenced(packet, bytes, &fence);
    kernel_free(packet);
    return ret ? ret : svga_submit_wait(fence);
}

static int upload_cursor(const uint32_t *pixels, uint32_t pitch, uint32_t width, uint32_t height,
                         uint32_t hot_x, uint32_t hot_y)
{
    return cursor_uses_mob() ? define_mob_cursor(pixels, pitch, width, height, hot_x, hot_y) :
                              define_fifo_cursor(pixels, pitch, width, height, hot_x, hot_y);
}

static int32_t cursor_coordinate(int64_t value)
{
    if (value < INT32_MIN)
        return INT32_MIN;
    return value > INT32_MAX ? INT32_MAX : (int32_t)value;
}

static void show_cursor(unsigned unit, const struct svga_scanout *scanout)
{
    const struct svga_cursor_position *p = &positions[unit];
    cursor_position(true, cursor_coordinate((int64_t)p->x + scanout->root_x + p->hot_x),
                          cursor_coordinate((int64_t)p->y + scanout->root_y + p->hot_y));
}

int svga_cursor_set_property(struct drm_file *file, unsigned unit, struct svga_cursor_plane *cursor,
                             uint32_t property, uint64_t value)
{
    switch (property) {
    case SVGA_PROP_PLANE_CRTC:
        if (value && value != SVGA_CRTC_ID + unit)
            return -EINVAL;
        cursor->routed = value != 0;
        return 0;
    case SVGA_PROP_FB_ID:
        if (value > UINT32_MAX)
            return -EINVAL;
        cursor->fb = value ? svga_fb_lookup(file, value) : NULL;
        cursor->refresh = true;
        return value && !cursor->fb ? -ENOENT : 0;
    case SVGA_PROP_SRC_X:
    case SVGA_PROP_SRC_Y:
    case SVGA_PROP_SRC_W:
    case SVGA_PROP_SRC_H:
        if (value > UINT32_MAX || (value & 0xffff))
            return -EINVAL;
        if (property == SVGA_PROP_SRC_X)
            cursor->src_x = value;
        else if (property == SVGA_PROP_SRC_Y)
            cursor->src_y = value;
        else if (property == SVGA_PROP_SRC_W)
            cursor->src_w = value;
        else
            cursor->src_h = value;
        return 0;
    case SVGA_PROP_CRTC_X:
    case SVGA_PROP_CRTC_Y:
    case SVGA_PROP_HOTSPOT_X:
    case SVGA_PROP_HOTSPOT_Y:
        if ((int64_t)value < INT32_MIN || (int64_t)value > INT32_MAX)
            return -EINVAL;
        if (property == SVGA_PROP_CRTC_X)
            cursor->x = (int32_t)value;
        else if (property == SVGA_PROP_CRTC_Y)
            cursor->y = (int32_t)value;
        else if (property == SVGA_PROP_HOTSPOT_X)
            cursor->hot_x = (int32_t)value;
        else
            cursor->hot_y = (int32_t)value;
        return 0;
    case SVGA_PROP_CRTC_W:
    case SVGA_PROP_CRTC_H:
        if (value > INT32_MAX)
            return -EINVAL;
        if (property == SVGA_PROP_CRTC_W)
            cursor->width = value;
        else
            cursor->height = value;
        return 0;
    default:
        return -EINVAL;
    }
}

int svga_cursor_plane_check(unsigned unit, const struct svga_scanout *state)
{
    const struct svga_cursor_plane *c = &state->cursor;
    if (c->routed != (c->fb != NULL))
        return -EINVAL;
    if (!c->fb)
        return 0;
    uint32_t max = svga_cursor_limit();
    int64_t hot_x = (int64_t)c->hot_x + positions[unit].bypass_x;
    int64_t hot_y = (int64_t)c->hot_y + positions[unit].bypass_y;
    if (!state->mode_blob || c->fb->format != SVGA_ARGB8888 || !c->fb->bo ||
        !c->width || !c->height || c->width > max || c->height > max ||
        c->src_w != c->width << 16 || c->src_h != c->height << 16 ||
        (c->src_x >> 16) + c->width > c->fb->width ||
        (c->src_y >> 16) + c->height > c->fb->height ||
        hot_x < 0 || hot_y < 0 || hot_x >= c->width || hot_y >= c->height)
        return -EINVAL;
    if (cursor_uses_mob()) {
        uint32_t bytes = sizeof(SVGAGBCursorHeader) + c->width * c->height * sizeof(uint32_t);
        uint32_t limit = svga_read(SVGA_REG_CURSOR_MAX_BYTE_SIZE);
        if ((limit && bytes > limit) || bytes > svga_dma_max_mob_size())
            return -EINVAL;
    }
    return 0;
}

static bool cursor_visible(const struct svga_scanout *state)
{
    return svga_scanout_visible(state) && state->cursor.fb;
}

/* Each CRTC has plane state, while SVGA II has one host pointer. Prefer the
 * cursor updated by the commit, then retain the currently displayed cursor. */
int svga_cursor_commit(const struct svga_scanout *state, unsigned changed)
{
    int unit = -1;
    for (unsigned i = 0; i < svga_num_outputs; ++i)
        if (cursor_visible(&state[i]) && ((changed & (1u << i)) || !cursor_visible(&svga_scanouts[i])))
            unit = (int)i;
    if (unit < 0 && atomic_cursor) {
        if (cursor_visible(&state[visible_unit])) {
            unit = visible_unit;
        } else {
            for (unsigned i = 0; i < svga_num_outputs; ++i)
                if (cursor_visible(&state[i])) {
                    unit = (int)i;
                    break;
                }
            if (unit < 0)
                svga_cursor_hide(visible_unit);
        }
    }
    if (unit < 0)
        return 0;

    const struct svga_cursor_plane *c = &state[unit].cursor;
    const struct svga_cursor_plane *previous = &svga_scanouts[unit].cursor;
    struct svga_cursor_position p = positions[unit];
    int32_t hot_x = (int64_t)c->hot_x + p.bypass_x;
    int32_t hot_y = (int64_t)c->hot_y + p.bypass_y;
    bool upload = !atomic_cursor || unit != visible_unit || c->refresh || c->fb != previous->fb ||
        c->src_x != previous->src_x || c->src_y != previous->src_y ||
        c->width != previous->width || c->height != previous->height ||
        hot_x != p.hot_x || hot_y != p.hot_y;
    if (upload) {
        struct drm_bo *bo = c->fb->bo;
        int ret = svga_buffer_wait(bo);
        if (!ret)
            ret = svga_surface_cpu_access(bo);
        if (ret)
            return ret;
        size_t offset = c->fb->offset + (size_t)(c->src_y >> 16) * c->fb->pitch + (c->src_x >> 16) * 4;
        const uint32_t *pixels = (const uint32_t *)(c->fb->pixels + offset);
        ret = upload_cursor(pixels, c->fb->pitch / 4, c->width, c->height, hot_x, hot_y);
        if (ret)
            return ret;
    }
    if (!upload && c->x == p.x && c->y == p.y && state[unit].root_x == svga_scanouts[unit].root_x &&
        state[unit].root_y == svga_scanouts[unit].root_y)
        return 0;
    p.hot_x = hot_x;
    p.hot_y = hot_y;
    p.x = c->x;
    p.y = c->y;
    positions[unit] = p;
    visible_unit = unit;
    atomic_cursor = true;
    show_cursor(unit, &state[unit]);
    return 0;
}

int svga_cursor_legacy(struct drm_file *file, const struct drm_mode_cursor2 *arg)
{
    int unit = svga_output_index(arg->crtc_id, SVGA_CRTC_ID);
    if (unit < 0)
        return -ENOENT;
    if (!arg->flags || (arg->flags & ~DRM_MODE_CURSOR_FLAGS))
        return -EINVAL;
    if (svga.fifo.fault)
        return -EIO;
    uint32_t max = svga_cursor_limit();
    if (!max)
        return -EOPNOTSUPP;
    struct svga_scanout *scanout = &svga_scanouts[unit];
    if (scanout->owner != file)
        return -EINVAL;
    struct svga_cursor_position position = positions[unit];
    if (arg->flags & DRM_MODE_CURSOR_MOVE) {
        position.x = arg->x;
        position.y = arg->y;
    }
    if (arg->flags & DRM_MODE_CURSOR_BO) {
        if (!arg->handle) {
            svga_cursor_disable(unit);
            scanout->cursor = (struct svga_cursor_plane){0};
            position.hot_x = position.hot_y = 0;
            positions[unit] = position;
            return 0;
        }
        int64_t hot_x = (int64_t)arg->hot_x + position.bypass_x;
        int64_t hot_y = (int64_t)arg->hot_y + position.bypass_y;
        if (!arg->width || !arg->height || arg->width > max || arg->height > max ||
            hot_x < 0 || hot_y < 0 || hot_x >= arg->width || hot_y >= arg->height)
            return -EINVAL;
        struct drm_bo *bo = drm_bo_lookup_handle_get(file, arg->handle);
        if (!bo)
            return -ENOENT;
        uint32_t bytes = arg->width * arg->height * 4;
        const uint32_t *pixels = mm_phys_to_virt(drm_bo_page_physical(bo, 0));
        int ret = !pixels || bytes > drm_bo_size(bo) ? -EINVAL : svga_buffer_wait(bo);
        if (!ret)
            ret = svga_surface_cpu_access(bo);
        if (!ret)
            ret = upload_cursor(pixels, arg->width, arg->width, arg->height, hot_x, hot_y);
        drm_bo_put(bo);
        if (ret)
            return ret;
        scanout->cursor = (struct svga_cursor_plane){0};
        atomic_cursor = false;
        visible_unit = unit;
        position.hot_x = hot_x;
        position.hot_y = hot_y;
    }
    positions[unit] = position;
    if (atomic_cursor && visible_unit == unit) {
        scanout->cursor.x = position.x;
        scanout->cursor.y = position.y;
    }
    if (visible_unit == unit) {
        show_cursor(unit, scanout);
    }
    return 0;
}

int svga_cursor_bypass(const struct drm_vmw_cursor_bypass_arg *arg)
{
    if (arg->flags & ~DRM_VMW_CURSOR_BYPASS_FLAGS)
        return -EINVAL;
    int unit = svga_output_index(arg->crtc_id, SVGA_CRTC_ID);
    if (!(arg->flags & DRM_VMW_CURSOR_BYPASS_ALL) && unit < 0)
        return -ENOENT;
    for (unsigned i = 0; i < svga_num_outputs; ++i) {
        if (!(arg->flags & DRM_VMW_CURSOR_BYPASS_ALL) && i != (unsigned)unit)
            continue;
        const struct svga_cursor_plane *c = &svga_scanouts[i].cursor;
        if (c->fb) {
            int64_t x = (int64_t)c->hot_x + arg->xhot, y = (int64_t)c->hot_y + arg->yhot;
            if (x < 0 || y < 0 || x >= c->width || y >= c->height)
                return -EINVAL;
        }
    }
    for (unsigned i = 0; i < svga_num_outputs; ++i) {
        if ((arg->flags & DRM_VMW_CURSOR_BYPASS_ALL) || i == (unsigned)unit) {
            positions[i].bypass_x = arg->xhot;
            positions[i].bypass_y = arg->yhot;
        }
    }
    return 0;
}
