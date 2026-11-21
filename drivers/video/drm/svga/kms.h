/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SVGA_KMS_H
#define RELIEFNT_SVGA_KMS_H

#include "internal.h"
#include <drm/drm_mode.h>
#include <drm/vmwgfx_drm.h>
#include <reliefnt/framebuffer.h>

struct svga_blob;
struct svga_vblank_event;
struct svga_region;
struct svga_surface;

#define SVGA_CRTC_ID 0x100u
#define SVGA_ENCODER_ID 0x200u
#define SVGA_CONNECTOR_ID 0x300u
#define SVGA_PLANE_ID 0x400u
#define SVGA_CURSOR_PLANE_ID 0x500u
#define SVGA_PLANE_TYPE_ID 1u
#define SVGA_SUGGESTED_X_ID 2u
#define SVGA_SUGGESTED_Y_ID 3u
#define SVGA_HOTPLUG_MODE_ID 4u
#define SVGA_FB_ID_BASE 0x10000u
#define SVGA_BLOB_ID_BASE 0x80000000u
#define SVGA_MAX_FBS 128u
#define SVGA_MAX_EVENTS 32u
#define SVGA_MAX_OUTPUTS 8u
#define SVGA_XRGB8888 0x34325258u
#define SVGA_ARGB8888 0x34325241u

struct svga_fb {
    struct svga_fb *next;
    struct drm_bo *bo;
    struct svga_surface *surface;
    struct svga_region *gmr, *mob;
    uint8_t *pixels;
    uint32_t id, handle, width, height, pitch, offset, format;
};

struct svga_blit {
    uint32_t sx, sy, dx, dy, width, height;
};

struct svga_cursor_plane {
    struct svga_fb *fb;
    uint32_t src_x, src_y, src_w, src_h;
    int32_t x, y, hot_x, hot_y;
    uint32_t width, height;
    bool routed, refresh;
};

/* Each open-file description owns its framebuffer list and completion queue.
 * Each framebuffer owns one BO ref, independent of the originating handle. */
struct svga_file {
    struct svga_fb *fbs;
    uint32_t fb_count;
    union {
        struct drm_event base;
        struct drm_event_vblank vblank;
        struct drm_event_crtc_sequence sequence;
        struct drm_vmw_event_fence fence;
    } events[SVGA_MAX_EVENTS];
    unsigned head, count, reserved;
};

struct svga_scanout {
    struct drm_mode_modeinfo mode;
    struct svga_fb *fb;
    struct drm_file *owner;
    struct svga_blob *mode_blob, *damage_blob;
    struct svga_cursor_plane cursor;
    uint32_t x, y, root_x, root_y;
    uint32_t src_w, src_h, dst_w, dst_h;
    int32_t dst_x, dst_y;
    bool active, connector_routed, plane_routed;
};

static inline bool svga_scanout_visible(const struct svga_scanout *scanout)
{
    return scanout->active && scanout->fb;
}

enum svga_atomic_prop {
    SVGA_PROP_ACTIVE = 0x10, SVGA_PROP_MODE_ID, SVGA_PROP_CONNECTOR_CRTC,
    SVGA_PROP_PLANE_CRTC, SVGA_PROP_FB_ID, SVGA_PROP_SRC_X, SVGA_PROP_SRC_Y,
    SVGA_PROP_SRC_W, SVGA_PROP_SRC_H, SVGA_PROP_CRTC_X, SVGA_PROP_CRTC_Y,
    SVGA_PROP_CRTC_W, SVGA_PROP_CRTC_H, SVGA_PROP_DAMAGE_CLIPS,
    SVGA_PROP_HOTSPOT_X, SVGA_PROP_HOTSPOT_Y, SVGA_PROP_END,
};

/* DRM execution ownership serializes KMS, including region and surface lifetime. */
extern struct svga_scanout svga_scanouts[SVGA_MAX_OUTPUTS];
extern unsigned svga_num_outputs;
extern struct drm_vmw_rect svga_layout[SVGA_MAX_OUTPUTS];

static inline int svga_output_index(uint32_t id, uint32_t base)
{
    return id - base < svga_num_outputs ? (int)(id - base) : -1;
}

struct svga_fb *svga_fb_lookup(const struct drm_file *file, uint32_t id);
int svga_kms_check(const struct svga_scanout *state);
int svga_kms_commit(const struct svga_scanout *state, unsigned scanouts, unsigned cursors);
int svga_atomic_init(void);
void svga_atomic_fini(void);
void svga_atomic_drain(void);
void svga_atomic_cancel(struct drm_file *file);
int svga_queue_commit(struct drm_file *file, const struct svga_scanout *state,
                      unsigned scanouts, unsigned cursors,
                      struct svga_vblank_event *const *events);
int svga_vblank_init(void);
void svga_vblank_fini(void);
void svga_vblank_set(unsigned unit, const struct svga_scanout *scanout);
void svga_vblank_release(struct drm_file *file);
int svga_vblank_reserve(struct drm_file *file, unsigned unit, uint32_t type,
                        uint64_t user_data, struct svga_vblank_event **out);
void svga_vblank_arm_flip(struct svga_vblank_event *event, bool immediate);
void svga_vblank_cancel(struct svga_vblank_event *event);
int svga_vblank_ioctl(struct task *task, struct drm_file *file, uint32_t command, void *data);

/* Blobs are immutable. The creating file, queued commits, and published
 * scanout states hold independent references. */
int svga_blob_alloc(struct drm_file *owner, uint32_t length, struct svga_blob **out);
void svga_blob_get(struct svga_blob *blob);
void svga_blob_put(struct svga_blob *blob);
void svga_blobs_release(struct drm_file *file);
uint32_t svga_blob_id(const struct svga_blob *blob);
void *svga_blob_data(struct svga_blob *blob);
struct svga_blob *svga_blob_lookup(uint32_t id, uint32_t length);
struct svga_blob *svga_damage_lookup(uint32_t id);
const struct drm_mode_rect *svga_damage_rects(const struct svga_blob *blob, unsigned *count);
int svga_blob_destroy(struct drm_file *file, uint32_t id);
int svga_property_blob_ioctl(struct task *task, struct drm_file *file, uint32_t command, void *data);
int svga_atomic_property(struct task *task, struct drm_mode_get_property *arg);
unsigned svga_atomic_properties(uint32_t type, unsigned unit, bool cursor,
                                uint32_t *ids, uint64_t *values);
int svga_atomic_ioctl(struct task *task, struct drm_file *file, const struct drm_mode_atomic *arg);

void svga_kms_init(void);
bool svga_output_connected(unsigned unit);
int svga_layout_check(const struct drm_vmw_rect *rects, unsigned count);
int svga_update_layout(struct task *task, struct drm_file *file,
                       const struct drm_vmw_update_layout_arg *arg);
uint32_t svga_cursor_limit(void);
bool svga_cursor_plane_available(const struct drm_file *file);
int svga_cursor_set_property(struct drm_file *file, unsigned unit, struct svga_cursor_plane *cursor,
                             uint32_t property, uint64_t value);
int svga_cursor_plane_check(unsigned unit, const struct svga_scanout *state);
int svga_cursor_commit(const struct svga_scanout *state, unsigned changed);
int svga_cursor_legacy(struct drm_file *file, const struct drm_mode_cursor2 *arg);
int svga_cursor_bypass(const struct drm_vmw_cursor_bypass_arg *arg);
void svga_cursor_hide(unsigned unit);
void svga_cursor_disable(unsigned unit);
bool svga_overlay_supported(void);
unsigned svga_overlay_free(void);
int svga_overlay_claim(struct drm_file *file, struct drm_vmw_stream_arg *arg);
int svga_overlay_unref(struct drm_file *file, const struct drm_vmw_stream_arg *arg);
int svga_overlay_control(struct drm_file *file, const struct drm_vmw_control_stream_arg *arg);
int svga_overlay_stop(void);
void svga_overlay_release(struct drm_file *file);
/* Caller bounds the whole span by the kernel's BAR1 mapping. */
void svga_copy_to_vram(uint64_t offset, const uint8_t *pixels, size_t bytes);
void svga_copy_from_vram(uint8_t *pixels, uint64_t offset, size_t bytes);

/* Graphics execution owner only. The console leases screen zero when KMS
 * is idle. Stop the console before retiring DMA and the command queue. */
int svga_console_show(void);
void svga_console_update(void);
void svga_console_hide(void);
void svga_console_fini(void);
void svga_kms_restore_firmware(void);

enum svga_display_type { SVGA_DISPLAY_LEGACY, SVGA_DISPLAY_OBJECT, SVGA_DISPLAY_TARGET };
void svga_screen_init(uint32_t boot_vram_end);
enum svga_display_type svga_screen_type(void);
uint32_t svga_screen_max_primary(void);
int svga_screen_configure(unsigned unit, const struct svga_scanout *scanout);
void svga_screen_release(unsigned unit);
int svga_screen_present(unsigned unit, struct svga_fb *fb, const struct svga_scanout *scanout,
                        struct framebuffer_damage rect, uint32_t *fence);
int svga_screen_blit(unsigned unit, struct svga_surface *surface, const struct svga_blit *rects,
                     unsigned count, bool scanout, uint32_t *sequence);
int svga_screen_readback(unsigned unit, struct svga_fb *fb, const struct svga_blit *rects,
                         unsigned count, uint32_t *sequence);
int svga_kms_present_ioctl(struct task *task, struct drm_file *file, const struct drm_vmw_present_arg *arg);
int svga_kms_readback_ioctl(struct task *task, struct drm_file *file, const struct drm_vmw_present_readback_arg *arg);
bool svga_mode_fits(uint32_t width, uint32_t height);
int svga_kms_query(struct task *task, struct drm_file *file, uint32_t command, void *data);
int svga_events_read(struct task *task, struct drm_file *file, uint64_t address, uint32_t length);
bool svga_events_pending(const struct drm_file *file);
int svga_event_reserve(struct drm_file *file);
void svga_event_cancel(struct drm_file *file);
void svga_event_publish(struct drm_file *file, const struct drm_event *event);

#endif
