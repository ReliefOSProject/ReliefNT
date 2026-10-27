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
int64_t syscall_legacy_descriptors(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    if (number == LINUX_SYS_PIPE) {
        return syscall_ipc_pipe(a0);
    }

    if (number == LINUX_SYS_PIPE2) {
        return syscall_ipc_pipe2(a0, a1);
    }

    if (number == LINUX_SYS_CLOSE) {
        struct task *task = sched_current_task();
        /* Linux close takes unsigned int, including on native x86-64. */
        uint32_t fd = (uint32_t)a0;
        if (!task) return -RELIEFOS_EBADF;
        if (fd < SCHED_TASK_STDIO_MAX) {
            int result = 0;
            struct task_file *stdio_file = task_descriptor_for_fd(task, (int)fd);
            struct task_pty_fd *pty_fd = task_pty_fd_for_fd(task, (int)fd);
            if (stdio_file) {
                syscall_record_locks_close(task, stdio_file);
                result = clear_task_file_result(stdio_file);
            } else if (pty_fd) {
                task_pty_release_entry(pty_fd);
            } else if (sched_task_fds(task)->closed_stdio_mask & (1u << fd)) {
                return -RELIEFOS_EBADF;
            }
            /* The inherited PTY/console streams have no task_file entry.
             * Closing one must still release its descriptor number so that
             * open("/dev/null") after close(0) returns zero. */
            sched_task_fds(task)->closed_stdio_mask |= 1u << fd;
            return result;
        }
        struct task_file *descriptor = task_descriptor_for_fd(task, (int)fd);
        if (descriptor) {
            syscall_record_locks_close(task, descriptor);
            return clear_task_file_result(descriptor);
        }
        {
            struct task_pty_fd *pty_fd = task_pty_fd_for_fd(task, (int)fd);
            if (pty_fd) {
                task_pty_release_entry(pty_fd);
                return 0;
            }
        }
        return -RELIEFOS_EBADF;
    }

    if (number == LINUX_SYS_DUP3) {
        struct task *task = sched_current_task();
        struct task_file *file;
        int result;
        a2 = (uint32_t)a2;
        if ((int)a0 == (int)a1) return -RELIEFOS_EINVAL;
        if (a2 & ~(uint64_t)RELIEFOS_O_CLOEXEC) return -RELIEFOS_EINVAL;
        if (task_file_for_fd(task, (int)a0)) {
            result = task_dup2_fd(task, (int)a0, (int)a1);
            if (result < 0) return result;
            file = task_descriptor_for_fd(task, result);
            if (file) file->fd_flags = (a2 & RELIEFOS_O_CLOEXEC) ? RELIEFOS_FD_CLOEXEC : 0;
            return result;
        }
        result = task_pty_dup2_fd(task, (int)a0, (int)a1);
        if (result < 0) return result;
        struct task_pty_fd *pty_fd = task_pty_fd_for_fd(task, result);
        if (pty_fd) pty_fd->flags = (a2 & RELIEFOS_O_CLOEXEC) ? RELIEFOS_FD_CLOEXEC : 0;
        return result;
    }

    if (number == LINUX_SYS_DUP || number == LINUX_SYS_DUP2 || number == LINUX_SYS_FCNTL) {
        struct task *task = sched_current_task();
        /* fcntl's command is unsigned int; its argument remains unsigned long. */
        if (number == LINUX_SYS_FCNTL) a1 = (uint32_t)a1;
        if (number == LINUX_SYS_FCNTL && (a1 == LINUX_F_GETLK || a1 == LINUX_F_SETLK || a1 == LINUX_F_SETLKW))
            return syscall_record_lock((int32_t)a0, (uint32_t)a1, a2);
        if (number == LINUX_SYS_DUP) {
            struct task_file *source = task_file_for_fd(task, (int)a0);
            if (source) {
                if (!sched_task_limits(task)->nofile.rlim_cur) return -RELIEFOS_EMFILE;
                return task_duplicate_file_fd(task, (int)a0, 0, 0);
            }
            if (task && !sched_task_limits(task)->nofile.rlim_cur &&
                (task_pty_fd_for_fd(task, (int)a0) || task_pty_stream_for_fd(task, (int)a0) >= 0))
                return -RELIEFOS_EMFILE;
            return task_pty_duplicate_fd(task, (int)a0, 0, 0);
        }
        if (number == LINUX_SYS_DUP2) {
            if (task_file_for_fd(task, (int)a0)) {
                return task_dup2_fd(task, (int)a0, (int)a1);
            }
            return task_pty_dup2_fd(task, (int)a0, (int)a1);
        }
        if (a1 == RELIEFOS_F_DUPFD || a1 == RELIEFOS_F_DUPFD_CLOEXEC) {
            if (task_file_for_fd(task, (int)a0)) {
                return task_duplicate_file_fd(task, (int)a0, (int)a2,
                                              a1 == RELIEFOS_F_DUPFD_CLOEXEC ?
                                                  RELIEFOS_FD_CLOEXEC : 0);
            }
            return task_pty_duplicate_fd(task, (int)a0, (int)a2,
                                         a1 == RELIEFOS_F_DUPFD_CLOEXEC ? RELIEFOS_FD_CLOEXEC : 0);
        }
        {
            struct task_file *file_fd = task_file_for_fd(task, (int)a0);
            struct task_pty_fd *pty_fd = task_pty_fd_for_fd(task, (int)a0);
            /* Descriptor flags live in the descriptor table entry, never in
             * the shared open file description, and use the same storage as
             * ioctl(FIOCLEX/FIONCLEX) so the two interfaces cannot disagree.
             * Implicit stdin/stdout/stderr descriptors are covered as well. */
            if (a1 == RELIEFOS_F_GETFD) {
                return task_fd_descriptor_flags(task, (int)a0);
            }
            if (a1 == RELIEFOS_F_SETFD) {
                int ret = task_fd_set_descriptor_flags(task, (int)a0, (uint32_t)a2);
                return ret < 0 ? ret : 0;
            }
            if (!file_fd && !pty_fd && task_pty_stream_for_fd(task, (int)a0) < 0) {
                return -RELIEFOS_EBADF;
            }
            if (a1 == RELIEFOS_F_GETFL) {
                if (file_fd) {
                    return (int64_t)(file_fd->flags &
                        (RELIEFOS_O_ACCMODE | RELIEFOS_O_APPEND | RELIEFOS_O_NONBLOCK));
                }
                if (pty_fd) {
                    return (int64_t)task_pty_status(pty_fd);
                }
                /* Implicit controlling-TTY stdio descriptors are readable
                 * (fd 0) or writable (fd 1/2). */
                return a0 == 0 ? (int64_t)RELIEFOS_O_RDONLY
                               : (int64_t)RELIEFOS_O_WRONLY;
            }
            if (a1 == RELIEFOS_F_SETFL) {
                if (file_fd) {
                    /* Preserve descriptor-kind/device bits (PIPE, SOCKET_*,
                     * DEV_*) while updating only the open-status flags. */
                    file_fd->flags = (file_fd->flags &
                                      ~(RELIEFOS_O_APPEND | RELIEFOS_O_NONBLOCK)) |
                                     (file_fd->flags & RELIEFOS_O_ACCMODE) |
                                     ((uint32_t)a2 & (RELIEFOS_O_APPEND |
                                                      RELIEFOS_O_NONBLOCK));
                } else {
                    int ret = task_pty_ensure_fd(task, (int)a0, &pty_fd);
                    if (ret < 0) return ret;
                    pty_fd->description->status_flags = (task_pty_status(pty_fd) & RELIEFOS_O_ACCMODE) |
                        ((uint32_t)a2 & (RELIEFOS_O_APPEND | RELIEFOS_O_NONBLOCK));
                }
                return 0;
            }
            if (a1 == LINUX_F_GETPIPE_SZ || a1 == LINUX_F_SETPIPE_SZ)
                return task_pipe_capacity(file_fd, a1 == LINUX_F_SETPIPE_SZ, (uint32_t)a2);
            if (a1 == LINUX_F_GET_SEALS || a1 == LINUX_F_ADD_SEALS) {
                /* Linux memfd_fcntl: sealing is a shmem-file interface; pty
                 * and stream descriptors are not sealable and report EINVAL. */
                if (!file_fd) return -RELIEFOS_EINVAL;
                return a1 == LINUX_F_GET_SEALS
                    ? task_shm_get_seals(file_fd)
                    : task_shm_add_seals(file_fd, (uint32_t)a2);
            }
        }
        return -RELIEFOS_ENOSYS;
    }

    if (number == LINUX_SYS_FLOCK) {
        return syscall_flock(a0, (uint32_t)a1);
    }

    if (number == LINUX_SYS_CLOSE_RANGE) {
        struct task *task = sched_current_task();
        uint32_t first = (uint32_t)a0;
        uint32_t last = (uint32_t)a1;
        uint32_t flags = (uint32_t)a2;
        if (!task) return -RELIEFOS_EPERM;
        if (last < first || (flags & ~6u)) return -RELIEFOS_EINVAL;
        if (flags & 2u) {
            int unshare_ret = syscall_unshare_task_files(task);
            if (unshare_ret < 0) return unshare_ret;
        }
        /* Linux accepts UINT_MAX as the open-ended upper bound. Iterate only
         * materialized descriptor slots; PTY aliases live in a separate small
         * table and are handled independently below. */
        uint32_t stdio_last = last < 2u ? last : 2u;
        for (uint32_t fd = first; fd <= stdio_last; ++fd) {
            if (flags & 4u) {
                sched_task_fds(task)->cloexec_stdio_mask |= 1u << fd;
            } else {
                (void)syscall_dispatch_regs_legacy(LINUX_SYS_CLOSE, fd, 0, 0, 0, 0, 0);
            }
        }
        uint32_t file_first = first < 3u ? 3u : first;
        uint32_t file_capacity = sched_task_file_capacity(task);
        uint64_t file_end = (uint64_t)file_capacity + 2u;
        uint32_t file_last = last < file_end ? last :
                             file_end > UINT32_MAX ? UINT32_MAX : (uint32_t)file_end;
        if (file_first <= file_last) {
            for (uint64_t fd = file_first; fd <= file_last; ++fd) {
                if (flags & 4u) {
                    struct task_file *descriptor = task_descriptor_for_fd(task, (int)fd);
                    if (descriptor) descriptor->fd_flags |= RELIEFOS_FD_CLOEXEC;
                } else {
                    (void)syscall_dispatch_regs_legacy(LINUX_SYS_CLOSE, fd, 0, 0, 0, 0, 0);
                }
            }
        }
        for (uint32_t i = 0; i < sched_task_pty_capacity(task); ++i) {
            struct task_pty_fd *pty_fd = sched_task_pty_at(task, i);
            if (!pty_fd->used || pty_fd->fd < 0 || (uint32_t)pty_fd->fd < first ||
                (uint32_t)pty_fd->fd > last) continue;
            if (flags & 4u) pty_fd->flags |= RELIEFOS_FD_CLOEXEC;
            else (void)syscall_dispatch_regs_legacy(LINUX_SYS_CLOSE,
                                                     (uint32_t)pty_fd->fd, 0, 0, 0, 0, 0);
        }
        return 0;
    }

    return -RELIEFOS_ENOSYS;
}
