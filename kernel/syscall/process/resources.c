#include "../internal.h"

static void syscall_rusage_cpu_time(uint64_t ticks, struct linux_timeval *out);


static void syscall_rusage_cpu_time(uint64_t ticks, struct linux_timeval *out)
{
    out->tv_sec = (int64_t)(ticks / RELIEFNT_TICK_HZ);
    out->tv_usec = (int64_t)((ticks % RELIEFNT_TICK_HZ) * (1000000ULL / RELIEFNT_TICK_HZ));
}

int64_t syscall_getrusage(uint64_t who_arg, uint64_t usage_ptr)
{
    struct task *task = sched_current_task();
    struct linux_rusage usage = {0};
    int32_t who = (int32_t)who_arg;
    if (!task || !usage_ptr || !user_range_writable(usage_ptr, sizeof(usage))) {
        return -RELIEFOS_EFAULT;
    }
    if (who != 0 && who != -1 && who != -2) return -RELIEFOS_EINVAL;
    if (who == 0 || who == -2) {
        syscall_rusage_cpu_time(sched_task_cpu_ticks(task, false), &usage.ru_utime);
        usage.ru_maxrss = address_space_user_resident_kib(sched_task_as(task));
    }
    *(struct linux_rusage *)(uintptr_t)usage_ptr = usage;
    return 0;
}

int64_t syscall_sysinfo(uint64_t info_ptr)
{
    struct linux_sysinfo info = {0};
    uint32_t task_count = 0;
    if (!info_ptr || !user_range_writable(info_ptr, sizeof(info))) return -RELIEFOS_EFAULT;
    info.uptime = (int64_t)(time_ticks() / RELIEFNT_TICK_HZ);
    sched_load_averages(info.loads);
    info.totalram = mm_total_memory_kib();
    info.freeram = mm_free_memory_kib();
    info.mem_unit = 1024;
    sched_task_counts(&task_count, NULL, NULL, NULL);
    info.procs = task_count > UINT16_MAX ? UINT16_MAX : (uint16_t)task_count;
    *(struct linux_sysinfo *)(uintptr_t)info_ptr = info;
    return 0;
}

int64_t syscall_times(uint64_t tms_ptr)
{
    struct task *task = sched_current_task();
    if (!task) return -RELIEFOS_EFAULT;
    if (tms_ptr && !user_range_writable(tms_ptr, sizeof(struct linux_tms))) {
        return -RELIEFOS_EFAULT;
    }
    if (tms_ptr) {
        struct linux_tms value = {
            .tms_utime = (int64_t)sched_task_cpu_ticks(task, false),
            .tms_stime = 0,
            .tms_cutime = 0,
            .tms_cstime = 0,
        };
        *(struct linux_tms *)(uintptr_t)tms_ptr = value;
    }
    return (int64_t)time_ticks();
}
