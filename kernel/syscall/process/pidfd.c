/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"
#include <reliefnt/pidfd.h>
#include <reliefnt/swap.h>
#include <linux/errno.h>
#include <linux/pidfd.h>
#include <linux/resource.h>
#include <linux/wait.h>

/** @brief Resolve an admitted file description to its retained identity.
 * @param file Nullable description borrowed under execution ownership.
 * @return Borrowed PID object for a pidfd, otherwise NULL.
 */
static struct task_identity *pidfd_identity(const struct task_file *file)
{
    return file && file->kind == TASK_FILE_KIND_PIDFD ? (void *)(uintptr_t)file->aux : NULL;
}

/** @brief Open a close-on-exec descriptor retaining one stable PID object.
 * @param caller Calling task under execution ownership. @param pid Positive PID or TID.
 * @param flags Linux pidfd_open flags. @return New descriptor or negative Linux errno.
 */
int64_t syscall_pidfd_open(struct task *caller, int32_t pid, uint32_t flags)
{
    if (pid <= 0 || (flags & ~(PIDFD_NONBLOCK | PIDFD_THREAD))) return -LINUX_EINVAL;
    if (!caller) return -LINUX_ESRCH;
    struct task *target = sched_find((uint32_t)pid);
    if (!target || target->kind != TASK_KIND_USER) return -LINUX_ESRCH;
    bool thread = (flags & PIDFD_THREAD) != 0;
    bool leader = target->pid == sched_task_tgid(target);
    if (target->state == TASK_EXITED &&
        ((leader && !(target->flags & TASK_FLAG_WAITABLE_CHILD)) ||
         (!leader && (target->flags & TASK_FLAG_RESOURCES_RELEASED)))) return -LINUX_ESRCH;
    if (!thread && !leader) return -LINUX_EINVAL;

    struct task_identity *identity;
    struct task_file *file;
    uint64_t ino;
    int rc = sched_task_pidfd_get(target, &identity, &ino);
    if (rc) return rc;
    int fd = task_allocate_fd(caller, 0, &file);
    if (fd < 0) {
        sched_task_pidfd_put(identity);
        return fd;
    }
    file->kind = TASK_FILE_KIND_PIDFD;
    file->flags = LINUX_O_RDWR | flags;
    file->fd_flags = LINUX_FD_CLOEXEC;
    __builtin_memcpy(file->path, "anon_inode:[pidfd]", sizeof("anon_inode:[pidfd]"));
    file->aux = (uint64_t)(uintptr_t)identity;
    file->aux2 = ino;
    return fd;
}

/** @brief Recheck authorization and signal the identity pinned by an OFD.
 * @param caller Calling task under execution ownership. @param fd Its pidfd.
 * @param sig Linux signal number, or zero for a permission/existence check.
 * @param pointer Optional user siginfo address. @param flags Linux signal scope.
 * @return Zero, negative Linux errno, or a private pre-enqueue retry result.
 */
int64_t syscall_pidfd_send_signal(struct task *caller, int fd, int sig,
                                 uint64_t pointer, uint32_t flags)
{
    if (flags && flags != PIDFD_SIGNAL_THREAD && flags != PIDFD_SIGNAL_THREAD_GROUP)
        return -LINUX_EINVAL;
    const struct task_file *file = task_file_for_io(caller, fd);
    struct task_identity *identity = pidfd_identity(file);
    if (!identity) return -LINUX_EBADF;
    if (sig < 0 || sig >= LINUX_NSIG) return -LINUX_EINVAL;
    const bool thread = flags == PIDFD_SIGNAL_THREAD || (!flags && (file->flags & PIDFD_THREAD));

    struct linux_siginfo info = {.signo = sig, .code = thread ? LINUX_SI_TKILL : LINUX_SI_USER,
        .fields.sender = {.pid = (int32_t)sched_task_tgid(caller), .uid = caller->uid}};
    if (pointer) {
        int rc = kernel_signal_import_info(caller, pointer, sig, &info);
        if (rc) return rc;
        if (info.signo != sig) return -LINUX_EINVAL;
        if (sched_task_identity_task(identity) != caller &&
            (info.code >= 0 || info.code == LINUX_SI_TKILL)) return -LINUX_EPERM;
    }

    struct task *target = sched_task_identity_task(identity);
    if (!target || sched_task_pidfd_state(identity, thread) == SCHED_PIDFD_REAPED)
        return -LINUX_ESRCH;
    if (!kernel_signal_may_send(caller, target, sig)) return -LINUX_EPERM;
    if (!sig) return 0;
    /* Linux protects the initial namespace's init from unhandled user
     * signals, including SIGKILL and SIGSTOP. Do not enter ReliefNT's
     * legacy numeric kill/power path through this new handle API. */
    if (sched_task_tgid(target) == 1 && sched_task_actions(target)[sig].handler <= 1)
        return 0;
    if (!thread) return sched_signal_user_process_target(target, sig, &info);
    return target->state == TASK_EXITED ? 0 : kernel_signal_queue_task_info(target, sig, &info);
}

/** @brief Report exit/reap readiness without consuming the child event.
 * @param file Borrowed pidfd description under execution ownership.
 * @return Linux poll event mask; zero while the observed target is live.
 */
short task_pidfd_poll(const struct task_file *file)
{
    const struct task_identity *identity = pidfd_identity(file);
    switch (sched_task_pidfd_state(identity, (file->flags & PIDFD_THREAD) != 0)) {
    case SCHED_PIDFD_EXITED: return POLLIN | POLLRDNORM;
    case SCHED_PIDFD_REAPED: return POLLIN | POLLRDNORM | POLLHUP;
    default: return 0;
    }
}

/** @brief Prepare an optional result for swap I/O before consuming a child event.
 * @param caller Calling task under execution ownership.
 * @param address User result destination, zero when omitted. @param size Result bytes.
 * @return True when waiting for swap I/O; ordinary copy errors are reported after wait.
 */
static bool pidfd_wait_output_pending(struct task *caller, uint64_t address, uint64_t size)
{
    return address && swap_prepare_user_range(caller, address, size, true) == SWAP_STORAGE_CONTENDED;
}

/** @brief Observe or consume a child wait event using a pinned PID identity.
 * @param caller Calling task under execution ownership. @param fd Its pidfd.
 * @param infop Optional user siginfo address. @param usage_ptr Optional raw ABI rusage address.
 * @param options Linux waitid event selection and flags.
 * @return Zero, negative Linux errno, or a private blocking/retry result.
 */
int64_t syscall_pidfd_waitid(struct task *caller, int fd, uint64_t infop, uint64_t usage_ptr,
                             uint32_t options)
{
    const uint32_t events = LINUX_WSTOPPED | LINUX_WEXITED | LINUX_WCONTINUED;
    if (fd < 0 || !(options & events) ||
        (options & ~(events | LINUX_WNOHANG | LINUX_WNOWAIT |
                     LINUX_WNOTHREAD | LINUX_WALL | LINUX_WCLONE))) return -LINUX_EINVAL;
    const struct task_file *file = task_file_for_io(caller, fd);
    const struct task_identity *identity = pidfd_identity(file);
    if (!caller || !identity) return -LINUX_EBADF;

    /* An output that needs storage I/O must leave the event available across
     * a restart. A permanent copy fault, just like Linux, does not undo a
     * consuming wait. The OFD remains pinned across retries. */
    struct linux_siginfo info = {0};
    struct linux_rusage usage;
    struct linux_rusage *usage_out = usage_ptr ? &usage : NULL;
    bool peek_only = (options & LINUX_WNOWAIT) != 0;
    uint64_t generation = kernel_poll_readiness_generation();
    int64_t result = sched_wait_identity(caller->pid, identity, options | LINUX_WNOWAIT,
                                        &info, peek_only ? usage_out : NULL);
    if (result == -LINUX_EAGAIN) {
        if (!(options & LINUX_WNOHANG) && !(file->flags & LINUX_O_NONBLOCK)) {
            if (kernel_poll_wait_current(generation, time_ticks() + 1) >= 0)
                return KERNEL_SYSCALL_BLOCKED;
            return KERNEL_SYSCALL_SWAP_CONTENDED;
        }
        info = (struct linux_siginfo){0};
        if (options & LINUX_WNOHANG) result = 0;
    } else if (result == 0) {
        result = -LINUX_ECHILD;
    }
    if ((result > 0 && pidfd_wait_output_pending(caller, usage_ptr, sizeof(usage))) ||
        pidfd_wait_output_pending(caller, infop, sizeof(info)))
        return KERNEL_SYSCALL_SWAP_CONTENDED;
    if (result > 0 && !peek_only) {
        result = sched_wait_identity(caller->pid, identity, options, &info, usage_out);
        if (result <= 0) {
            info = (struct linux_siginfo){0};
            if (result == 0) result = -LINUX_ECHILD;
            else if (result == -LINUX_EAGAIN) return KERNEL_SYSCALL_SWAP_CONTENDED;
        }
    }
    if (result > 0 && usage_ptr) {
        if (!user_range_writable(usage_ptr, sizeof(usage))) return -LINUX_EFAULT;
        __builtin_memcpy((void *)(uintptr_t)usage_ptr, &usage, sizeof(usage));
    }
    if (infop) {
        /* Native waitid copies only the six defined fields. Leave the union
         * padding and unused members untouched, like Linux copyout. */
        struct linux_siginfo *out = (void *)(uintptr_t)infop;
        if (!user_range_writable(infop, sizeof(info))) return -LINUX_EFAULT;
        out->signo = info.signo;
        out->error = info.error;
        out->code = info.code;
        out->fields.child.pid = info.fields.child.pid;
        out->fields.child.uid = info.fields.child.uid;
        out->fields.child.status = info.fields.child.status;
    }
    return result > 0 ? 0 : result;
}
