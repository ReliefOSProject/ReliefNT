#include "../internal.h"



/**
 * @brief Implements the validated Linux openat2 argument block and delegates
 * the supported no-resolve path to the existing openat object allocator.
 * @param dirfd Directory descriptor used for relative resolution.
 * @param pathname User pathname pointer.
 * @param how_ptr User open_how pointer.
 * @param size Size of the user open_how block.
 * @return New descriptor or negative errno.
 */
int64_t syscall_openat2(uint64_t dirfd, uint64_t pathname,
                               uint64_t how_ptr, uint64_t size)
{
    struct task *opening_task = sched_current_task();
    if (opening_task && opening_task->fifo_open_file)
        return task_fifo_open(opening_task, NULL, 0, NULL);
    struct open_how how;
    const uint64_t supported_flags = LINUX_O_RDONLY | LINUX_O_WRONLY | LINUX_O_RDWR |
        LINUX_O_CREAT | LINUX_O_EXCL | LINUX_O_NOCTTY | LINUX_O_TRUNC |
        LINUX_O_APPEND | LINUX_O_NONBLOCK | LINUX_O_CLOEXEC | LINUX_O_DIRECTORY;
    const uint64_t valid_flags = supported_flags | LINUX_O_DSYNC | LINUX_O_SYNC |
        LINUX_O_ASYNC | LINUX_O_DIRECT | LINUX_O_LARGEFILE | LINUX_O_NOFOLLOW |
        LINUX_O_NOATIME | LINUX_O_PATH | LINUX_O_TMPFILE;
    const uint64_t valid_resolve = RESOLVE_NO_XDEV | RESOLVE_NO_MAGICLINKS |
        RESOLVE_NO_SYMLINKS | RESOLVE_BENEATH | RESOLVE_IN_ROOT | RESOLVE_CACHED;
    if (size < sizeof(how)) return -RELIEFOS_EINVAL;
    if (size > 4096u) return -RELIEFOS_E2BIG;
    if (!user_range_ok(how_ptr, size)) return -RELIEFOS_EFAULT;
    __builtin_memcpy(&how, (const void *)(uintptr_t)how_ptr, sizeof(how));
    for (uint64_t offset = sizeof(how); offset < size; ++offset) {
        if (((const uint8_t *)(uintptr_t)how_ptr)[offset] != 0) return -RELIEFOS_E2BIG;
    }
    /* Linux strips __FMODE_NONOTIFY before checking valid open flags. */
    how.flags &= ~0x04000000ULL;
    if (how.flags & ~valid_flags) return -RELIEFOS_EINVAL;
    if (how.resolve & ~valid_resolve) return -RELIEFOS_EINVAL;
    if ((how.resolve & (RESOLVE_BENEATH | RESOLVE_IN_ROOT)) ==
        (RESOLVE_BENEATH | RESOLVE_IN_ROOT)) return -RELIEFOS_EINVAL;
    bool tmpfile = (how.flags & (LINUX_O_TMPFILE & ~LINUX_O_DIRECTORY)) != 0;
    if (how.mode && !(how.flags & LINUX_O_CREAT) && !tmpfile) return -RELIEFOS_EINVAL;
    if (how.mode & ~07777ULL) return -RELIEFOS_EINVAL;
    if ((how.flags & (LINUX_O_CREAT | LINUX_O_DIRECTORY)) ==
        (LINUX_O_CREAT | LINUX_O_DIRECTORY)) return -RELIEFOS_EINVAL;
    if (tmpfile && (!(how.flags & LINUX_O_DIRECTORY) ||
                   !(how.flags & (LINUX_O_WRONLY | LINUX_O_RDWR)))) return -RELIEFOS_EINVAL;
    if ((how.flags & LINUX_O_PATH) && (how.flags &
        ~(uint64_t)(LINUX_O_PATH | LINUX_O_CLOEXEC | LINUX_O_DIRECTORY | LINUX_O_NOFOLLOW)))
        return -RELIEFOS_EINVAL;
    if (how.flags & ~supported_flags) return -RELIEFOS_EOPNOTSUPP;
    if (how.resolve) return -RELIEFOS_EOPNOTSUPP;
    return syscall_dispatch_regs_legacy(LINUX_SYS_OPENAT, dirfd, pathname,
                                         (uint32_t)how.flags, (uint32_t)how.mode,
                                         0, 0);
}

/**
 * @brief Set inode timestamps by pathname, empty-path handle, or Linux futimens fd.
 * @param task Caller whose identity controls timestamp permissions.
 * @param dirfd Directory fd or the target fd when path_ptr is NULL.
 * @param path_ptr User pathname, or NULL for an fd-only operation.
 * @param times Validated access/modification times, with UTIME_NOW/UTIME_OMIT.
 * @param flags Lookup flags; fd-only operations require zero.
 * @param times_supplied Caller explicitly supplied the times vector.
 * @return Zero or a negative lookup, descriptor, permission, or storage error.
 */
int64_t syscall_update_path_times(struct task *task, int32_t dirfd, uint64_t path_ptr,
                                         const struct linux_timespec times[2], uint32_t flags,
                                         bool times_supplied)
{
    struct storage_node node;
    char path[RELIEFOS_FS_PATH_LEN];
    if (flags & ~(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH))
        return -RELIEFOS_EINVAL;
    int ret;
    struct task_file *file = NULL;
    if (!path_ptr && dirfd != LINUX_AT_FDCWD) {
        /* Linux do_utimes_fd: futimens uses a NULL pathname, not AT_EMPTY_PATH. */
        if (flags) return -RELIEFOS_EINVAL;
        file = task_file_for_fd(task, dirfd);
        if (!file || (file->flags & TASK_FILE_FLAG_PATH)) return -RELIEFOS_EBADF;
        copy_text(path, sizeof(path), file->path);
    } else {
        ret = resolve_user_path_at_flags(task, dirfd, path_ptr, flags & LINUX_AT_EMPTY_PATH,
                                        path, &file, false,
                                        flags & LINUX_AT_SYMLINK_NOFOLLOW ? 0 : FS_LOOKUP_FOLLOW);
        if (ret < 0) return ret;
    }
    if (file) {
        node = file->node;
        ret = storage_inode_refresh(&node);
    } else ret = storage_lookup_path(path, &node);
    if (ret < 0) return ret;
    struct reliefos_permissions perms;
    ret = fs_permissions_get(path, &node, &perms);
    if (ret < 0) return ret;
    if (task && task->euid != 0 && task->euid != perms.uid) {
        bool explicit_time = false;
        if (times_supplied) {
            explicit_time = (times[0].tv_nsec != 1073741823 && times[0].tv_nsec != 1073741822) ||
                            (times[1].tv_nsec != 1073741823 && times[1].tv_nsec != 1073741822);
        }
        if (explicit_time) return -RELIEFOS_EPERM;
        ret = fs_permissions_check_node(task, path, &node, FS_ACCESS_WRITE, false);
        if (ret < 0) return ret;
    }
    struct reliefos_time_info now;
    if (time_wall_clock(&now) < 0) return -RELIEFOS_EIO;
    int64_t atime = times[0].tv_nsec == 1073741823 ? now.unix_seconds : times[0].tv_sec;
    int64_t mtime = times[1].tv_nsec == 1073741823 ? now.unix_seconds : times[1].tv_sec;
    bool set_atime = times[0].tv_nsec != 1073741822;
    bool set_mtime = times[1].tv_nsec != 1073741822;
    return storage_inode_utimensat(&node, atime, mtime, set_atime, set_mtime);
}
