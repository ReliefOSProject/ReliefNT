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
int64_t syscall_legacy_filesystem(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    if (number == LINUX_SYS_UTIME || number == LINUX_SYS_UTIMES ||
        number == LINUX_SYS_FUTIMESAT || number == LINUX_SYS_UTIMENSAT)
        return syscall_timestamp_object(sched_current_task(),number,a0,a1,a2,a3);

    if (number == LINUX_SYS_OPENAT2) {
        return syscall_openat2(a0, a1, a2, a3);
    }

    if (number == LINUX_SYS_FADVISE64) {
        struct task *task = sched_current_task();
        if ((int64_t)a1 < 0 || (int64_t)a2 < 0) return -RELIEFOS_EINVAL;
        struct task_file *file = task_file_for_fd(task, (int)a0);
        if (!file) return -RELIEFOS_EBADF;
        if (a3 > 5) return -RELIEFOS_EINVAL;
        return 0;
    }

    if (number == LINUX_SYS_READAHEAD) {
        struct task_file *file = task_file_for_fd(sched_current_task(), (int)a0);
        if (!file) return -RELIEFOS_EBADF;
        if ((int64_t)a1 < 0) return -RELIEFOS_EINVAL;
        if (file->node.type != RELIEFOS_FS_TYPE_FILE &&
            !(file->flags & TASK_FILE_FLAG_DEV_BLOCK)) return -LINUX_ESPIPE;
        return 0;
    }

    if (number == LINUX_SYS_SYNC_FILE_RANGE) {
        struct task_file *file = task_file_for_fd(sched_current_task(), (int)a0);
        if (!file) return -RELIEFOS_EBADF;
        if ((int64_t)a1 < 0 || (int64_t)a2 < 0 || (a3 & ~7u)) {
            return -RELIEFOS_EINVAL;
        }
        if (file->node.type != RELIEFOS_FS_TYPE_FILE &&
            !(file->flags & TASK_FILE_FLAG_DEV_BLOCK)) {
            return -RELIEFOS_EINVAL;
        }
        /* Storage writes are synchronous before write(2) returns. Valid
         * Linux wait/write flags therefore complete here without a second
         * backend operation; the descriptor, range and flag contract still
         * applies at the syscall boundary. */
        return 0;
    }

    if (number == LINUX_SYS_GETGROUPS || number == LINUX_SYS_SETGROUPS) {
        struct task *task = sched_current_task();
        int32_t size = (int32_t)a0;
        if (!task) return -RELIEFOS_EPERM;
        if (number == LINUX_SYS_SETGROUPS) {
            if (!(task->cap_effective & (1ULL << CAP_SETGID))) return -RELIEFOS_EPERM;
            if (size < 0 || size > 65536) return -RELIEFOS_EINVAL;
            if (size && !user_range_ok(a1, (uint64_t)size * 4)) return -RELIEFOS_EFAULT;
            return task_groups_set(task, (const uint32_t *)(uintptr_t)a1, (uint32_t)size);
        }
        uint32_t count = task->groups ? task->groups->count : 0;
        if (size < 0) return -RELIEFOS_EINVAL;
        if (!size) return count;
        if ((uint32_t)size < count) return -RELIEFOS_EINVAL;
        if (count && !user_range_writable(a1, (uint64_t)count * 4)) return -RELIEFOS_EFAULT;
        for (uint32_t i = 0; i < count; ++i) ((uint32_t *)(uintptr_t)a1)[i] = task->groups->ids[i];
        return count;
    }








    if (number == LINUX_SYS_UMASK) {
        struct task *task = sched_current_task();
        uint32_t old;
        if (!task) return -RELIEFOS_EPERM;
        old = (*sched_task_umask(task)) & 0777u;
        (*sched_task_umask(task)) = (uint32_t)a0 & 0777u;
        return old;
    }

    if(number==LINUX_SYS_MOUNT || number==LINUX_SYS_UMOUNT2)
        return syscall_mount_control(vfs_system_namespace(),sched_current_task(),number,a0,a1,a2,a3,a4);

    return -RELIEFOS_ENOSYS;
}
