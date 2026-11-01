#include "../internal.h"
#include <reliefnt/memory_stats.h>
#include <reliefnt/swap.h>

/** @brief Prepare one bounded statistics output before scheduler queries.
 * @param task Current task. @param address User output. @param size Bytes.
 * @return Zero, EFAULT, or the private prerequisite-contention marker.
 */
static int resource_prepare_output(struct task *task, uint64_t address, uint64_t size)
{
    if (!task) return 0;
    int ret = swap_prepare_user_range(task, address, size, true);
    if (ret == SWAP_STORAGE_CONTENDED) return KERNEL_SYSCALL_SWAP_CONTENDED;
    return ret < 0 ? -RELIEFOS_EFAULT : 0;
}

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
    if (task && usage_ptr) {
        int prep=resource_prepare_output(task,usage_ptr,sizeof(usage));if(prep)return prep;
    }
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
    if (info_ptr) {
        int prep=resource_prepare_output(sched_current_task(),info_ptr,sizeof(info));if(prep)return prep;
    }
    if (!info_ptr || !user_range_writable(info_ptr, sizeof(info))) return -RELIEFOS_EFAULT;
    info.uptime = (int64_t)(time_ticks() / RELIEFNT_TICK_HZ);
    sched_load_averages(info.loads);
    struct memory_snapshot memory; memory_snapshot_get(&memory);
    info.totalram = memory.total_kib;
    info.freeram = memory.free_kib;
    info.totalswap = memory.swap_total_kib;
    info.freeswap = memory.swap_free_kib;
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
    if (tms_ptr) {
        int prep=resource_prepare_output(task,tms_ptr,sizeof(struct linux_tms));if(prep)return prep;
    }
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
