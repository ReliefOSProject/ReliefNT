/* SPDX-License-Identifier: Apache-2.0 */
#include "drm_internal.h"
#include <reliefnt/usercopy.h>

/* Linux mode queries return the actual count without copying an array when
 * the caller's capacity is short. Copyout uses the usual syscall retry path. */
int drm_kms_copy_array(struct task *task, uint64_t address, uint32_t capacity,
                       const void *data, uint32_t count, uint32_t size)
{
    if (capacity < count || !count)
        return 0;
    uint64_t bytes = (uint64_t)count * size;
#ifdef CONFIG_SWAP
    int ret = drm_user_prepare(task, address, bytes, true);
    if (ret)
        return ret;
#endif
    return user_copy_to_task(task, address, data, bytes) < 0 ? -EFAULT : 0;
}

bool drm_device_kms_scanout_active(void)
{
    struct drm_device *device = drm_device_get();
    return device && device->driver->kms_active && device->driver->kms_active();
}

int drm_kms_ioctl(struct task *task, struct drm_file *file, uint32_t command,
                  void *data, uint32_t *created_fb_out)
{
    const struct drm_driver *driver = file->device->driver;
    return driver->kms_ioctl ? driver->kms_ioctl(task, file, command, data, created_fb_out) :
        -LINUX_EOPNOTSUPP;
}

void drm_kms_remove_fb(struct drm_file *file, uint32_t fb_id)
{
    if (file && fb_id && file->device->driver->kms_remove_fb)
        file->device->driver->kms_remove_fb(file, fb_id);
}

void drm_kms_drop_master(struct drm_file *file)
{
    if (file && file->is_master && file->device->driver->kms_vt_release)
        file->device->driver->kms_vt_release();
}

void drm_device_kms_vt_release(void)
{
    struct drm_device *device = drm_device_get();
    if (device && device->driver->kms_vt_release)
        device->driver->kms_vt_release();
}

void drm_kms_file_release(struct drm_file *file)
{
    if (file && file->device->driver->kms_file_release)
        file->device->driver->kms_file_release(file);
}
