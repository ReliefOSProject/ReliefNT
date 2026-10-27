#include <reliefnt/graphics_service.h>
#include "internal.h"
#include <reliefnt/module.h>

/** @brief Distinguish a nonblocking stream result from an internal I/O wait.
 * @param task Current task under execution ownership, nullable.
 * @param number Native Linux syscall number.
 * @param fd First argument interpreted as the admitted original descriptor.
 * @return One for userspace-visible EAGAIN, zero for a retryable storage wait.
 * Regular files ignore O_NONBLOCK; sendfile tests its output stream and
 * copy_file_range never turns disk waits into nonblocking errors. No callback,
 * allocation, reference transfer or graph/module lock occurs.
 */
static int syscall_eagain_is_nonblocking_device(struct task *task,
                                                uint64_t number, uint64_t fd);

/** @brief Match only explicitly selected executable paths; tracing is disabled by default. */
static bool syscall_trace_task(const struct task *task);

/** @brief Pin a retryable file operation's description across waits and signals.
 * @param task Calling task under execution ownership, or NULL.
 * @param number Linux syscall number.
 * @param fd Descriptor argument as signed low-32 bits; mmap uses argument five.
 * @param arg2 Third syscall argument for finit_module flag preflight, or mmap
 * fourth-argument flags so anonymous mappings ignore their numeric descriptor.
 * @return Zero or negative authorization/validation/allocation errno. A matching
 * retry keeps the same object even if another thread closes/reuses fd; task_release_syscall_file ends ownership.
 */
static int task_begin_syscall_io(struct task *task, uint64_t number, int32_t fd,
                                  uint64_t arg2);

static char syscall_trace_prefix[128];
/** @brief Distinguish a nonblocking stream result from an internal I/O wait.
 * @param task Current task under execution ownership, nullable.
 * @param number Native Linux syscall number.
 * @param fd First argument interpreted as the admitted original descriptor.
 * @return One for userspace-visible EAGAIN, zero for a retryable storage wait.
 * Regular files ignore O_NONBLOCK; sendfile tests its output stream and
 * copy_file_range never turns disk waits into nonblocking errors. No callback,
 * allocation, reference transfer or graph/module lock occurs.
 */
static int syscall_eagain_is_nonblocking_device(struct task *task,
                                                uint64_t number, uint64_t fd)
{
    struct task_pty_fd *endpoint;
    struct task_file *file;
    if(number==LINUX_SYS_COPY_FILE_RANGE)return 0;
    if(number==LINUX_SYS_SENDFILE) {
        file=task_file_for_io(task,(int)fd);
        return file && (file->flags&RELIEFOS_O_NONBLOCK) &&
            (file->flags&(TASK_FILE_FLAG_PIPE|TASK_FILE_FLAG_SOCKET_UNIX|TASK_FILE_FLAG_SOCKET_INET));
    }
    if (number == LINUX_SYS_RECV || number == LINUX_SYS_RECVFROM ||
        number == LINUX_SYS_RECVMSG || number == LINUX_SYS_SEND ||
        number == LINUX_SYS_SENDTO || number == LINUX_SYS_SENDMSG ||
        number == LINUX_SYS_ACCEPT || number == LINUX_SYS_ACCEPT4 ||
        number == LINUX_SYS_CONNECT) {
        /* The dedicated socket calls report EAGAIN for genuinely nonblocking
         * descriptors; the rewind path below would otherwise park the caller
         * inside the kernel until data or space appears, silently disabling
         * every user-space poll/deadline loop built on these calls. */
        file = task_file_for_io(task, (int)fd);
        return file && (file->flags & RELIEFOS_O_NONBLOCK) ? 1 : 0;
    }
    if (number != LINUX_SYS_READ && number != LINUX_SYS_WRITE &&
        number != LINUX_SYS_PREAD64 &&
        number != LINUX_SYS_READV && number != LINUX_SYS_WRITEV &&
        number != LINUX_SYS_PREADV && number != LINUX_SYS_PWRITEV &&
        number != LINUX_SYS_PREADV2 && number != LINUX_SYS_PWRITEV2) {
        return 0;
    }
    endpoint = task_pty_for_io(task, (int)fd);
    if (endpoint) {
        return (task_pty_status(endpoint) & RELIEFOS_O_NONBLOCK) != 0;
    }
    file = task_file_for_io(task, (int)fd);
    /* DRM event reads use EAGAIN as the userspace-visible empty-queue result
     * for O_NONBLOCK. Positional scalar/vector reads enter the same read
     * operation, so they must escape the storage retry loop as well. Keep
     * this scoped to reads; DRM ioctl EAGAIN may have other meanings. */
    if (file && (number == LINUX_SYS_READ || number == LINUX_SYS_READV ||
                 number == LINUX_SYS_PREAD64 || number == LINUX_SYS_PREADV ||
                 number == LINUX_SYS_PREADV2) &&
        file->drm_file &&
        (file->flags & RELIEFOS_O_NONBLOCK)) {
        return 1;
    }
    if (file && (file->flags & RELIEFOS_O_NONBLOCK)) {
        if (file->flags & (TASK_FILE_FLAG_PIPE |
                           TASK_FILE_FLAG_SOCKET_UNIX |
                           TASK_FILE_FLAG_SOCKET_INET |
                           TASK_FILE_FLAG_EVENTFD)) {
            return 1;
        }
        if (file->kind == TASK_FILE_KIND_SIGNALFD) return 1;
        if ((file->flags & TASK_FILE_FLAG_DEV_NODE) &&
            (task_device_is(file, STORAGE_DEV_KIND_KEYBOARD) ||
             task_device_is(file, STORAGE_DEV_KIND_MOUSE) ||
             task_device_is(file, STORAGE_DEV_KIND_CONSOLE) ||
             task_device_is(file, STORAGE_DEV_KIND_AUDIO) ||
             (file->node.flags & (STORAGE_NODE_FLAG_AUDIO_CONTROL |
                                  STORAGE_NODE_FLAG_AUDIO_TIMER)))) {
            return 1;
        }
    }
    return 0;
}

/** @brief Parse the optional boot syscall-trace=/path/prefix diagnostic filter. */
void syscall_trace_configure(const char *cmdline)
{
    static const char option[] = "syscall-trace=";
    syscall_trace_prefix[0] = 0;
    for (const char *p = cmdline; p && *p; ++p) {
        if (p != cmdline && p[-1] != ' ') continue;
        unsigned i = 0;
        while (option[i] && p[i] == option[i]) ++i;
        if (option[i]) continue;
        p += i;
        for (i = 0; p[i] && p[i] != ' ' && i + 1 < sizeof(syscall_trace_prefix); ++i)
            syscall_trace_prefix[i] = p[i];
        syscall_trace_prefix[i] = 0;
        return;
    }
}

/** @brief Match only explicitly selected executable paths; tracing is disabled by default. */
static bool syscall_trace_task(const struct task *task)
{
    if (!task || !syscall_trace_prefix[0]) return false;
    for (unsigned i = 0; syscall_trace_prefix[i]; ++i)
        if (task->path[i] != syscall_trace_prefix[i]) return false;
    return true;
}

/** @brief Pin a retryable file operation's description across waits and signals.
 * @param task Calling task under execution ownership, or NULL.
 * @param number Linux syscall number.
 * @param fd Descriptor argument as signed low-32 bits; mmap uses argument five.
 * @param arg2 Third syscall argument for finit_module flag preflight, or mmap
 * fourth-argument flags so anonymous mappings ignore their numeric descriptor.
 * @return Zero or negative authorization/validation/allocation errno. A matching
 * retry keeps the same object even if another thread closes/reuses fd; task_release_syscall_file ends ownership.
 */
static int task_begin_syscall_io(struct task *task, uint64_t number, int32_t fd,
                                  uint64_t arg2)
{
    bool file_io = (number==LINUX_SYS_MMAP && !((uint32_t)arg2&LINUX_MAP_ANONYMOUS)) ||
        number == LINUX_SYS_READ || number == LINUX_SYS_WRITE ||
        number == LINUX_SYS_FINIT_MODULE ||
        number == LINUX_SYS_FSTAT || number == LINUX_SYS_FSTATFS ||
        number == LINUX_SYS_FCHMOD || number == LINUX_SYS_FCHOWN || number == LINUX_SYS_FCHDIR ||
        number == LINUX_SYS_FTRUNCATE || number == LINUX_SYS_FALLOCATE ||
        number == LINUX_SYS_GETDENTS || number == LINUX_SYS_GETDENTS64 ||
        number == LINUX_SYS_FSYNC || number == LINUX_SYS_FDATASYNC ||
        number == LINUX_SYS_SYNCFS || number == LINUX_SYS_LSEEK ||
        number == LINUX_SYS_UTIMENSAT || number == LINUX_SYS_FUTIMESAT ||
        number == LINUX_SYS_IOCTL ||
        number == LINUX_SYS_PREAD64 || number == LINUX_SYS_PWRITE64 ||
        number == LINUX_SYS_READV || number == LINUX_SYS_WRITEV ||
        number == LINUX_SYS_PREADV || number == LINUX_SYS_PWRITEV ||
        number == LINUX_SYS_PREADV2 || number == LINUX_SYS_PWRITEV2 ||
        number == LINUX_SYS_SENDFILE || number == LINUX_SYS_COPY_FILE_RANGE ||
        number == LINUX_SYS_SENDTO || number == LINUX_SYS_RECVFROM ||
        number == LINUX_SYS_SENDMSG || number == LINUX_SYS_RECVMSG ||
        number == LINUX_SYS_SENDMMSG || number == LINUX_SYS_RECVMMSG ||
        number == LINUX_SYS_ACCEPT || number == LINUX_SYS_ACCEPT4 ||
        number == LINUX_SYS_BIND || number == LINUX_SYS_CONNECT;
    /* Metadata, directory and size operations can also wait for backing I/O.
     * Hold their OFD before the first callback, exactly as scalar data I/O. */
    if (number == LINUX_SYS_RT_SIGRETURN) return 0;
    if (task && (task->syscall_file || task->syscall_pty.used) &&
        (!file_io || task->syscall_fd != fd || task->syscall_file_number != number))
        task_release_syscall_file(task);
    /* An unrelated suspended call must retire before eligibility is checked:
     * its old OFD can have the same numeric fd as a newly opened module file.
     * A genuine finit retry retains the original object across close/reuse. */
    if (number == LINUX_SYS_FINIT_MODULE) {
        int rc=syscall_module_file_prepare((uint64_t)(uint32_t)fd,arg2);
        if (rc) return rc;
    }
    if (!task || !file_io || task->syscall_file || task->syscall_pty.used) return 0;
    struct task_file *file = task_file_for_fd(task, fd);
    if (file) {
        task->syscall_file = task_file_get(file);
        if (!task->syscall_file) return -RELIEFOS_ENOMEM;
        task->syscall_fd = fd;
        task->syscall_file_number = (uint32_t)number;
    } else if (task_pty_fd_for_fd(task, fd) || task_pty_stream_for_fd(task, fd) >= 0) {
        struct task_pty_fd *entry;
        int ret = task_pty_ensure_fd(task, fd, &entry);
        if (ret) return ret;
        task->syscall_pty = *entry;
        ++entry->description->references;
        task->syscall_fd = fd;
        task->syscall_file_number = (uint32_t)number;
    }
    return 0;
}

/**
 * @brief Dispatch one int 0x80 request while serializing kernel execution.
 * @param frame Validated syscall frame, updated with the result or restart state.
 * @return A task for direct user return, or NULL when scheduler handling is required.
 * @note Bounded TCP maintenance runs under the execution transaction; the
 *       tick gate limits packet polling to one small batch per timer tick.
 * Path operands retain their checked copy and exact directory references over
 * pre-I/O retries; final return, signal handoff or replacement cancels ownership.
 */
struct task *syscall_dispatch_frame(struct trap_frame *frame)
{
    uint64_t number;
    uint64_t lock_flags;
    int64_t result;
    int restored_signal_frame = 0;
    int eagain_from_nonblocking = 0;
    if (!frame) {
        return NULL;
    }
    /* Several storage and GUI services still own global scratch buffers. */
    kernel_execution_lock_irqsave(&lock_flags);
    net_tcp_maintenance();
    /* Drain small fair batches after taking execution ownership. Failed
     * filesystem cleanup retains its provider until a later successful flush;
     * final DRM close queues work, including closes from signal return. */
    vfs_reclaim_mounts(2);
    graphics_service_process_retirements(2);
    driver_manager_process_audio_faults();
    input_process_pending();
    number = frame->rax;
    struct task *calling_task = sched_current_task();
    bool trace = syscall_trace_task(calling_task);
    if (trace) console_debug("[syscall-trace] pid=%u nr=%llu args=%llx,%llx,%llx,%llx,%llx,%llx\n",
        calling_task->pid, (unsigned long long)number,
        (unsigned long long)frame->rdi, (unsigned long long)frame->rsi,
        (unsigned long long)frame->rdx, (unsigned long long)frame->r10,
        (unsigned long long)frame->r8, (unsigned long long)frame->r9);
    if (calling_task && number != LINUX_SYS_RT_SIGRETURN) {
        if (calling_task->sysv_sem.ops && calling_task->sysv_sem.number != number)
            task_sysv_sem_cancel(calling_task);
        if (calling_task->sysv_msg.queue && calling_task->sysv_msg.number != number)
            task_sysv_msg_cancel(calling_task);
        if (calling_task->fifo_open_file && number != LINUX_SYS_OPEN && number != LINUX_SYS_OPENAT && number != LINUX_SYS_OPENAT2)
            task_fifo_cancel(calling_task);
        calling_task->restart_syscall = 0;
        /* sigaltstack must inspect the live user SP, not the last timer frame. */
        calling_task->frame.rsp = frame->rsp;
    }
    task_transfer_enter(calling_task,frame);
    task_vector_enter(calling_task,frame);
    int pin_error=task_begin_syscall_io(calling_task,number,
        (int32_t)(number==LINUX_SYS_MMAP ? frame->r8 : frame->rdi),
        number==LINUX_SYS_MMAP ? frame->r10 : frame->rdx);
    task_lookup_scope_begin(calling_task,frame);
    storage_set_io_async_context(true);
    if (pin_error) {
        result = pin_error;
    } else if (number == LINUX_SYS_RT_SIGRETURN) {
        struct task *task = sched_current_task();
        result = task ? kernel_signal_rt_sigreturn(task, frame) : -RELIEFOS_EPERM;
        restored_signal_frame = result >= 0;
    } else if (number == LINUX_SYS_CLONE) {
        /* Legacy clone() only consumes the low 32 flag bits; Linux silently
         * ignores the upper half rather than returning EINVAL. */
        result = sched_clone_current(frame, (uint32_t)frame->rdi, frame->rsi,
                                     frame->rdx, frame->r10, frame->r8);
    } else if (number == LINUX_SYS_CLONE3) {
        result = syscall_clone3(frame, frame->rdi, frame->rsi);
    } else if (number == LINUX_SYS_VFORK) {
        result = sched_vfork_current(frame);
    } else if (number == LINUX_SYS_FORK) {
        result = sched_fork_current(frame);
    } else {
        result = RELIEFOS_SYSCALL_FRAME_DISPATCH(calling_task, number,
                                       frame->rdi,
                                       frame->rsi,
                                       frame->rdx,
                                       frame->r10,
                                       frame->r8,
                                       frame->r9);
    }
    bool module_file_io_retry=number==LINUX_SYS_FINIT_MODULE && result==MODULE_FILE_IO_RETRY;
    if(module_file_io_retry) result=-RELIEFOS_EAGAIN;
    if ((number == LINUX_SYS_EXECVE || number == LINUX_SYS_EXECVEAT) && result == 0 && calling_task) {
        *frame = calling_task->frame;
        /* The fresh image frame is not a SYSCALL stub frame; drop the
         * SYSRET-eligible marker so the return keeps live rcx/r11 intact. */
        frame->error = 0;
        restored_signal_frame = 1;
    }
    if (trace) console_debug("[syscall-trace] pid=%u nr=%llu result=%lld\n",
        calling_task->pid, (unsigned long long)number, (long long)result);
    eagain_from_nonblocking =
        result == -RELIEFOS_EAGAIN &&
        syscall_eagain_is_nonblocking_device(sched_current_task(),
                                             number, frame->rdi);
    if (result == -LINUX_EAGAIN && calling_task && calling_task->syscall_file &&
        calling_task->syscall_file->kind == TASK_FILE_KIND_SIGNALFD) eagain_from_nonblocking = 1;
    if ((number == LINUX_SYS_FUTEX || number == LINUX_SYS_FUTEX_WAIT ||
         number == LINUX_SYS_FUTEX_WAITV ||
         number == LINUX_SYS_RT_SIGQUEUEINFO || number == LINUX_SYS_RT_TGSIGQUEUEINFO ||
         number == LINUX_SYS_RT_SIGTIMEDWAIT ||
         number == LINUX_SYS_MSGSND ||
         number == LINUX_SYS_SEMOP || number == LINUX_SYS_SEMTIMEDOP ||
         number == LINUX_SYS_TKILL || number == LINUX_SYS_TGKILL ||
         number == LINUX_SYS_SENDMMSG || number == LINUX_SYS_RECVMMSG ||
         number == LINUX_SYS_FUTEX_REQUEUE || number == LINUX_SYS_CLONE ||
         number == LINUX_SYS_CLONE3 || number == LINUX_SYS_FORK ||
         number == LINUX_SYS_VFORK) && result == -RELIEFOS_EAGAIN) {
        eagain_from_nonblocking = 1;
    }
    /* A deferred RLIMIT_NPROC exec rejection is final, unlike storage I/O. */
    if ((number == LINUX_SYS_EXECVE || number == LINUX_SYS_EXECVEAT) && result == -RELIEFOS_EAGAIN &&
        calling_task && calling_task->nproc_exceeded)
        eagain_from_nonblocking = 1;
    if (number == LINUX_SYS_FCNTL && (uint32_t)frame->rsi == LINUX_F_SETLK && result == -RELIEFOS_EAGAIN)
        eagain_from_nonblocking = 1;
    /* Inactive VT presentation must return to the compositor, allowing its
     * loop to observe the switch and request a complete repaint on return. */
    if (number == LINUX_SYS_IOCTL && result == -RELIEFOS_EAGAIN &&
        ((uint32_t)frame->rsi == RELIEFOS_FBIOBLIT ||
         (uint32_t)frame->rsi == FBIOPUT_VSCREENINFO))
        eagain_from_nonblocking = 1;
    /* LOCK_NB conflicts are final even on blocking, readonly descriptions.
     * EWOULDBLOCK aliases EAGAIN; retrying it here deadlocks session readers
     * while the PAM owner correctly keeps its marker locked. */
    if (number == LINUX_SYS_FLOCK && ((uint32_t)frame->rsi & RELIEFOS_FLOCK_NB) &&
        result == -RELIEFOS_EWOULDBLOCK)
        eagain_from_nonblocking = 1;
    /* A module's external users must release it from userspace. Rewinding
     * delete_module on EWOULDBLOCK would park the very owner that must close
     * its device or mapping, because Linux aliases that error to EAGAIN. */
    /* Initializers may also return a final Linux EAGAIN after side effects and
     * rollback. Only the explicit pre-init file token is restartable; never
     * replay a module initializer to satisfy the legacy storage retry path. */
    if ((number==LINUX_SYS_INIT_MODULE || number==LINUX_SYS_FINIT_MODULE ||
         number==LINUX_SYS_DELETE_MODULE) && result==-RELIEFOS_EWOULDBLOCK &&
        !module_file_io_retry)
        eagain_from_nonblocking = 1;
    if (result == -RELIEFOS_EAGAIN && calling_task && calling_task->socket_io_timed &&
        calling_task->socket_io_deadline <= time_ticks()) eagain_from_nonblocking = 1;
    if (result == -RELIEFOS_EAGAIN &&
        (((number == LINUX_SYS_SENDMSG || number == LINUX_SYS_RECVMSG) && (frame->rdx & 0x40)) ||
         ((number == LINUX_SYS_SENDTO || number == LINUX_SYS_RECVFROM) && (frame->r10 & 0x40))))
        eagain_from_nonblocking = 1;
    if (result != -RELIEFOS_EAGAIN || eagain_from_nonblocking) {
        storage_release_task_io(sched_current_pid());
    }
    storage_set_io_async_context(false);
    if (restored_signal_frame) {
        /* sigreturn and successful exec already supplied the user context. */
    } else if (result == KERNEL_SYSCALL_BLOCKED ||
               (result == -RELIEFOS_EAGAIN && !eagain_from_nonblocking)) {
        /* int $0x80 has advanced RIP by two bytes.  Park this task for one
         * timer tick and re-execute the exact same instruction when its AHCI
         * DMA request can be polled again.  User programs keep normal
         * blocking read/open/stat semantics and never observe EAGAIN. */
        frame->rax = number;
        frame->rip -= 2u;
        if (calling_task) calling_task->restart_syscall = number + 1;
        if (result != KERNEL_SYSCALL_BLOCKED)
            sched_sleep_current_until(time_ticks() + 1u);
        kernel_execution_unlock_irqrestore(lock_flags);
        return NULL;
    }
    if (!restored_signal_frame) {
        frame->rax = (uint64_t)result;
    }
    task_release_syscall_file(calling_task);
    /* Complete an unchanged task's return inside the transaction that already
     * pins its lifetime. The scheduler still checks signals, preemption, CPU
     * ownership, affinity and the frame under scheduler_lock. Drain input
     * arriving during the syscall before that check. Rejected returns keep
     * the full signal/retirement/selection path in the architecture entry. */
    input_process_pending();
    struct task *resume = NULL;
    if (sched_try_resume_current_user_frame(frame)) {
        resume = sched_current_task();
        arch_set_user_fs(resume->fs_base);
    }
    kernel_execution_unlock_irqrestore(lock_flags);
    return resume;
}
