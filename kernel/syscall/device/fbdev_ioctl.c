#include "../internal.h"
#include <reliefnt/swap.h>



/**
 * @brief Copy, validate, and copy back one fbdev variable-screen request.
 * @param user_info Readable user address of a complete fb_var_screeninfo.
 * @return Zero on success, or a negative Linux errno from validation/copyout.
 */
int64_t syscall_fbdev_put_var(uint64_t user_info)
{
    struct fb_var_screeninfo info;
#ifdef CONFIG_SWAP
    struct task *task = sched_current_task();
    int prep = swap_prepare_user_range(task, user_info, sizeof(info), false);
    if (prep == SWAP_STORAGE_CONTENDED) return KERNEL_SYSCALL_SWAP_CONTENDED;
    if (prep < 0) return -RELIEFOS_EFAULT;
#endif
    if (!user_range_ok(user_info, sizeof(info))) return -RELIEFOS_EFAULT;
#ifdef CONFIG_SWAP
    /* Resolve a transient output prerequisite before the mode change. Other
     * output failures keep the original late-EFAULT behavior below. */
    prep = swap_prepare_user_range(task, user_info, sizeof(info), true);
    if (prep == SWAP_STORAGE_CONTENDED) return KERNEL_SYSCALL_SWAP_CONTENDED;
#endif
    __builtin_memcpy(&info, (const void *)(uintptr_t)user_info, sizeof(info));
    int ret = framebuffer_fbdev_put_var(&info);
    if (ret < 0) return ret;
    if (!user_range_writable(user_info, sizeof(info))) return -RELIEFOS_EFAULT;
    __builtin_memcpy((void *)(uintptr_t)user_info, &info, sizeof(info));
    return 0;
}
