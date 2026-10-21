#include "../internal.h"





/**
 * @brief Dispatch an already-normalised syscall within this ABI category.
 * @param number Linux or ReliefOS syscall number selected by the common dispatcher.
 * @param a0 First native ABI argument; pointers remain subject to the original checks.
 * @param a1 Second native ABI argument.
 * @param a2 Third native ABI argument.
 * @param a3 Fourth native ABI argument.
 * @param a4 Fifth native ABI argument.
 * @param a5 Sixth native ABI argument.
 * @return Original result, negative errno, or internal blocking/retry status.
 * @note The caller retains the kernel execution transaction and descriptor pins.
 */
int64_t syscall_legacy_events(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    if (number == LINUX_SYS_EPOLL_CREATE)
        return syscall_epoll_create(a0, 0);

    if (number == LINUX_SYS_EPOLL_CREATE1)
        return syscall_epoll_create(1, (uint32_t)a0);

    if (number == LINUX_SYS_EPOLL_CTL)
        return syscall_epoll_ctl(a0, a1, a2, a3);

    if (number == LINUX_SYS_EPOLL_WAIT || number == LINUX_SYS_EPOLL_PWAIT)
        return syscall_epoll_wait(a0, a1, a2, (int64_t)a3);

    if (number == LINUX_SYS_EPOLL_PWAIT2)
        return syscall_epoll_pwait2(a0, a1, a2, a3, a4, a5);

    if (number == LINUX_SYS_EVENTFD || number == LINUX_SYS_EVENTFD2) {
        return syscall_eventfd_create(a0, number == LINUX_SYS_EVENTFD2 ? (uint32_t)a1 : 0);
    }

    if (number == LINUX_SYS_SIGNALFD || number == LINUX_SYS_SIGNALFD4)
        return syscall_signalfd((int32_t)a0, a1, a2, number == LINUX_SYS_SIGNALFD4 ? (uint32_t)a3 : 0);

    if (number == LINUX_SYS_TIMERFD_CREATE)
        return syscall_timerfd_create(a0, (uint32_t)a1);

    if (number == LINUX_SYS_TIMERFD_SETTIME)
        return syscall_timerfd_settime(a0, (uint32_t)a1, a2, a3);

    if (number == LINUX_SYS_TIMERFD_GETTIME)
        return syscall_timerfd_gettime(a0, a1);

    if (number == LINUX_SYS_POLL) {
        return syscall_poll(a0, a1, (int64_t)a2);
    }

    if (number == LINUX_SYS_SELECT) {
        return syscall_select_common(a0, a1, a2, a3, a4, false);
    }

    if (number == LINUX_SYS_PSELECT6) {
        struct task *task = sched_current_task();
        uint64_t mask_ptr = 0;
        uint64_t mask_len = 0;
        if (a5) {
            if (!user_range_ok(a5, sizeof(mask_ptr) + sizeof(mask_len))) return -RELIEFOS_EFAULT;
            __builtin_memcpy(&mask_ptr, (const void *)(uintptr_t)a5, sizeof(mask_ptr));
            __builtin_memcpy(&mask_len, (const void *)(uintptr_t)(a5 + sizeof(mask_ptr)), sizeof(mask_len));
            if (mask_ptr) {
                if (mask_len != sizeof(uint64_t)) return -RELIEFOS_EINVAL;
                if (!user_range_ok(mask_ptr, sizeof(uint64_t))) return -RELIEFOS_EFAULT;
            }
        }
        if (!task) return -RELIEFOS_EINVAL;
        /* Keep the temporary mask installed across the scheduler's EAGAIN
         * replay. Signal delivery uses this saved mask in the rt_sigframe. */
        if (mask_ptr && !task->sigsuspend_active) {
            task->sigsuspend_saved_mask = task->blocked_signals;
            task->sigsuspend_active = 1;
            task->blocked_signals = *(const uint64_t *)(uintptr_t)mask_ptr &
                                    ~((1ULL << 8) | (1ULL << 18));
        }
        int64_t result = syscall_select_common(a0, a1, a2, a3, a4, true);
        if (result != -RELIEFOS_EAGAIN && result != KERNEL_SYSCALL_BLOCKED &&
            task->sigsuspend_active) {
            task->blocked_signals = task->sigsuspend_saved_mask;
            task->sigsuspend_active = 0;
        }
        return result;
    }

    if (number == LINUX_SYS_PPOLL) {
        return syscall_ppoll(a0, a1, a2, a3, a4);
    }

    return -RELIEFOS_ENOSYS;
}
