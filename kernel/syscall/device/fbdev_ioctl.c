#include "../internal.h"



/**
 * @brief Copy, validate, and copy back one fbdev variable-screen request.
 * @param user_info Readable user address of a complete fb_var_screeninfo.
 * @return Zero on success, or a negative Linux errno from validation/copyout.
 */
int64_t syscall_fbdev_put_var(uint64_t user_info)
{
    struct fb_var_screeninfo info;
    if (!user_range_ok(user_info, sizeof(info))) return -RELIEFOS_EFAULT;
    __builtin_memcpy(&info, (const void *)(uintptr_t)user_info, sizeof(info));
    int ret = framebuffer_fbdev_put_var(&info);
    if (ret < 0) return ret;
    if (!user_range_writable(user_info, sizeof(info))) return -RELIEFOS_EFAULT;
    __builtin_memcpy((void *)(uintptr_t)user_info, &info, sizeof(info));
    return 0;
}
