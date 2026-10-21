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
int64_t syscall_legacy_system(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    if (number == LINUX_SYS_GETUID || number == LINUX_SYS_GETGID ||
        number == LINUX_SYS_GETEUID || number == LINUX_SYS_GETEGID ||
        number == LINUX_SYS_SETUID || number == LINUX_SYS_SETGID ||
        number == LINUX_SYS_SETREUID || number == LINUX_SYS_SETREGID ||
        number == LINUX_SYS_SETRESUID || number == LINUX_SYS_GETRESUID ||
        number == LINUX_SYS_SETRESGID || number == LINUX_SYS_GETRESGID ||
        number == LINUX_SYS_SETFSUID || number == LINUX_SYS_SETFSGID ||
        number == LINUX_SYS_CAPGET || number == LINUX_SYS_CAPSET ||
        number == LINUX_SYS_SCHED_GET_PRIORITY_MAX ||
        number == LINUX_SYS_SCHED_GET_PRIORITY_MIN ||
        number == LINUX_SYS_SCHED_GETPARAM ||
        number == LINUX_SYS_SCHED_GETSCHEDULER ||
        number == LINUX_SYS_SCHED_SETPARAM ||
        number == LINUX_SYS_SCHED_SETSCHEDULER ||
        number == LINUX_SYS_SCHED_RR_GET_INTERVAL ||
        number == LINUX_SYS_SCHED_SETATTR || number == LINUX_SYS_SCHED_GETATTR ||
        number == LINUX_SYS_PERSONALITY ||
        number == LINUX_SYS_UNAME || number == LINUX_SYS_SETHOSTNAME ||
        number == LINUX_SYS_SETDOMAINNAME || number == LINUX_SYS_MEMBARRIER ||
        number == LINUX_SYS_RSEQ ||
        number == LINUX_SYS_GETTIMEOFDAY ||
        number == LINUX_SYS_CLOCK_SETTIME ||
        number == LINUX_SYS_SETTIMEOFDAY || number == LINUX_SYS_CLOCK_GETTIME ||
        number == LINUX_SYS_SCHED_SETAFFINITY ||
        number == LINUX_SYS_SCHED_GETAFFINITY || number == LINUX_SYS_REBOOT) {
        return syscall_process_control(number, a0, a1, a2, a3);
    }

    if (number == __NR_adjtimex || number == __NR_clock_adjtime)
        return syscall_adjtimex(number == __NR_adjtimex ? 0 : (int32_t)a0,
                               number == __NR_adjtimex ? a0 : a1);

    if (number == LINUX_SYS_GETRUSAGE) {
        return syscall_getrusage(a0, a1);
    }

    if (number == LINUX_SYS_SYSINFO) {
        return syscall_sysinfo(a0);
    }

    if (number == LINUX_SYS_TIMES) {
        return syscall_times(a0);
    }

    if (number == LINUX_SYS_TIME) {
        struct linux_timespec now;
        int ret;
        ret = time_clock_get(LINUX_CLOCK_REALTIME, &now);
        if (ret < 0) return ret;
        if (a0) {
            if (!user_range_writable(a0, sizeof(int64_t))) return -RELIEFOS_EFAULT;
            *(int64_t *)(uintptr_t)a0 = now.tv_sec;
        }
        return now.tv_sec;
    }

    if (number == LINUX_SYS_GETCPU) {
        uint32_t cpu = smp_current_cpu();
        uint32_t node = 0;
        if (a0 && !user_range_writable(a0, sizeof(uint32_t))) return -RELIEFOS_EFAULT;
        if (a1 && !user_range_writable(a1, sizeof(uint32_t))) return -RELIEFOS_EFAULT;
        if (a0) *(uint32_t *)(uintptr_t)a0 = cpu;
        if (a1) *(uint32_t *)(uintptr_t)a1 = node;
        return 0;
    }

    if (number == LINUX_SYS_NANOSLEEP) {
        return syscall_nanosleep(1, 0, a0, a1);
    }

    if (number == LINUX_SYS_ALARM) {
        struct task *task = sched_current_task();
        return task ? (int64_t)sched_alarm_task(task, (uint32_t)a0) : -LINUX_ESRCH;
    }

    if (number == LINUX_SYS_GETITIMER || number == LINUX_SYS_SETITIMER) {
        return syscall_itimer(number == LINUX_SYS_SETITIMER, (int32_t)a0, a1, a2);
    }

    if (number == LINUX_SYS_TIMER_CREATE)
        return syscall_timer_create(a0, a1, a2);

    if (number == LINUX_SYS_TIMER_SETTIME)
        return syscall_timer_settime(a0, a1, a2, a3);

    if (number == LINUX_SYS_TIMER_GETTIME)
        return syscall_timer_gettime(a0, a1);

    if (number == LINUX_SYS_TIMER_GETOVERRUN)
        return syscall_timer_getoverrun(a0);

    if (number == LINUX_SYS_TIMER_DELETE)
        return syscall_timer_delete(a0);

    if (number == LINUX_SYS_RT_SIGTIMEDWAIT)
        return syscall_rt_sigtimedwait(a0, a1, a2, a3);

    if (number == LINUX_SYS_CLOCK_NANOSLEEP) {
        return syscall_nanosleep((int32_t)a0, (uint32_t)a1, a2, a3);
    }

    if (number == LINUX_SYS_GETRANDOM) {
        uint32_t flags = (uint32_t)a2;
        if ((flags & ~7u) || (flags & 6u) == 6u) return -RELIEFOS_EINVAL;
        uint64_t length = a1 > 0x7ffff000ULL ? 0x7ffff000ULL : a1;
        if (length && !user_range_writable(a0, length)) return -RELIEFOS_EFAULT;
        int ret = kernel_random_fill((void *)(uintptr_t)a0, (size_t)length);
        return ret < 0 ? ret : (int64_t)length;
    }

    return -RELIEFOS_ENOSYS;
}
