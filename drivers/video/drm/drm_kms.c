#include "drm_internal.h"
#ifdef CONFIG_DRM_VIRTIO_GPU
#include "virtio/internal.h"
#endif

bool drm_device_kms_scanout_active(void)
{
#ifdef CONFIG_DRM_VIRTIO_GPU
    if (drm_is_virtio()) return vg_kms_active();
#endif
    return false;
}

int drm_kms_ioctl(struct task *task, struct drm_file *file, uint32_t command,
                  void *data, uint32_t *created_fb_out)
{
#ifdef CONFIG_DRM_VIRTIO_GPU
    if (drm_is_virtio())
        return vg_kms_ioctl(task, file, command, data, created_fb_out);
#else
    (void)task;
    (void)file;
    (void)command;
    (void)data;
    (void)created_fb_out;
#endif
    return -LINUX_EOPNOTSUPP;
}

void drm_kms_remove_fb(struct drm_file *file, uint32_t fb_id)
{
#ifdef CONFIG_DRM_VIRTIO_GPU
    if (drm_is_virtio() && file && fb_id) vg_kms_remove_fb(file, fb_id);
#else
    (void)file;
    (void)fb_id;
#endif
}

void drm_kms_drop_master(struct drm_file *file)
{
#ifdef CONFIG_DRM_VIRTIO_GPU
    if (drm_is_virtio() && file && file->is_master) vg_kms_vt_release();
#else
    (void)file;
#endif
}

void drm_device_kms_vt_release(void)
{
#ifdef CONFIG_DRM_VIRTIO_GPU
    if (drm_is_virtio()) vg_kms_vt_release();
#endif
}

void drm_kms_file_release(struct drm_file *file)
{
#ifdef CONFIG_DRM_VIRTIO_GPU
    if (drm_is_virtio() && file) vg_kms_file_release(file);
#else
    (void)file;
#endif
}
