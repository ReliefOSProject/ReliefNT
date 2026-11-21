/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_VIRTGPU_KMS_H
#define RELIEFNT_VIRTGPU_KMS_H
#include "internal.h"
#define VG_CRTC 0x100u
#define VG_ENCODER 0x200u
#define VG_CONNECTOR 0x300u
#define VG_PLANE 0x400u
#define VG_EDID_BLOB 0x500u
#define VG_PROP_EDID 1u
#define VG_PROP_DPMS 2u
#define VG_PROP_PLANE_TYPE 3u
#define VG_XRGB8888 0x34325258u
#define VG_ARGB8888 0x34325241u
struct vg_kms_fb {
    struct vg_kms_fb *next;
    struct drm_bo *bo;
    uint32_t id, width, height, pitch, format;
};
struct vg_output {
    struct vg_kms_fb *fb;
    struct drm_file *owner;
    struct drm_mode_modeinfo mode;
    struct drm_bo *cursor;
    int32_t cursor_x, cursor_y;
    uint32_t hot_x, hot_y, x, y, sequence;
    bool enabled;
};
struct vg_event { struct vg_event *next; struct drm_event_vblank payload; };
extern struct vg_output vg_outputs[VIRTIO_GPU_MAX_SCANOUTS];


/** @brief Dispatch KMS metadata queries without hardware state mutations.
 * @param task Caller. @param file OFD. @param command Canonical ioctl.
 * @param data Copied argument. @return Zero/errno/retry or -VG_IOCTL_UNOWNED if unowned.
 */
int vg_kms_query(struct task *task,struct drm_file *file,uint32_t command,void *data);
/** @brief Find a framebuffer in its authorizing file's namespace.
 * @param file Owner OFD. @param id FB identifier. @return Borrowed object or NULL.
 */
struct vg_kms_fb *vg_fb_find(struct drm_file *file,uint32_t id);
/** @brief Dispatch framebuffer creation, damage, removal and description.
 * @param task Caller. @param file Authorized OFD. @param command Selector.
 * @param data Copied argument. @param created_fb Rollback output.
 * @return Zero/errno/retry or -VG_IOCTL_UNOWNED when unowned.
 */
int vg_fb_ioctl(struct task *task,struct drm_file *file,uint32_t command,void *data,uint32_t *created_fb);
/** @brief Queue a completed virtual vblank event after its GPU fence.
 * @param file Live receiving OFD. @param event Preallocated owned event.
 * @param output Index. @param user_data Opaque client cookie.
 */
void vg_event_publish(struct drm_file *file,struct vg_event *event,unsigned output,uint64_t user_data);
/** @brief Copy whole completed events and consume only successfully copied records.
 * @param task Caller. @param file OFD. @param buffer User destination.
 * @param length Capacity. @return Bytes, zero if empty, or errno/retry.
 */
int vg_events_read(struct task *task,struct drm_file *file,uint64_t buffer,uint32_t length);
/** @brief Drop unread events during final OFD teardown. @param file Closing OFD. */
void vg_events_close(struct drm_file *file);
/** @brief Disable one CRTC and release its retained scanout and cursor references.
 * @param index Hardware output. @return Zero or protocol errno.
 */
int vg_output_disable(unsigned index);
/** @brief Apply legacy CURSOR/CURSOR2 on the independent fast queue.
 * @param file Authorized owner. @param command Canonical selector.
 * @param data Copied cursor argument. @return Zero or validation/protocol errno.
 */
int vg_cursor_ioctl(struct drm_file *file,uint32_t command,void *data);
/** @brief Hide the host cursor before releasing its retained image backing.
 * @param output Hardware scanout. @return Zero or command/reset errno.
 */
int vg_cursor_disable(unsigned output);
#endif
