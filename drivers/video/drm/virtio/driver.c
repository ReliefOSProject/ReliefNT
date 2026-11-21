/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"

#define VG_IOCTL_ENTRY(command_) {command_, DRM_PERMISSION_RENDER_ALLOW}
static const struct drm_driver_ioctl vg_ioctls[] = {
    VG_IOCTL_ENTRY(DRM_IOCTL_VIRTGPU_MAP),
    VG_IOCTL_ENTRY(DRM_IOCTL_VIRTGPU_EXECBUFFER),
    VG_IOCTL_ENTRY(DRM_IOCTL_VIRTGPU_GETPARAM),
    VG_IOCTL_ENTRY(DRM_IOCTL_VIRTGPU_RESOURCE_CREATE),
    VG_IOCTL_ENTRY(DRM_IOCTL_VIRTGPU_RESOURCE_INFO),
    VG_IOCTL_ENTRY(DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST),
    VG_IOCTL_ENTRY(DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST),
    VG_IOCTL_ENTRY(DRM_IOCTL_VIRTGPU_WAIT),
    VG_IOCTL_ENTRY(DRM_IOCTL_VIRTGPU_GET_CAPS),
    VG_IOCTL_ENTRY(DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB),
    VG_IOCTL_ENTRY(DRM_IOCTL_VIRTGPU_CONTEXT_INIT),
};

static bool render_ready(void)
{
    return vg.ready && vg.virgl && !vg.fault;
}

static bool events_pending(const struct drm_file *file)
{
    return file->virtio_events != NULL;
}

static int get_cap(struct drm_get_cap *cap)
{
    switch (cap->capability) {
    case DRM_CAP_CURSOR_WIDTH:
    case DRM_CAP_CURSOR_HEIGHT:
        cap->value = 64;
        return 0;
    case DRM_CAP_ADDFB2_MODIFIERS:
    case DRM_CAP_ASYNC_PAGE_FLIP:
        cap->value = 0;
        return 0;
    case DRM_CAP_CRTC_IN_VBLANK_EVENT:
        cap->value = 1;
        return 0;
    default:
        return -EOPNOTSUPP;
    }
}

static int dumb_init(struct drm_bo *bo, uint32_t width, uint32_t height,
                     uint32_t bpp, uint32_t pitch)
{
    if (bpp != 32 && bpp != 24)
        return -EINVAL;
    struct drm_virtgpu_resource_create parameters = {
        .width = width, .height = height, .depth = 1, .array_size = 1,
        .format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM, .target = 2, .stride = pitch,
    };
    return vg_resource_create(bo, &parameters, false);
}

const struct drm_driver vg_driver = {
    .name = "virtio_gpu", .description = "virtio GPU", .minor = 1,
    .start = vg_device_start, .stop = vg_device_stop, .render_ready = render_ready,
    .ioctls = vg_ioctls, .ioctl_count = sizeof(vg_ioctls) / sizeof(vg_ioctls[0]),
    .ioctl = vg_ioctl, .dumb_init = dumb_init, .get_cap = get_cap,
    .universal_planes = true, .file_release = vg_context_close,
    .events_read = vg_events_read, .events_pending = events_pending,
    .kms_ioctl = vg_kms_ioctl, .kms_file_release = vg_kms_file_release,
    .kms_remove_fb = vg_kms_remove_fb, .kms_active = vg_kms_active,
    .kms_vt_release = vg_kms_vt_release, .connector_info = vg_connector_info,
    .retire = vg_process_retirements, .panic_prepare = vg_panic_prepare,
    .panic_damage = vg_panic_damage, .panic_present = vg_panic_present,
};
