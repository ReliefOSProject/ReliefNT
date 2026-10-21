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
int64_t syscall_legacy_process(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    if (number == LINUX_SYS_PRCTL)
        return syscall_process_prctl(a0, a1, a2, a3, a4);

    if (number == LINUX_SYS_RT_SIGACTION || number == LINUX_SYS_RT_SIGPROCMASK ||
        number == LINUX_SYS_RT_SIGSUSPEND || number == LINUX_SYS_RT_SIGPENDING ||
        number == LINUX_SYS_SIGALTSTACK) {
        return syscall_linux_signal(number, a0, a1, a2, a3, a4);
    }

    if (number == LINUX_SYS_CLOCK_GETRES || number == LINUX_SYS_GETTID ||
        number == LINUX_SYS_SET_TID_ADDRESS || number == LINUX_SYS_ARCH_PRCTL ||
        number == LINUX_SYS_SET_ROBUST_LIST || number == LINUX_SYS_GET_ROBUST_LIST) {
        return syscall_process_control(number, a0, a1, a2, a3);
    }

    if (number == LINUX_SYS_PAUSE) {
        sched_block_current();
        return -RELIEFOS_EINTR;
    }

    if (number == LINUX_SYS_PROCESS_VM_READV || number == LINUX_SYS_PROCESS_VM_WRITEV)
        return syscall_process_vm((int32_t)a0, a1, a2, a3, a4, a5, number == LINUX_SYS_PROCESS_VM_WRITEV);

    if (number == LINUX_SYS_EXIT || number == LINUX_SYS_EXIT_GROUP) {
        struct task *task = sched_current_task();
        uint32_t pid = sched_current_pid();
        if (number == LINUX_SYS_EXIT_GROUP && task) sched_exit_group(sched_task_tgid(task), a0 & 255);
        if (!task || !task->shared_files || task->shared_files->references == 1) pty_process_exit(pid);
        userland_process_exit(a0 & 255);
        return 0;
    }

    if (number == LINUX_SYS_EXECVE || number == LINUX_SYS_EXECVEAT) {
        struct task *task = sched_current_task();
        char path[RELIEFOS_FS_PATH_LEN];
        char raw[RELIEFOS_FS_PATH_LEN], execfn[RELIEFOS_FS_PATH_LEN + 32];
        struct exec_params_kernel params;
        bool at = number == LINUX_SYS_EXECVEAT;
        uint32_t flags = at ? (uint32_t)a4 : 0;
        if (flags & ~(LINUX_AT_EMPTY_PATH | LINUX_AT_SYMLINK_NOFOLLOW)) return -RELIEFOS_EINVAL;
        int dirfd = at ? (int32_t)a0 : LINUX_AT_FDCWD;
        uint64_t path_pointer = at ? a1 : a0;
        int ret = copy_user_path(raw, sizeof(raw), path_pointer);
        if (ret < 0) return ret;
        struct task_file *file = NULL;
        ret = resolve_kernel_path_at_flags(task, dirfd, raw, flags & LINUX_AT_EMPTY_PATH,
                    path, &file, false, flags & LINUX_AT_SYMLINK_NOFOLLOW ? 0 : FS_LOOKUP_FOLLOW);
        if (ret < 0) return ret;
        struct storage_node node;
        if (file) node = file->node;
        else {
            ret = storage_lookup_path(path, &node);
            if (ret < 0) return ret;
        }
        if (node.type == RELIEFOS_FS_TYPE_SYMLINK) return -LINUX_ELOOP;
        if (at && raw[0] != '/' && dirfd != LINUX_AT_FDCWD) {
            uint32_t position = 0;
            execfn[0] = 0;
            path_append_text(execfn, &position, sizeof(execfn), "/dev/fd/");
            path_append_u64(execfn, &position, sizeof(execfn), (uint32_t)dirfd);
            if (raw[0]) path_append_char(execfn, &position, sizeof(execfn), '/');
            path_append_text(execfn, &position, sizeof(execfn), raw);
        } else copy_text(execfn, sizeof(execfn), raw);
        ret = copy_exec_params_from_user(at ? a2 : a1, at ? a3 : a2, &params);
        if (ret < 0) return ret;
        bool inaccessible = at && raw[0] != '/' && dirfd != LINUX_AT_FDCWD &&
                            (task_fd_descriptor_flags(task, dirfd) & RELIEFOS_FD_CLOEXEC);
        ret = exec_resolve_scripts(task, path, &node, execfn, inaccessible, &params);
        if (ret < 0) return ret;
        return userland_exec_current_node(path, &node, execfn, params.argc, params.argv,
                                          params.envc, params.envp,
                                          params.data, params.data_len);
    }

    if (number == LINUX_SYS_FORK) {
        struct task *task = sched_current_task();
        return sched_fork_current(task ? &task->frame : NULL);
    }

    if (number == LINUX_SYS_VFORK) {
        struct task *task = sched_current_task();
        return sched_vfork_current(task ? &task->frame : NULL);
    }

    if (number == LINUX_SYS_GETPID || number == LINUX_SYS_GETPPID ||
        number == LINUX_SYS_SETPGID || number == LINUX_SYS_GETPGRP ||
        number == LINUX_SYS_SETSID || number == LINUX_SYS_GETPGID || number == LINUX_SYS_GETSID ||
        number == LINUX_SYS_KILL || number == LINUX_SYS_TKILL ||
        number == LINUX_SYS_TGKILL || number == RELIEFOS_SYS_NICE ||
        number == LINUX_SYS_RT_SIGQUEUEINFO || number == LINUX_SYS_RT_TGSIGQUEUEINFO ||
        number == LINUX_SYS_GETPRIORITY || number == LINUX_SYS_SETPRIORITY) {
        return syscall_process_control(number, a0, a1, a2, a3);
    }

    if (number == LINUX_SYS_GETRLIMIT || number == LINUX_SYS_SETRLIMIT || number == LINUX_SYS_PRLIMIT64) {
        return syscall_process_control(number, a0, a1, a2, a3);
    }

    if (number == LINUX_SYS_SCHED_YIELD) {
        sched_yield_current();
        return 0;
    }

    if (number == LINUX_SYS_WAIT4) {
        int32_t requested_pid = (int32_t)a0;
        int status = 0;
        int64_t pid;
        if ((uint32_t)a2 & ~(1u | 2u | 8u | 0xc0000000u)) return -RELIEFOS_EINVAL;
        pid = sched_wait_reap(sched_current_pid(), requested_pid, (uint32_t)a2 | 4u,
                              a1 ? &status : NULL, NULL);
        if (pid == -RELIEFOS_EAGAIN) {
            return (a2 & 1U) != 0 ? 0 : -RELIEFOS_EAGAIN;
        }
        if (pid <= 0) {
            return -RELIEFOS_ECHILD;
        }
        if (a1) {
            if (!user_range_writable(a1, sizeof(int))) return -RELIEFOS_EFAULT;
            *(int *)(uintptr_t)a1 = status;
        }
        return pid;
    }

    if (number == LINUX_SYS_WAITID) {
        /* waitid(2) reports the same child state as wait4, but writes a
         * fixed 128-byte siginfo_t and always returns zero on success. */
        enum { LINUX_P_ALL = 0, LINUX_P_PID = 1, LINUX_P_PGID = 2 };
        enum { LINUX_WNOHANG = 1, LINUX_WSTOPPED = 2, LINUX_WEXITED = 4,
               LINUX_WCONTINUED = 8, LINUX_WNOWAIT = 0x01000000 };
        uint32_t idtype = (uint32_t)a0;
        uint32_t options = (uint32_t)a3;
        int32_t wanted = -1;
        int status = 0;
        int64_t pid;
        if (idtype == LINUX_P_PID) {
            if ((int32_t)a1 <= 0) return -RELIEFOS_EINVAL;
            wanted = (int32_t)a1;
        }
        else if (idtype == LINUX_P_PGID) {
            if ((int32_t)a1 < 0) return -RELIEFOS_EINVAL;
            wanted = (int32_t)a1 ? -(int32_t)a1 : 0;
        } else if (idtype != LINUX_P_ALL) return -RELIEFOS_EINVAL;
        if (!(options & (LINUX_WEXITED | LINUX_WSTOPPED | LINUX_WCONTINUED)) ||
            options & ~(uint32_t)(LINUX_WNOHANG | LINUX_WSTOPPED |
                                  LINUX_WEXITED | LINUX_WCONTINUED | LINUX_WNOWAIT | 0xc0000000u)) {
            return -RELIEFOS_EINVAL;
        }
        struct linux_siginfo info = {0};
        pid = sched_wait_reap(sched_current_pid(), wanted, options, &status, &info);
        if (pid == -RELIEFOS_EAGAIN) {
            if (!(options & LINUX_WNOHANG)) return pid;
            pid = 0;
        } else if (pid <= 0) pid = -RELIEFOS_ECHILD;
        if (a2) {
            if (!user_range_writable(a2, sizeof(info))) return -RELIEFOS_EFAULT;
            *(struct linux_siginfo *)(uintptr_t)a2 = info;
        }
        return pid < 0 ? pid : 0;
    }

    return -RELIEFOS_ENOSYS;
}
