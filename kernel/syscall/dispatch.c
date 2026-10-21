#include "internal.h"

/**
 * @brief Apply Linux' ioctl() descriptor-resolution rule for O_PATH files.
 *
 * ioctl() resolves its descriptor with fdget(), which rejects FMODE_PATH, so
 * every request on an open(O_PATH) descriptor fails with EBADF before any
 * generic or device handler runs.  fcntl() uses fdget_raw() instead and keeps
 * working on the same descriptor.
 *
 * @param task Descriptor-table owner; the caller runs under the syscall
 *             execution lock.
 * @param fd_arg Raw first syscall argument; only the low 32 bits are used.
 * @return Zero for an open, non-O_PATH descriptor, otherwise -RELIEFOS_EBADF.
 */
static int syscall_ioctl_resolve_fd(struct task *task, uint64_t fd_arg);

/**
 * @brief Implement Linux do_vfs_ioctl() FIOCLEX/FIONCLEX for every descriptor.
 *
 * Linux handles these two requests in the VFS switch before any device or
 * filesystem ->unlocked_ioctl handler, for every descriptor type that can be
 * resolved by fdget(): regular files, directories, pipes, sockets, PTYs,
 * device nodes and anonymous descriptors (O_PATH is rejected earlier).  The third
 * ioctl argument is unused and must never be dereferenced, so it is not part
 * of this interface at all.
 *
 * @param task Descriptor-table owner; NULL yields -RELIEFOS_EBADF.
 * @param fd_arg Raw first syscall argument; only the low 32 bits name the
 *               descriptor, matching Linux' unsigned int fd parameter.
 * @param cmd ioctl request, already truncated to 32 bits by the dispatcher.
 * @return Zero on success, -RELIEFOS_EBADF for an unopened descriptor, or
 *         -RELIEFOS_ENOTTY for every other ioctl request.
 */
static int64_t syscall_ioctl_descriptor_flags(struct task *task, uint64_t fd_arg,
                                              uint32_t cmd);

static int64_t syscall_ioctl_nonblock(struct task *task, int fd, uint64_t argument);

/**
 * Syscall dispatch regs.
 * @param frame Value supplied by the caller.
 */
int64_t syscall_dispatch_regs(struct task *task, uint64_t number, uint64_t a0, uint64_t a1,
                                     uint64_t a2, uint64_t a3, uint64_t a4,
                                     uint64_t a5);

struct reliefos_flock_entry reliefos_flocks[RELIEFOS_FLOCK_MAX];

/**
 * @brief Apply Linux' ioctl() descriptor-resolution rule for O_PATH files.
 *
 * ioctl() resolves its descriptor with fdget(), which rejects FMODE_PATH, so
 * every request on an open(O_PATH) descriptor fails with EBADF before any
 * generic or device handler runs.  fcntl() uses fdget_raw() instead and keeps
 * working on the same descriptor.
 *
 * @param task Descriptor-table owner; the caller runs under the syscall
 *             execution lock.
 * @param fd_arg Raw first syscall argument; only the low 32 bits are used.
 * @return Zero for an open, non-O_PATH descriptor, otherwise -RELIEFOS_EBADF.
 */
static int syscall_ioctl_resolve_fd(struct task *task, uint64_t fd_arg)
{
    int fd = (int)(uint32_t)fd_arg;
    if (task && task->syscall_file && task->syscall_fd == fd &&
        task->syscall_file_number == LINUX_SYS_IOCTL)
        return task->syscall_file->flags & TASK_FILE_FLAG_PATH ? -RELIEFOS_EBADF : 0;
    if (task_fd_descriptor_flags(task, fd) < 0) return -RELIEFOS_EBADF;
    struct task_file *file = task_file_for_fd(task, fd);
    return file && (file->flags & TASK_FILE_FLAG_PATH) ? -RELIEFOS_EBADF : 0;
}

/**
 * @brief Implement Linux do_vfs_ioctl() FIOCLEX/FIONCLEX for every descriptor.
 *
 * Linux handles these two requests in the VFS switch before any device or
 * filesystem ->unlocked_ioctl handler, for every descriptor type that can be
 * resolved by fdget(): regular files, directories, pipes, sockets, PTYs,
 * device nodes and anonymous descriptors (O_PATH is rejected earlier).  The third
 * ioctl argument is unused and must never be dereferenced, so it is not part
 * of this interface at all.
 *
 * @param task Descriptor-table owner; NULL yields -RELIEFOS_EBADF.
 * @param fd_arg Raw first syscall argument; only the low 32 bits name the
 *               descriptor, matching Linux' unsigned int fd parameter.
 * @param cmd ioctl request, already truncated to 32 bits by the dispatcher.
 * @return Zero on success, -RELIEFOS_EBADF for an unopened descriptor, or
 *         -RELIEFOS_ENOTTY for every other ioctl request.
 */
static int64_t syscall_ioctl_descriptor_flags(struct task *task, uint64_t fd_arg,
                                              uint32_t cmd)
{
    int fd = (int)(uint32_t)fd_arg;
    if (cmd == FIOCLEX) {
        return task_fd_set_descriptor_flags(task, fd, RELIEFOS_FD_CLOEXEC);
    }
    if (cmd == FIONCLEX) {
        return task_fd_set_descriptor_flags(task, fd, 0);
    }
    return -RELIEFOS_ENOTTY;
}

static int64_t syscall_ioctl_nonblock(struct task *task, int fd, uint64_t argument)
{
    int32_t enabled;
    struct task_file *file = task_file_for_fd(task, fd);
    if (!user_range_ok(argument, sizeof(enabled))) return -RELIEFOS_EFAULT;
    __builtin_memcpy(&enabled, (const void *)(uintptr_t)argument, sizeof(enabled));
    /* Linux fs/ioctl.c updates the shared open description, not the
     * descriptor flags. Preserve access mode, append and internal kind bits. */
    if (file) {
        if (enabled) file->flags |= RELIEFOS_O_NONBLOCK;
        else file->flags &= ~RELIEFOS_O_NONBLOCK;
    } else {
        struct task_pty_fd *entry;
        int ret = task_pty_ensure_fd(task, fd, &entry);
        if (ret < 0) return ret;
        if (enabled) entry->description->status_flags |= RELIEFOS_O_NONBLOCK;
        else entry->description->status_flags &= ~RELIEFOS_O_NONBLOCK;
    }
    return 0;
}

void syscall_init(void)
{
    console_info("[reliefnt] Linux x86_64 syscall ABI registered\n");
}

/**
 * Syscall dispatch.
 * @param frame Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int64_t syscall_dispatch(const struct syscall_frame *frame)
{
    if (!frame) {
        return -RELIEFOS_EFAULT;
    }

    switch (frame->number) {
    case LINUX_SYS_READ:
    case LINUX_SYS_WRITE:
    case LINUX_SYS_OPEN:
    case LINUX_SYS_OPENAT:
    case LINUX_SYS_OPENAT2:
    case LINUX_SYS_CLOSE:
    case LINUX_SYS_STAT:
    case LINUX_SYS_FSTAT:
    case LINUX_SYS_STATX:
    case LINUX_SYS_SENDFILE:
    case LINUX_SYS_COPY_FILE_RANGE:
    case LINUX_SYS_EVENTFD:
    case LINUX_SYS_EVENTFD2:
    case LINUX_SYS_SIGNALFD:
    case LINUX_SYS_SIGNALFD4:
    case LINUX_SYS_PROCESS_VM_READV:
    case LINUX_SYS_PROCESS_VM_WRITEV:
    case LINUX_SYS_MSGGET:
    case LINUX_SYS_SEMGET:
    case LINUX_SYS_SEMOP:
    case LINUX_SYS_SEMCTL:
    case LINUX_SYS_SEMTIMEDOP:
    case LINUX_SYS_MSGSND:
    case LINUX_SYS_MSGRCV:
    case LINUX_SYS_MSGCTL:
    case LINUX_SYS_EPOLL_CREATE:
    case LINUX_SYS_EPOLL_WAIT:
    case LINUX_SYS_EPOLL_CTL:
    case LINUX_SYS_EPOLL_PWAIT:
    case LINUX_SYS_EPOLL_CREATE1:
    case LINUX_SYS_EPOLL_PWAIT2:
    case LINUX_SYS_MEMFD_CREATE:
    case LINUX_SYS_LSEEK:
    case LINUX_SYS_FTRUNCATE:
    case LINUX_SYS_BRK:
    case LINUX_SYS_MSYNC:
    case LINUX_SYS_MINCORE:
    case LINUX_SYS_MADVISE:
    case LINUX_SYS_FADVISE64:
    case LINUX_SYS_READAHEAD:
    case LINUX_SYS_FALLOCATE:
    case LINUX_SYS_SYNC_FILE_RANGE:
    case LINUX_SYS_PREADV:
    case LINUX_SYS_PWRITEV:
    case LINUX_SYS_PREADV2:
    case LINUX_SYS_PWRITEV2:
    case LINUX_SYS_SYNC:
    case LINUX_SYS_SYNCFS:
    case LINUX_SYS_IOCTL:
    case LINUX_SYS_SELECT:
    case LINUX_SYS_PSELECT6:
    case LINUX_SYS_GETRUSAGE:
    case LINUX_SYS_SYSINFO:
    case LINUX_SYS_TIMES:
    case LINUX_SYS_POLL:
    case LINUX_SYS_PPOLL:
    case LINUX_SYS_SCHED_YIELD:
    case LINUX_SYS_NANOSLEEP:
    case LINUX_SYS_ALARM:
    case LINUX_SYS_GETITIMER:
    case LINUX_SYS_SETITIMER:
    case LINUX_SYS_EXECVE:
    case LINUX_SYS_EXECVEAT:
    case LINUX_SYS_EXIT:
    case LINUX_SYS_EXIT_GROUP:
    case LINUX_SYS_WAIT4:
    case LINUX_SYS_WAITID:
    case LINUX_SYS_CHMOD:
    case LINUX_SYS_FCHMOD:
    case LINUX_SYS_CHOWN:
    case LINUX_SYS_FCHOWN:
    case LINUX_SYS_GETCWD:
    case LINUX_SYS_CHDIR:
    case LINUX_SYS_CHROOT:
    case LINUX_SYS_RENAME:
    case LINUX_SYS_RENAMEAT:
    case LINUX_SYS_RENAMEAT2:
    case LINUX_SYS_CREAT:
    case LINUX_SYS_GETDENTS:
    case LINUX_SYS_MKDIR:
    case LINUX_SYS_RMDIR:
    case LINUX_SYS_UNLINK:
    case LINUX_SYS_LINK:
    case LINUX_SYS_SYMLINK:
    case LINUX_SYS_READLINK:
    case LINUX_SYS_LINKAT:
    case LINUX_SYS_SYMLINKAT:
    case LINUX_SYS_READLINKAT:
    case LINUX_SYS_PIPE:
    case LINUX_SYS_PIPE2:
    case LINUX_SYS_DUP:
    case LINUX_SYS_DUP2:
    case LINUX_SYS_DUP3:
    case LINUX_SYS_FORK:
    case LINUX_SYS_VFORK:
    case LINUX_SYS_FCNTL:
    case LINUX_SYS_FLOCK:
    case LINUX_SYS_SOCKET:
    case LINUX_SYS_SOCKETPAIR:
    case LINUX_SYS_SENDMSG:
    case LINUX_SYS_RECVMSG:
    case LINUX_SYS_SENDMMSG:
    case LINUX_SYS_RECVMMSG:
    case LINUX_SYS_ACCEPT4:
    case LINUX_SYS_CONNECT:
    case LINUX_SYS_ACCEPT:
    case LINUX_SYS_BIND:
    case LINUX_SYS_LISTEN:
    case LINUX_SYS_GETSOCKNAME:
    case LINUX_SYS_GETPEERNAME:
    case LINUX_SYS_GETSOCKOPT:
    case LINUX_SYS_SETSOCKOPT:
    case LINUX_SYS_SHUTDOWN:
    case LINUX_SYS_SEND:
    case LINUX_SYS_RECV:
    case LINUX_SYS_RT_SIGACTION:
    case LINUX_SYS_RT_SIGPROCMASK:
    case LINUX_SYS_RT_SIGSUSPEND:
    case LINUX_SYS_RT_SIGPENDING:
    case LINUX_SYS_SIGALTSTACK:
    case LINUX_SYS_FUTEX:
    case LINUX_SYS_FUTEX_WAKE:
    case LINUX_SYS_FUTEX_WAIT:
    case LINUX_SYS_FUTEX_REQUEUE:
    case LINUX_SYS_FUTEX_WAITV:
    case LINUX_SYS_CLOCK_NANOSLEEP:
    case LINUX_SYS_GETRANDOM:
    case LINUX_SYS_TIME:
    case LINUX_SYS_GETCPU:
    case LINUX_SYS_CLOSE_RANGE:
        return syscall_dispatch_regs(sched_current_task(), frame->number, frame->args[0], frame->args[1],
                                     frame->args[2], frame->args[3], frame->args[4],
                                     frame->args[5]);
    case LINUX_SYS_CLOCK_GETRES:
    case LINUX_SYS_GETTID:
    case LINUX_SYS_SET_TID_ADDRESS:
    case LINUX_SYS_SET_ROBUST_LIST:
    case LINUX_SYS_GET_ROBUST_LIST:
    case LINUX_SYS_ARCH_PRCTL:
        return syscall_dispatch_regs(sched_current_task(), frame->number, frame->args[0], frame->args[1],
                                     frame->args[2], frame->args[3], frame->args[4],
                                     frame->args[5]);
    case LINUX_SYS_PAUSE:
        return syscall_dispatch_regs(sched_current_task(), frame->number, 0, 0, 0, 0, 0, 0);
    case LINUX_SYS_RT_SIGRETURN:
        /* rt_sigreturn restores the live trap frame directly. The regs-only
         * debug dispatch entry has no frame to restore and returns ENOSYS. */
        return -RELIEFOS_ENOSYS;
    case LINUX_SYS_GETUID:
    case LINUX_SYS_GETGID:
    case LINUX_SYS_GETEUID:
    case LINUX_SYS_GETEGID:
    case LINUX_SYS_SETUID:
    case LINUX_SYS_SETGID:
    case LINUX_SYS_SETREUID:
    case LINUX_SYS_SETREGID:
    case LINUX_SYS_SETRESUID:
    case LINUX_SYS_GETRESUID:
    case LINUX_SYS_SETRESGID:
    case LINUX_SYS_GETRESGID:
    case LINUX_SYS_SETFSUID:
    case LINUX_SYS_SETFSGID:
    case LINUX_SYS_SCHED_GET_PRIORITY_MAX:
    case LINUX_SYS_SCHED_GET_PRIORITY_MIN:
    case LINUX_SYS_SCHED_GETPARAM:
    case LINUX_SYS_SCHED_GETSCHEDULER:
    case LINUX_SYS_SCHED_SETPARAM:
    case LINUX_SYS_SCHED_SETSCHEDULER:
    case LINUX_SYS_SCHED_RR_GET_INTERVAL:
    case LINUX_SYS_SCHED_SETATTR:
    case LINUX_SYS_SCHED_GETATTR:
    case LINUX_SYS_PERSONALITY:
    case LINUX_SYS_UNAME:
    case LINUX_SYS_SETHOSTNAME:
    case LINUX_SYS_SETDOMAINNAME:
    case LINUX_SYS_MEMBARRIER:
    case LINUX_SYS_RSEQ:
    case LINUX_SYS_GETTIMEOFDAY:
    case LINUX_SYS_SETTIMEOFDAY:
    case LINUX_SYS_SCHED_SETAFFINITY:
    case LINUX_SYS_SCHED_GETAFFINITY:
    case LINUX_SYS_CLOCK_GETTIME:
    case LINUX_SYS_REBOOT:
    case LINUX_SYS_GETPID:
    case LINUX_SYS_GETPPID:
    case LINUX_SYS_SETPGID:
    case LINUX_SYS_GETPGRP:
    case LINUX_SYS_SETSID:
    case LINUX_SYS_GETPGID:
    case LINUX_SYS_GETSID:
    case LINUX_SYS_CAPGET:
    case LINUX_SYS_CAPSET:
    case LINUX_SYS_KILL:
    case LINUX_SYS_TKILL:
    case LINUX_SYS_TGKILL:
    case LINUX_SYS_RT_SIGQUEUEINFO:
    case LINUX_SYS_RT_TGSIGQUEUEINFO:
    case RELIEFOS_SYS_NICE:
    case LINUX_SYS_GETPRIORITY:
    case LINUX_SYS_SETPRIORITY:
        return syscall_process_control(frame->number, frame->args[0], frame->args[1],
                                       frame->args[2], frame->args[3]);
    case LINUX_SYS_GETRLIMIT:
    case LINUX_SYS_SETRLIMIT:
        return syscall_process_control(frame->number, frame->args[0], frame->args[1],
                                       frame->args[2], frame->args[3]);
    case LINUX_SYS_PRCTL:
        return syscall_process_prctl(frame->args[0], frame->args[1], frame->args[2],
                                     frame->args[3], frame->args[4]);
    case LINUX_SYS_MMAP:
        return syscall_mm_mmap(frame->args[0], frame->args[1], frame->args[2],
                        frame->args[3], frame->args[4], frame->args[5]);
    case LINUX_SYS_MREMAP:
        return syscall_mm_mremap(frame->args[0], frame->args[1], frame->args[2],
                                 frame->args[3], frame->args[4]);
    case LINUX_SYS_MLOCK:
    case LINUX_SYS_MLOCK2:
        return syscall_mm_mlock(frame->args[0], frame->args[1],
                                frame->number == LINUX_SYS_MLOCK2 ? frame->args[2] : 0);
    case LINUX_SYS_MUNLOCK:
        return syscall_mm_munlock(frame->args[0], frame->args[1]);
    case LINUX_SYS_MLOCKALL:
        return syscall_mm_mlockall(frame->args[0]);
    case LINUX_SYS_MUNLOCKALL:
        return syscall_mm_munlockall();
    case LINUX_SYS_MUNMAP:
        return syscall_mm_munmap(frame->args[0], frame->args[1]);
    case LINUX_SYS_MPROTECT:
        return syscall_mm_mprotect(frame->args[0], frame->args[1], frame->args[2]);
    default:
        return -RELIEFOS_ENOSYS;
    }
}

int64_t syscall_dispatch_regs_legacy(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    if (number == LINUX_SYS_UTIME || number == LINUX_SYS_UTIMES ||
        number == LINUX_SYS_FUTIMESAT || number == LINUX_SYS_UTIMENSAT)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_OPENAT2)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_SOCKET || number == LINUX_SYS_CONNECT ||
        number == LINUX_SYS_ACCEPT || number == LINUX_SYS_ACCEPT4 ||
        number == LINUX_SYS_SOCKETPAIR || number == LINUX_SYS_BIND ||
        number == LINUX_SYS_LISTEN || number == LINUX_SYS_GETSOCKNAME || number == LINUX_SYS_GETPEERNAME ||
        number == LINUX_SYS_GETSOCKOPT || number == LINUX_SYS_SETSOCKOPT ||
        number == LINUX_SYS_SHUTDOWN || number == LINUX_SYS_SEND ||
        number == LINUX_SYS_RECV || number == LINUX_SYS_SENDTO ||
        number == LINUX_SYS_RECVFROM || number == LINUX_SYS_SENDMSG ||
        number == LINUX_SYS_RECVMSG || number == LINUX_SYS_SENDMMSG || number == LINUX_SYS_RECVMMSG)
        return syscall_legacy_ipc(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_PRCTL)
        return syscall_legacy_process(number, a0, a1, a2, a3, a4, a5);
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
        number == LINUX_SYS_SCHED_GETAFFINITY || number == LINUX_SYS_REBOOT)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == __NR_adjtimex || number == __NR_clock_adjtime)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_RT_SIGACTION || number == LINUX_SYS_RT_SIGPROCMASK ||
        number == LINUX_SYS_RT_SIGSUSPEND || number == LINUX_SYS_RT_SIGPENDING ||
        number == LINUX_SYS_SIGALTSTACK)
        return syscall_legacy_process(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_CLOCK_GETRES || number == LINUX_SYS_GETTID ||
        number == LINUX_SYS_SET_TID_ADDRESS || number == LINUX_SYS_ARCH_PRCTL ||
        number == LINUX_SYS_SET_ROBUST_LIST || number == LINUX_SYS_GET_ROBUST_LIST)
        return syscall_legacy_process(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_PAUSE)
        return syscall_legacy_process(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_EPOLL_CREATE)
        return syscall_legacy_events(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_EPOLL_CREATE1)
        return syscall_legacy_events(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_EPOLL_CTL)
        return syscall_legacy_events(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_EPOLL_WAIT || number == LINUX_SYS_EPOLL_PWAIT)
        return syscall_legacy_events(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_EPOLL_PWAIT2)
        return syscall_legacy_events(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_EVENTFD || number == LINUX_SYS_EVENTFD2)
        return syscall_legacy_events(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_SIGNALFD || number == LINUX_SYS_SIGNALFD4)
        return syscall_legacy_events(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_PROCESS_VM_READV || number == LINUX_SYS_PROCESS_VM_WRITEV)
        return syscall_legacy_process(number, a0, a1, a2, a3, a4, a5);
    if (number >= LINUX_SYS_MSGGET && number <= LINUX_SYS_MSGCTL)
        return syscall_legacy_ipc(number, a0, a1, a2, a3, a4, a5);
    if ((number >= LINUX_SYS_SEMGET && number <= LINUX_SYS_SEMCTL) || number == LINUX_SYS_SEMTIMEDOP)
        return syscall_legacy_ipc(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_TIMERFD_CREATE)
        return syscall_legacy_events(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_TIMERFD_SETTIME)
        return syscall_legacy_events(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_TIMERFD_GETTIME)
        return syscall_legacy_events(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_MEMFD_CREATE)
        return syscall_legacy_memory(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_POLL)
        return syscall_legacy_events(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_GETRUSAGE)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_SYSINFO)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_TIMES)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_SELECT)
        return syscall_legacy_events(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_PSELECT6)
        return syscall_legacy_events(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_PPOLL)
        return syscall_legacy_events(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_MSYNC)
        return syscall_legacy_memory(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_MINCORE)
        return syscall_legacy_memory(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_MADVISE)
        return syscall_legacy_memory(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_FADVISE64)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_READAHEAD)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_FALLOCATE)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_SYNC_FILE_RANGE)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_SYNC)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_SYNCFS)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_FCHMODAT || number == LINUX_SYS_FCHMODAT2 ||
        number == LINUX_SYS_FCHOWNAT || number == LINUX_SYS_FACCESSAT ||
        number == LINUX_SYS_FACCESSAT2)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_GETGROUPS || number == LINUX_SYS_SETGROUPS)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_CHMOD || number == LINUX_SYS_CHOWN || number == LINUX_SYS_LCHOWN)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_FCHMOD || number == LINUX_SYS_FCHOWN)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_WRITE)
        return syscall_legacy_io(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_READ)
        return syscall_legacy_io(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_EXIT || number == LINUX_SYS_EXIT_GROUP)
        return syscall_legacy_process(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_EXECVE || number == LINUX_SYS_EXECVEAT)
        return syscall_legacy_process(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_FORK)
        return syscall_legacy_process(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_VFORK)
        return syscall_legacy_process(number, a0, a1, a2, a3, a4, a5);
    if (number == __NR_mknod || number == __NR_mknodat)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_CREAT)
        return syscall_legacy_descriptors(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_OPEN || number == LINUX_SYS_OPENAT)
        return syscall_legacy_descriptors(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_PIPE)
        return syscall_legacy_descriptors(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_PIPE2)
        return syscall_legacy_descriptors(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_CLOSE)
        return syscall_legacy_descriptors(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_FTRUNCATE)
        return syscall_legacy_descriptors(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_DUP3)
        return syscall_legacy_descriptors(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_DUP || number == LINUX_SYS_DUP2 || number == LINUX_SYS_FCNTL)
        return syscall_legacy_descriptors(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_FLOCK)
        return syscall_legacy_descriptors(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_STATFS || number == LINUX_SYS_FSTATFS)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_STAT || number == LINUX_SYS_LSTAT || number == LINUX_SYS_NEWFSTATAT)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_STATX)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_SENDFILE)
        return syscall_legacy_io(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_COPY_FILE_RANGE)
        return syscall_legacy_io(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_FSTAT)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_ACCESS)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_LINK || number == LINUX_SYS_LINKAT ||
        number == LINUX_SYS_SYMLINK || number == LINUX_SYS_SYMLINKAT)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_READLINK || number == LINUX_SYS_READLINKAT)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_FCHDIR)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_TRUNCATE)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_FSYNC || number == LINUX_SYS_FDATASYNC)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_UMASK)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_PREAD64 || number == LINUX_SYS_PWRITE64)
        return syscall_legacy_io(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_TIME)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_GETCPU)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_CLOSE_RANGE)
        return syscall_legacy_descriptors(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_READV || number == LINUX_SYS_WRITEV ||
        number == LINUX_SYS_PREADV || number == LINUX_SYS_PWRITEV ||
        number == LINUX_SYS_PREADV2 || number == LINUX_SYS_PWRITEV2)
        return syscall_legacy_io(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_GETDENTS || number == LINUX_SYS_GETDENTS64)
        return syscall_legacy_io(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_LSEEK)
        return syscall_legacy_io(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_GETCWD)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_CHDIR || number == LINUX_SYS_CHROOT)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_MKDIR || number == LINUX_SYS_MKDIRAT)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_UNLINK || number == LINUX_SYS_RMDIR || number == LINUX_SYS_UNLINKAT)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_RENAME || number == LINUX_SYS_RENAMEAT ||
        number == LINUX_SYS_RENAMEAT2)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_MOUNT)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_UMOUNT2)
        return syscall_legacy_filesystem(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_GETPID || number == LINUX_SYS_GETPPID ||
        number == LINUX_SYS_SETPGID || number == LINUX_SYS_GETPGRP ||
        number == LINUX_SYS_SETSID || number == LINUX_SYS_GETPGID || number == LINUX_SYS_GETSID ||
        number == LINUX_SYS_KILL || number == LINUX_SYS_TKILL ||
        number == LINUX_SYS_TGKILL || number == RELIEFOS_SYS_NICE ||
        number == LINUX_SYS_RT_SIGQUEUEINFO || number == LINUX_SYS_RT_TGSIGQUEUEINFO ||
        number == LINUX_SYS_GETPRIORITY || number == LINUX_SYS_SETPRIORITY)
        return syscall_legacy_process(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_GETRLIMIT || number == LINUX_SYS_SETRLIMIT || number == LINUX_SYS_PRLIMIT64)
        return syscall_legacy_process(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_SCHED_YIELD)
        return syscall_legacy_process(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_NANOSLEEP)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_ALARM)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_GETITIMER || number == LINUX_SYS_SETITIMER)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_TIMER_CREATE)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_TIMER_SETTIME)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_TIMER_GETTIME)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_TIMER_GETOVERRUN)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_TIMER_DELETE)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_RT_SIGTIMEDWAIT)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_CLOCK_NANOSLEEP)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_GETRANDOM)
        return syscall_legacy_system(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_FUTEX)
        return syscall_legacy_ipc(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_FUTEX_WAITV)
        return syscall_legacy_ipc(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_FUTEX_WAKE)
        return syscall_legacy_ipc(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_FUTEX_WAIT)
        return syscall_legacy_ipc(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_FUTEX_REQUEUE)
        return syscall_legacy_ipc(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_MMAP)
        return syscall_legacy_memory(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_MREMAP)
        return syscall_legacy_memory(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_MLOCK || number == LINUX_SYS_MLOCK2)
        return syscall_legacy_memory(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_MUNLOCK)
        return syscall_legacy_memory(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_MLOCKALL)
        return syscall_legacy_memory(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_MUNLOCKALL)
        return syscall_legacy_memory(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_BRK)
        return syscall_legacy_memory(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_MUNMAP)
        return syscall_legacy_memory(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_MPROTECT)
        return syscall_legacy_memory(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_WAIT4)
        return syscall_legacy_process(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_WAITID)
        return syscall_legacy_process(number, a0, a1, a2, a3, a4, a5);
    if (number == LINUX_SYS_IOCTL)
        return syscall_legacy_ioctl(number, a0, a1, a2, a3, a4, a5);
    return -RELIEFOS_ENOSYS;
}

/**
 * Syscall dispatch regs.
 * @param frame Value supplied by the caller.
 */
int64_t syscall_dispatch_regs(struct task *task, uint64_t number, uint64_t a0, uint64_t a1,
                                     uint64_t a2, uint64_t a3, uint64_t a4,
                                     uint64_t a5)
{
    /* Identity/time calls and exact entry-pinned READ/WRITE descriptions need
     * neither stdio materialization nor a subsystem ownership scan. Keep their
     * existing legacy route; scalar backends are shared with the vector loop.
     * Other calls materialize first (so fcntl/dup/fork still see real stdio
     * descriptions) and keep the full ownership scan. */
    switch (number) {
    case LINUX_SYS_READ:
    case LINUX_SYS_WRITE:
        /* Entry already pinned this exact open description. These operations
         * cannot observe unrelated implicit standard streams; fcntl/dup/fork
         * still materialize them through the ordinary path. Keep all buffer,
         * access-mode and device checks in the shared read/write backend. */
        if (task && task->syscall_file && task->syscall_fd == (int32_t)a0 &&
            task->syscall_file_number == number)
            return syscall_dispatch_regs_legacy(number, a0, a1, a2, a3, a4, a5);
        {
            int ret = task_console_materialize_stdio(task);
            if (ret < 0) return ret;
        }
        break;
    case LINUX_SYS_GETPID:
    case LINUX_SYS_GETPPID:
    case LINUX_SYS_GETTID:
    case LINUX_SYS_GETUID:
    case LINUX_SYS_GETEUID:
    case LINUX_SYS_GETGID:
    case LINUX_SYS_GETEGID:
    case LINUX_SYS_CLOCK_GETTIME:
    case LINUX_SYS_CLOCK_GETRES:
    case LINUX_SYS_GETTIMEOFDAY:
        return syscall_dispatch_regs_legacy(number, a0, a1, a2, a3, a4, a5);
    default:
        {
            int ret = task_console_materialize_stdio(task);
            if (ret < 0) return ret;
        }
        break;
    }
    /* Linux ioctl's fd and cmd are unsigned int on native x86-64. */
    if (number == LINUX_SYS_IOCTL) {
        a0 = (uint32_t)a0;
        a1 = (uint32_t)a1;
    }

    /* ioctl() resolves its descriptor with fdget(), which rejects FMODE_PATH,
     * so every request on an open(O_PATH) descriptor fails with EBADF before
     * any generic or device handler runs.  fcntl() uses fdget_raw() and keeps
     * working on the same descriptor, so this check stays ioctl-only. */
    if (number == LINUX_SYS_IOCTL) {
        int resolve = syscall_ioctl_resolve_fd(sched_current_task(), a0);
        if (resolve < 0) return resolve;
    }

    /* Linux do_vfs_ioctl() resolves FIOCLEX/FIONCLEX in the generic VFS switch
     * before any filesystem, GPU or device-specific handler, so the request
     * works on every descriptor type and never reaches a device backend.  The
     * third argument is ignored and therefore never validated or dereferenced. */
    if (number == LINUX_SYS_IOCTL &&
        ((uint32_t)a1 == FIOCLEX || (uint32_t)a1 == FIONCLEX)) {
        return syscall_ioctl_descriptor_flags(sched_current_task(), a0,
                                              (uint32_t)a1);
    }
    if (number == LINUX_SYS_IOCTL && a1 == FIONBIO) {
        return syscall_ioctl_nonblock(sched_current_task(), (int)a0, a2);
    }
    if (number == LINUX_SYS_IOCTL) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_io(task, (int)a0);
        if (file && (file->drm_file || file->drm_prime_file))
            return drm_fd_ioctl(task, file, (uint32_t)a1, a2);
    }

    if (syscall_fs_owns(number)) {
        return syscall_fs_dispatch(number, a0, a1, a2, a3, a4, a5);
    }
    if (syscall_ipc_owns(number)) {
        return syscall_ipc_dispatch(number, a0, a1, a2, a3, a4, a5);
    }
    if (syscall_security_owns(number, a1)) {
        return syscall_security_dispatch(number, a0, a1, a2, a3, a4, a5);
    }
    if (syscall_gui_owns(number, a1)) {
        return syscall_gui_dispatch(number, a0, a1, a2, a3, a4, a5);
    }
    if (syscall_device_owns(number, a1)) {
        return syscall_device_dispatch(number, a0, a1, a2, a3, a4, a5);
    }
    return syscall_dispatch_regs_legacy(number, a0, a1, a2, a3, a4, a5);
}
