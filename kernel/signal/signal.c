/*
 * ReliefOS kernel signal delivery: installs and restores the user rt_sigframe
 * used by rt_sigaction/rt_sigprocmask/rt_sigreturn.
 */
#include <reliefnt/signal.h>
#include <reliefnt/sched.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/paging.h>
#include <reliefnt/swap.h>
#ifdef CONFIG_SWAP
#include <reliefnt/swap_internal.h>
#endif

#include <reliefnt/arch.h>
#include <reliefnt/smp.h>
#include <reliefnt/futex.h>
#include <reliefnt/wait.h>
#include <reliefnt/lock.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <linux/signal.h>
#include <linux/syscall.h>
#include <linux/time.h>
#include <linux/errno.h>
#include <linux/futex.h>
#include <reliefnt/time.h>
#include <reliefnt/power.h>
#include <reliefnt/storage.h>
#ifdef CONFIG_SWAP
/** @brief Cancel an interrupted fork's scalar-only private reclaim state. */
extern void sched_fork_reclaim_cancel(struct task *) __attribute__((weak));
#endif

static struct kernel_signal_action *signal_action(struct task *task, int sig)
{
    if (!task || sig <= 0 || sig >= LINUX_NSIG) return NULL;
    return &sched_task_actions(task)[sig];
}

static void signal_context_save(struct linux_sigcontext *context,
                                const struct trap_frame *frame)
{
    context->r15 = frame->r15;
    context->r14 = frame->r14;
    context->r13 = frame->r13;
    context->r12 = frame->r12;
    context->r11 = frame->r11;
    context->r10 = frame->r10;
    context->r9 = frame->r9;
    context->r8 = frame->r8;
    context->rbp = frame->rbp;
    context->rdi = frame->rdi;
    context->rsi = frame->rsi;
    context->rdx = frame->rdx;
    context->rcx = frame->rcx;
    context->rbx = frame->rbx;
    context->rax = frame->rax;
    context->vector = frame->vector;
    context->error = frame->error;
    context->rip = frame->rip;
    context->cs = frame->cs;
    context->rflags = frame->rflags;
    context->rsp = frame->rsp;
    context->ss = frame->ss;
}

static void signal_context_restore(struct trap_frame *frame,
                                   const struct linux_sigcontext *context)
{
    frame->r15 = context->r15;
    frame->r14 = context->r14;
    frame->r13 = context->r13;
    frame->r12 = context->r12;
    frame->r11 = context->r11;
    frame->r10 = context->r10;
    frame->r9 = context->r9;
    frame->r8 = context->r8;
    frame->rbp = context->rbp;
    frame->rdi = context->rdi;
    frame->rsi = context->rsi;
    frame->rdx = context->rdx;
    frame->rcx = context->rcx;
    frame->rbx = context->rbx;
    frame->rax = context->rax;
    frame->vector = context->vector;
    frame->error = context->error;
    frame->rip = context->rip;
    frame->cs = 0x23;
    /* User context may change arithmetic flags, TF, DF, RF and AC, never IOPL. */
    frame->rflags = (context->rflags & 0x50dd5ULL) | 0x202ULL;
    frame->rsp = context->rsp;
    frame->ss = 0x1b;
}

static void signal_default_action(struct task *task, int sig)
{
    if (!task || task->state == TASK_EXITED) return;
    /* BusyBox reboot/poweroff follows the traditional init protocol and
     * signals PID 1 after OpenRC has stopped its services.  PID 1 is
     * deliberately non-terminable in ReliefNT; translate those two power
     * signals into the machine transition instead of attempting sched_exit. */
    if (task->pid == 1 && (sig == 15 || sig == 12)) {
        if (sig == 15) power_reboot();
        power_shutdown();
    }
    switch (sig) {
    case 19: /* SIGSTOP */
    case 20: /* SIGTSTP */
    case 21: /* SIGTTIN */
    case 22: /* SIGTTOU */
        sched_signal_job_control(sched_task_tgid(task), sig);
        return;
    case 18: /* SIGCONT */
        sched_signal_job_control(sched_task_tgid(task), sig);
        return;
    case 23: /* SIGURG  */
    case 17: /* SIGCHLD */
    case 28: /* SIGWINCH */
        return;
    default:
        task->exit_signal = (uint32_t)sig;
        {
            struct task *leader = sched_find(sched_task_tgid(task));
            if (leader) leader->exit_signal = (uint32_t)sig;
            sched_exit_group(sched_task_tgid(task), (uint64_t)(128 + sig));
        }
        return;
    }
}

int kernel_signal_set_action(struct task *task, int signal_number,
                             uint64_t handler, uint64_t mask, uint32_t flags,
                             uint64_t restorer,
                             struct kernel_signal_action *previous)
{
    struct kernel_signal_action *slot;
    if (!task || task->kind != TASK_KIND_USER || task->state == TASK_EXITED ||
        signal_number <= 0 || signal_number >= LINUX_NSIG || signal_number == 9 ||
        signal_number == 19 || (handler != 0 && handler != 1 && !restorer)) {
        return -1;
    }
    slot = signal_action(task, signal_number);
    if (!slot) return -1;
    if (previous) *previous = *slot;
    slot->handler = handler;
    slot->mask = mask & KERNEL_SIGNAL_VALID_MASK;
    slot->flags = flags;
    slot->restorer = restorer;
    slot->reserved = 0;
    if (handler == 1 || (!handler && kernel_signal_default_ignored(signal_number))) {
        if (handler == 1) task->ignored_signals |= 1ULL << (uint32_t)(signal_number - 1);
        else task->ignored_signals &= ~(1ULL << (uint32_t)(signal_number - 1));
        sched_signal_discard(task, signal_number);
    } else {
        task->ignored_signals &= ~(1ULL << (uint32_t)(signal_number - 1));
    }
    return 0;
}

int kernel_signal_queue_task(struct task *task, int signal_number)
{
    return kernel_signal_queue_task_info(task, signal_number, NULL);
}

/**
 * @brief Queue siginfo or apply a default action to one live user thread.
 * @param task Target thread; its lifetime must be held by the caller.
 * @param signal_number Linux signal number, including zero for an existence probe.
 * @param info Native signal payload, or NULL for a kernel-generated signal.
 * @return Zero on success, negative errno for invalid target or exhausted RT queue.
 */
int kernel_signal_queue_task_info(struct task *task, int signal_number,
                                  const struct linux_siginfo *info)
{
    struct kernel_signal_action *action;
    uint64_t bit;
    if (!task || task->pid == 0 || task->kind != TASK_KIND_USER ||
        task->state == TASK_EXITED || signal_number < 0 || signal_number >= LINUX_NSIG) {
        return -1;
    }
    if (signal_number == 0) return 0;
    bit = 1ULL << (uint32_t)(signal_number - 1);
    action = signal_action(task, signal_number);
    if (signal_number == 18) signal_default_action(task, signal_number);
    if (signal_number >= 19 && signal_number <= 22)
        sched_signal_discard(task, 18);

    if (signal_number != 9 && signal_number != 19 &&
        !((task->blocked_signals | task->sigwait_mask) & bit) && action &&
        (action->handler == 1 || (!action->handler && kernel_signal_default_ignored(signal_number)))) {
        return 0;
    }

    int result = kernel_signal_enqueue(task, false, signal_number, info);
    if (result < 0) return result;
    if (task->signalfd_waiting && task->state == TASK_BLOCKED && !sched_vfork_waiting(task)) {
        task->wake_tick = 0;
        task->state = TASK_READY;
        sched_notify_runnable();
    }
    if (signal_number != 9 && signal_number != 19 &&
        (task->blocked_signals & bit) && !(task->sigwait_mask & bit)) {
        return 0;
    }
    if (signal_number != 9 && signal_number != 19 &&
        ((task->sigwait_mask & bit) || (action && action->handler > 1))) {
        /* Linux wait_for_vfork_done() uses TASK_KILLABLE: a caught or blocked
         * non-fatal signal stays pending while the parent waits for the
         * child's exec/exit.  The completion path wakes the parent; the
         * normal return-to-user path then delivers the handler. */
        if (task->state == TASK_BLOCKED) {
            if (sched_vfork_waiting(task)) return 0;
            task->wake_tick = 0;
            task->state = TASK_READY;
            sched_notify_runnable();
        }
        return 0;
    }

    if (sched_vfork_waiting(task)) {
        int fatal = kernel_signal_fatal_pending(task);
        if (fatal) signal_default_action(task, fatal);
        return 0;
    }
    kernel_signal_flush(task, false, bit);
    signal_default_action(task, signal_number);
    return 0;
}

/** @brief Build a user handler frame and settle interrupted syscall progress.
 * @param task Current task with execution ownership and a stable destination MM.
 * @param sig Validated Linux signal number whose handler is being installed.
 * @param action Borrowed disposition, required with a user handler/restorer.
 * @param frame Saved user registers, updated only after a complete frame copy.
 * @param info Borrowed validated signal information for the native ABI frame.
 * @return Zero after installing the handler context, or negative input/stack/
 * usercopy errno. Interrupted copy/read/message progress survives cleanup;
 * restartable zero-progress calls retain their instruction when appropriate.
 * Drains and final resource puts require no scheduler/graph/module spinlock.
 */
static int signal_setup_frame(struct task *task, int sig,
                              struct kernel_signal_action *action,
                              struct trap_frame *frame, const struct linux_siginfo *info)
{
    if (!task || !frame || !action || !action->handler ||
        action->handler == 1 || !action->restorer) return -22;
    struct linux_rt_sigframe user_frame;
    uint64_t size = sizeof(user_frame);
    uint64_t base;
    uint64_t bit = 1ULL << (uint32_t)(sig - 1);
    uint64_t handler_mask = task->blocked_signals;
    uint8_t fpstate[RELIEFNT_FPU_STATE_MAX_SIZE + 4u] __attribute__((aligned(64)));
    uint32_t fpu_state_size = arch_fpu_state_size();
    uint32_t fpstate_size = fpu_state_size + (arch_fpu_xstate_mask() ? 4u : 0u);
    uint64_t fpbase;
    bool on_altstack = task->signal_stack_size && frame->rsp >= task->signal_stack_base &&
        frame->rsp - task->signal_stack_base < task->signal_stack_size;
    uint64_t stack_top = frame->rsp;
    if ((action->flags & LINUX_SA_ONSTACK) && task->signal_stack_size && !on_altstack)
        stack_top = task->signal_stack_base + task->signal_stack_size;

    /* Preserve the interrupted red zone and align the FXSAVE image separately
     * from the ABI's rsp % 16 == 8 function-entry frame. */
    if (stack_top < 128 + fpstate_size + size + 80) return -14;
    fpbase = (stack_top - 128 - fpstate_size) & ~63ULL;
    base = ((fpbase - size) & ~15ULL) - 8;
    if (!base || base < RELIEFNT_USER_BASE || base > stack_top) {
        return -12;
    }

    __builtin_memset(&user_frame, 0, size);
    user_frame.restorer = action->restorer;
    user_frame.uc.flags = 6; /* UC_SIGCONTEXT_SS | UC_STRICT_RESTORE_SS */
    user_frame.uc.stack.sp = task->signal_stack_base;
    user_frame.uc.stack.size = task->signal_stack_size;
    user_frame.uc.stack.flags = task->signal_stack_size
        ? task->signal_stack_flags | (on_altstack ? 1 : 0) : 2;
    user_frame.uc.mask = task->sigsuspend_active
                                ? task->sigsuspend_saved_mask
                                : task->blocked_signals;
    user_frame.info = *info;
    if (sched_rseq_notify(task, frame, smp_current_cpu(), true) < 0)
        return -LINUX_EFAULT;
    if (task->restart_syscall) {
        uint64_t nr = task->restart_syscall - 1;
#ifdef CONFIG_SWAP
        if (nr == __NR_fork && sched_fork_reclaim_cancel) sched_fork_reclaim_cancel(task);
#endif
        bool sleeping = nr == __NR_nanosleep || nr == __NR_clock_nanosleep;
        bool interruptible = task->fifo_open_file != NULL || nr == __NR_read || nr == __NR_write || nr == __NR_readv ||
            nr == __NR_pread64 || nr == __NR_pwrite64 ||
            nr == __NR_preadv || nr == __NR_preadv2 || nr == __NR_pwritev || nr == __NR_pwritev2 ||
            nr == __NR_writev || nr == __NR_sendfile || nr == __NR_copy_file_range ||
            nr == __NR_recvfrom || nr == __NR_sendto ||
            nr == __NR_recvmsg || nr == __NR_sendmsg || nr == __NR_accept ||
            nr == __NR_recvmmsg || nr == __NR_sendmmsg ||
            nr == __NR_accept4 || nr == __NR_connect || nr == __NR_futex || nr == __NR_futex_wait ||
            nr == __NR_futex_waitv || nr == __NR_poll || nr == __NR_ppoll || nr == __NR_epoll_wait ||
            nr == __NR_epoll_pwait || nr == __NR_epoll_pwait2 || nr == __NR_select || nr == __NR_pselect6 ||
            nr == __NR_wait4 || nr == __NR_waitid || nr == __NR_rt_sigtimedwait || nr == __NR_fcntl || nr == __NR_flock ||
            nr == __NR_ioctl ||
            nr == __NR_msgsnd || nr == __NR_msgrcv ||
            nr == __NR_semop || nr == __NR_semtimedop || sleeping;
        bool restart = (action->flags & LINUX_SA_RESTART) &&
            nr != __NR_msgsnd && nr != __NR_msgrcv &&
            nr != __NR_semop && nr != __NR_semtimedop &&
            nr != __NR_poll && nr != __NR_ppoll && nr != __NR_epoll_wait && nr != __NR_epoll_pwait &&
            nr != __NR_epoll_pwait2 && nr != __NR_select && nr != __NR_pselect6 && nr != __NR_rt_sigtimedwait && !sleeping &&
            !(nr == __NR_futex && frame->r10) && !task->socket_io_timed;
        uint32_t futex_op = (uint32_t)frame->rsi & FUTEX_CMD_MASK;
        if (nr == __NR_futex &&
            (futex_op == FUTEX_LOCK_PI || futex_op == FUTEX_LOCK_PI2))
            restart = true;
        uint64_t received = task->socket_receive_done;
        bool batch = task->mmsg.active;
#ifdef CONFIG_SWAP
        int socket_output_error=batch?0:task_socket_signal_finish(task);
#else
        int socket_output_error=0;
#endif
        int64_t batch_result = batch ? task_socket_mmsg_interrupt(task, received) : 0;
        received = task_socket_cancel_receive(task);
        if (task->regular_io.active) {
            received = task->regular_io.done;
        }
        uint64_t vector_received=task_vector_progress(task);
        if(vector_received)received=vector_received;
        int64_t transfer_result=task->transfer_io ? task_transfer_interrupt(task) : 0;
        if(transfer_result>0)received=(uint64_t)transfer_result;
        task_release_syscall_file(task);
        if (received || socket_output_error || transfer_result<0 || (batch && batch_result != -LINUX_EINTR) || (interruptible && !restart)) {
            frame->rip += 2;
            frame->rax = socket_output_error ? (uint64_t)socket_output_error : transfer_result<0 ? (uint64_t)transfer_result :
                batch ? (uint64_t)batch_result : received ? received : (uint64_t)-4LL;
            task->poll_deadline_ticks = 0;
        }
        if (sleeping) {
            struct linux_timespec clock_value;
            uint64_t now = task->nanosleep_deadline;
            if (!time_clock_get(task->nanosleep_clock, &clock_value)) {
                const uint64_t tick_ns = 1000000000ULL / RELIEFNT_TICK_HZ;
                now = (uint64_t)clock_value.tv_sec * RELIEFNT_TICK_HZ +
                    (uint64_t)clock_value.tv_nsec / tick_ns +
                    ((uint64_t)clock_value.tv_nsec % tick_ns != 0);
            }
            uint64_t ticks = task->nanosleep_deadline > now ? task->nanosleep_deadline - now : 0;
            struct linux_timespec remaining = {(int64_t)(ticks / RELIEFNT_TICK_HZ),
                (int64_t)((ticks % RELIEFNT_TICK_HZ) * (1000000000ULL / RELIEFNT_TICK_HZ))};
            if (task->nanosleep_remaining &&
                user_copy_to_task(task, task->nanosleep_remaining, &remaining, sizeof(remaining)) < 0)
                frame->rax = (uint64_t)-14LL;
            task->nanosleep_deadline = task->nanosleep_remaining = 0;
        }
        if (task->waiting_queue) kernel_wait_queue_remove(task->waiting_queue, task);
        syscall_record_lock_cancel(task);
        if (nr == __NR_futex || nr == __NR_futex_wait) futex_cancel_wait(task);
        if (nr == __NR_rt_sigtimedwait) {
            task->sigwait_deadline = task->sigwait_mask = 0;
            task->sigwait_active = 0;
        }
        task->restart_syscall = 0;
    }
    signal_context_save(&user_frame.uc.context, frame);
    user_frame.uc.context.cr2 = task->signal_fault_address;
    user_frame.uc.context.oldmask = handler_mask;
    user_frame.uc.context.fpstate = fpbase;
    __builtin_memcpy(fpstate, sched_task_fpu_state(task), fpu_state_size);
    if (arch_fpu_xstate_mask()) {
        uint32_t magic = 0x46505853U;
        uint32_t magic2 = 0x46505845U;
        uint32_t extended_size = fpstate_size;
        uint64_t xfeatures = arch_fpu_xstate_mask();
        uint32_t xstate_size = fpu_state_size;
        uint64_t present;
        __builtin_memcpy(&present, fpstate + 512, sizeof(present));
        /* XSAVE may omit initial components. Materialize the legacy prefix
         * before exposing it to signal handlers that modify x87/XMM fields. */
        if (!(present & 1u)) {
            __builtin_memset(fpstate, 0, 24);
            __builtin_memset(fpstate + 32, 0, 128);
            uint16_t control = 0x037fU;
            __builtin_memcpy(fpstate, &control, sizeof(control));
        }
        if (!(present & 2u)) __builtin_memset(fpstate + 160, 0, 256);
        if (!(present & 6u)) {
            uint32_t mxcsr = 0x1f80U;
            __builtin_memcpy(fpstate + 24, &mxcsr, sizeof(mxcsr));
        }
        if ((xfeatures & 4u) && !(present & 4u))
            __builtin_memset(fpstate + 576, 0, 256);
        present |= 3u;
        __builtin_memcpy(fpstate + 512, &present, sizeof(present));
        __builtin_memset(fpstate + 416, 0, 48);
        user_frame.uc.flags |= 1u; /* UC_FP_XSTATE */
        /* Linux's _fpx_sw_bytes occupies the software-reserved FXSAVE
         * tail. The XSAVE header follows at offset 512 and the YMM upper
         * halves occupy 576..831 for the x87/SSE/AVX policy. */
        __builtin_memcpy(fpstate + 464, &magic, sizeof(magic));
        __builtin_memcpy(fpstate + 468, &extended_size, sizeof(extended_size));
        __builtin_memcpy(fpstate + 472, &xfeatures, sizeof(xfeatures));
        __builtin_memcpy(fpstate + 480, &xstate_size, sizeof(xstate_size));
        __builtin_memset(fpstate + 484, 0, 28);
        __builtin_memset(fpstate + 520, 0, 56);
        __builtin_memcpy(fpstate + fpstate_size - sizeof(magic2), &magic2, sizeof(magic2));
    } else {
        /* Legacy FXSAVE frames use bytes 464..511 for software metadata. */
        __builtin_memset(fpstate + 464, 0, fpu_state_size - 464);
    }
    if (user_copy_to_task(task, fpbase, fpstate, fpstate_size) < 0 ||
        user_copy_to_task(task, base, &user_frame, size) < 0) return -14;
    if (task->signal_stack_flags & 0x80000000u) {
        task->signal_stack_size = 0;
        task->signal_stack_base = 0;
        task->signal_stack_flags = 0;
    }
    task->sigsuspend_active = 0;

    /* The handler runs with the delivered signal and the action mask blocked;
     * SA_NODEFER omits only the automatic current-signal block. */
    task->blocked_signals = handler_mask | (action->mask & KERNEL_SIGNAL_VALID_MASK);
    if ((action->flags & LINUX_SA_NODEFER) == 0) {
        task->blocked_signals |= bit;
    }
    task->blocked_signals &= ~((1ULL << 8) | (1ULL << 18));
    frame->rsp = base;
    frame->rip = action->handler;
    frame->rdi = (uint64_t)sig;
    frame->rsi = (action->flags & LINUX_SA_SIGINFO)
                     ? (base + __builtin_offsetof(struct linux_rt_sigframe, info))
                     : 0;
    frame->rdx = (action->flags & LINUX_SA_SIGINFO)
                     ? (base + __builtin_offsetof(struct linux_rt_sigframe, uc))
                     : 0;
    frame->rax = 0;
    return 0;
}

/** @brief Dequeue and install a frame while task lifetime and dispositions are serialized. */
/** @brief Prepare one signal operand without discarding accepted I/O progress.
 * @param task Selected current task. @param address User operand. @param size Bytes.
 * @param write_access True for output. @return Swap preparation status.
 * If this interrupted task owns an actual provider request, drain it before
 * retrying page-in; the existing signal settlement still owns its staged data.
 */
static int signal_prepare_range(struct task *task, uint64_t address, uint64_t size,
                                bool write_access)
{
    int ret = swap_prepare_user_range(task, address, size, write_access);
#ifdef CONFIG_SWAP
    if (ret == -EBUSY && task == sched_current_task() && task->restart_syscall &&
        swap_storage_current_owner()) {
        storage_drain_task_io(task->pid);
        ret = swap_prepare_user_range(task, address, size, write_access);
    }
#endif
    return ret;
}

/** @brief Prepare only the stack and optional interrupted-sleep output in use.
 * @param task Selected task. @param action Candidate caught action.
 * @param frame Original saved frame. @return Prerequisite or ordinary error.
 * Invalid destinations keep the existing late-copy failure behavior.
 */
static int signal_prepare_frame(struct task *task, struct kernel_signal_action *action,
                                const struct trap_frame *frame)
{
    if (!action->restorer) return 0;
    uint64_t size = sizeof(struct linux_rt_sigframe);
    bool on_altstack = task->signal_stack_size && frame->rsp >= task->signal_stack_base &&
        frame->rsp - task->signal_stack_base < task->signal_stack_size;
    uint64_t top = frame->rsp;
    if ((action->flags & LINUX_SA_ONSTACK) && task->signal_stack_size && !on_altstack)
        top = task->signal_stack_base + task->signal_stack_size;
    uint32_t fpu_state_size = arch_fpu_state_size();
    uint32_t fpstate_size = fpu_state_size + (arch_fpu_xstate_mask() ? 4u : 0u);
    if (top < 128 + fpstate_size + size + 80) return 0;
    uint64_t fpbase = (top - 128 - fpstate_size) & ~63ULL;
    uint64_t base = ((fpbase - size) & ~15ULL) - 8;
    if (!base || base < RELIEFNT_USER_BASE || base > top) return 0;
    int ret = signal_prepare_range(task, base, fpbase + fpstate_size - base, true);
    if (ret) return ret;
    if (task->restart_syscall && task->nanosleep_remaining) {
        uint64_t nr = task->restart_syscall - 1;
        if (nr == __NR_nanosleep || nr == __NR_clock_nanosleep)
            ret = signal_prepare_range(task, task->nanosleep_remaining,
                                       sizeof(struct linux_timespec), true);
    }
#ifdef CONFIG_SWAP
    if(!ret)ret=task_socket_signal_prepare(task);
    if(!ret)ret=task_socket_batch_signal_prepare(task);
    if(!ret && task->transfer_io)ret=task_transfer_prepare_interrupt(task);
#endif
    return ret;
}

static int signal_deliver_pending_locked(struct task *task, struct trap_frame *frame,
                                         bool prepare)
{
    struct kernel_signal_action *action;
    struct linux_siginfo info;
    int ret;

    if (!task || !frame || task->kind != TASK_KIND_USER ||
        task->state == TASK_EXITED) {
        return 0;
    }
    if (sched_vfork_waiting(task)) {
        int fatal = kernel_signal_fatal_pending(task);
        if (fatal) signal_default_action(task, fatal);
        return 0;
    }
    /* A completed PI acquisition owns its user lock word. Consume the result
     * before a handler can enter another mutex operation on this thread. */
    if (task->restart_syscall && task->futex_pi && task->futex_state == 2)
        return 0;
    /* signalfd_dequeue checks its queue before signal_pending after a wakeup.
     * Our parked syscall must regain control before return-to-user delivery. */
    if (task->restart_syscall && task->signalfd_waiting && task->syscall_file &&
        task->syscall_file->kind == TASK_FILE_KIND_SIGNALFD &&
        (sched_task_pending(task) & task->syscall_file->aux)) return 0;
    /* A direct delivery or RMID result precedes signal_pending in ipc/msg.c. */
    if (task->restart_syscall && task->sysv_msg.completed) return 0;
    if (task->restart_syscall && task_sysv_sem_ready(task)) return 0;
    int sig;
    for (;;) {
        uint64_t allowed = ~(task->blocked_signals | task->sigwait_mask);
        if (prepare) {
            sig = kernel_signal_peek(task, allowed);
            if (!sig) break;
            action = signal_action(task, sig);
            if (action && action->handler && action->handler != 1) {
                ret = signal_prepare_frame(task, action, frame);
                if (ret == SWAP_STORAGE_CONTENDED) return ret;
            }
            /* Do not consume a newly queued, unprepared signal. */
            allowed &= 1ULL << (sig - 1);
        }
        sig = kernel_signal_dequeue(task, allowed, &info);
        if (!sig) { if (prepare) continue; break; }
        uint64_t bit = 1ULL << (uint32_t)(sig - 1);
        action = signal_action(task, sig);
        if (!action || action->handler == 0 || action->handler == 1) {
            if (!action || action->handler != 1) signal_default_action(task, sig);
            if (task->state == TASK_EXITED || task->state == TASK_STOPPED) return 0;
            continue;
        }
        ret = signal_setup_frame(task, sig, action, frame, &info);
        if (ret < 0) {
            /* Linux force_sigsegv(): a broken signal stack must not silently
             * discard the signal and return to the interrupted program. */
            struct kernel_signal_action *fault = signal_action(task, 11);
            if (sig == 11 || fault->handler == 1 || (task->blocked_signals & (1ULL << 10)))
                fault->handler = 0;
            task->blocked_signals &= ~(1ULL << 10);
            task->sigwait_mask &= ~(1ULL << 10);
            struct linux_siginfo failure = {.signo = 11, .code = LINUX_SI_KERNEL};
            kernel_signal_enqueue(task, false, 11, &failure);
            continue;
        }
        if ((action->flags & LINUX_SA_RESETHAND) != 0) {
            action->handler = 0;
            action->restorer = 0;
            task->ignored_signals &= ~bit;
        }
        /* A caught signal may have been queued before the task parked.  Its
         * successful frame installation must release that interruptible wait. */
        sched_wake_interruptible(task);
        return 1;
    }
    return 0;
}

/**
 * @brief Serialize return-to-user delivery against sigaction, exec and exit.
 * @param task Thread whose pending queue and destination address space are used.
 * @param frame Saved native x86-64 user register frame to update.
 * @return One for a handler frame, zero for no handler, or a negative stack-copy error.
 */
int kernel_signal_deliver_pending(struct task *task, struct trap_frame *frame)
{
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    int result = signal_deliver_pending_locked(task, frame, false);
    kernel_execution_unlock_irqrestore(flags);
    return result;
}

/** @brief Prepare and deliver a signal at the outer execution-writer gate.
 * @param task Live target. @param frame Saved user registers.
 * @return One for handler delivery, zero for no delivery, or a private wait /
 * terminal preparation error. Accepted work keeps caught signals queued;
 * default fatal, stop, continue and ignored actions remain effective.
 */
int kernel_signal_deliver_pending_outer(struct task *task, struct trap_frame *frame)
{
    if (!kernel_execution_is_outermost()) return -EDEADLK;
#ifdef CONFIG_SWAP
    if (task && (sched_exec_staged(task) || task_socket_stage_receive_accepted(task) ||
                 task_socket_batch_accepted(task) ||
                 (task->restart_syscall && task->sysv_msg.completed))) {
        /* A committed exec cannot return to the old handler image; an
         * accepted socket or SysV message must finish its bounded copy first. Keep
         * caught signals queued; preserve ignore and default actions. */
        uint64_t allowed = ~task->blocked_signals | (1ULL << 8) | (1ULL << 18);
        for (;;) {
            int sig = kernel_signal_peek(task, allowed);
            if (!sig) break;
            uint64_t bit = 1ULL << (sig - 1);
            struct kernel_signal_action *action = signal_action(task, sig);
            if (action && action->handler > 1) {
                allowed &= ~bit;
                continue;
            }
            struct linux_siginfo info;
            if (kernel_signal_dequeue(task, bit, &info) && (!action || !action->handler))
                signal_default_action(task, sig);
            if (task->state == TASK_EXITED || task->state == TASK_STOPPED) break;
        }
        return 0;
    }
#endif
    return signal_deliver_pending_locked(task, frame, true);
}

void kernel_signal_force_fault(struct task *task, int sig, int code, uint64_t address)
{
    uint64_t flags, bit = 1ULL << (sig - 1);
    kernel_execution_lock_irqsave(&flags);
    struct kernel_signal_action *action = signal_action(task, sig);
    if (action->handler == 1 || (task->blocked_signals & bit)) action->handler = 0;
    task->blocked_signals &= ~bit;
    task->sigwait_mask &= ~bit;
    task->ignored_signals &= ~bit;
    task->signal_fault_address = address;
    struct linux_siginfo info = {.signo = sig, .code = code, .fields.fault.address = address};
    kernel_signal_enqueue(task, false, sig, &info);
    kernel_execution_unlock_irqrestore(flags);
}

int64_t kernel_signal_rt_sigreturn(struct task *task, struct trap_frame *frame)
{
    struct linux_rt_sigframe user_frame;
    uint8_t fpstate[RELIEFNT_FPU_STATE_MAX_SIZE + 4u] __attribute__((aligned(64)));
    uint32_t fpu_state_size = arch_fpu_state_size();
    uint64_t base;
    uint64_t size = sizeof(user_frame);

    if (!task || !frame || task->kind != TASK_KIND_USER || frame->rsp < 8 ||
        frame->rsp < size) {
        return -14;
    }
    base = frame->rsp - 8U;
    int prep = signal_prepare_range(task, base, size, false);
    if (prep == SWAP_STORAGE_CONTENDED) return KERNEL_SYSCALL_SWAP_CONTENDED;
    if (prep < 0) return -14;
    if (!user_range_ok(base, size)) return -14;
    __builtin_memcpy(&user_frame, (const void *)(uintptr_t)base, size);
    const struct linux_sigcontext *context = &user_frame.uc.context;
    if ((frame->cs & 3ULL) != 3ULL || context->cs != 0x23 ||
        context->ss != 0x1b || context->rip < RELIEFNT_USER_BASE ||
        context->rip >= RELIEFNT_USER_TOP || context->rsp >= RELIEFNT_USER_TOP) {
        return -22;
    }
    __builtin_memset(fpstate, 0, sizeof(fpstate));
    if (context->fpstate) {
        uint64_t address = context->fpstate;
        if ((address & 15u) || address < RELIEFNT_USER_BASE ||
            address > RELIEFNT_USER_TOP - RELIEFNT_FPU_LEGACY_STATE_SIZE)
            return -LINUX_EINVAL;
        /* Read only the legacy prefix until valid extension metadata proves
         * that additional bytes belong to this frame. */
        prep = signal_prepare_range(task, address, RELIEFNT_FPU_LEGACY_STATE_SIZE, false);
        if (prep == SWAP_STORAGE_CONTENDED) return KERNEL_SYSCALL_SWAP_CONTENDED;
        if (prep < 0 || !user_range_ok(address, RELIEFNT_FPU_LEGACY_STATE_SIZE))
            return -LINUX_EFAULT;
        __builtin_memcpy(fpstate, (const void *)(uintptr_t)address,
                         RELIEFNT_FPU_LEGACY_STATE_SIZE);
        uint32_t mxcsr;
        uint32_t magic;
        uint32_t extended_size;
        uint32_t xstate_size;
        uint64_t features;
        __builtin_memcpy(&mxcsr, fpstate + 24, sizeof(mxcsr));
        if (mxcsr & ~arch_fpu_mxcsr_mask()) return -LINUX_EINVAL;
        __builtin_memcpy(&magic, fpstate + 464, sizeof(magic));
        __builtin_memcpy(&extended_size, fpstate + 468, sizeof(extended_size));
        __builtin_memcpy(&features, fpstate + 472, sizeof(features));
        __builtin_memcpy(&xstate_size, fpstate + 480, sizeof(xstate_size));
        bool extended = false;
        if (arch_fpu_xstate_mask() && magic == 0x46505853U &&
            xstate_size >= 576u && xstate_size <= fpu_state_size &&
            xstate_size <= extended_size) {
            uint32_t bytes = xstate_size + 4u;
            if ((address & 63u) || address > RELIEFNT_USER_TOP - bytes)
                return -LINUX_EINVAL;
            prep = signal_prepare_range(task, address, bytes, false);
            if (prep == SWAP_STORAGE_CONTENDED) return KERNEL_SYSCALL_SWAP_CONTENDED;
            if (prep < 0 || !user_range_ok(address, bytes)) return -LINUX_EFAULT;
            __builtin_memcpy(fpstate, (const void *)(uintptr_t)address, bytes);
            uint32_t trailer;
            __builtin_memcpy(&trailer, fpstate + xstate_size, sizeof(trailer));
            extended = trailer == 0x46505845U;
            if (extended) {
                uint64_t present;
                __builtin_memcpy(&present, fpstate + 512, sizeof(present));
                if ((features & ~arch_fpu_xstate_mask()) || (present & ~features) ||
                    ((present & 4u) && xstate_size < 832u))
                    return -LINUX_EINVAL;
                for (unsigned i = 520; i < 576; ++i)
                    if (fpstate[i]) return -LINUX_EINVAL;
                __builtin_memset(fpstate + xstate_size, 0,
                                 sizeof(fpstate) - xstate_size);
            }
        }
        /* The extension read replaces the legacy prefix. Validate the final
         * kernel-owned image even if userspace changed it between the reads. */
        __builtin_memcpy(&mxcsr, fpstate + 24, sizeof(mxcsr));
        if (mxcsr & ~arch_fpu_mxcsr_mask()) return -LINUX_EINVAL;
        if (!extended && arch_fpu_xstate_mask()) {
            __builtin_memset(fpstate + 512, 0, sizeof(fpstate) - 512);
            uint64_t present = 3u;
            __builtin_memcpy(fpstate + 512, &present, sizeof(present));
        }
        __builtin_memset(fpstate + 464, 0, 48);
        __builtin_memcpy(sched_task_fpu_state(task), fpstate, fpu_state_size);
        __builtin_memset(sched_task_fpu_state(task) + fpu_state_size, 0,
                         RELIEFNT_FPU_STATE_MAX_SIZE - fpu_state_size);
    } else {
        arch_fpu_task_init(sched_task_fpu_state(task));
    }
    arch_fpu_restore(sched_task_fpu_state(task));
    signal_context_restore(frame, context);
    /* The restored context's rcx/r11 are live values again; drop the
     * SYSCALL-entry marker so the return goes through iretq. */
    frame->error = 0;
    task->blocked_signals = user_frame.uc.mask;
    task->blocked_signals &= ~((1ULL << 8) | (1ULL << 18));
    task->sigsuspend_active = 0;
    if (user_frame.uc.stack.flags & 2) {
        task->signal_stack_base = 0;
        task->signal_stack_size = 0;
        task->signal_stack_flags = 0;
    } else if (user_frame.uc.stack.sp < RELIEFNT_USER_TOP &&
               user_frame.uc.stack.size <= RELIEFNT_USER_TOP - user_frame.uc.stack.sp) {
        task->signal_stack_base = user_frame.uc.stack.sp;
        task->signal_stack_size = user_frame.uc.stack.size;
        task->signal_stack_flags = (uint32_t)user_frame.uc.stack.flags & 0x80000000u;
    }
    return 0;
}

void kernel_signal_reset_handlers(struct task *task)
{
    if (!task) return;
    task->signal_stack_base = 0;
    task->signal_stack_size = 0;
    task->signal_stack_flags = 0;
    task->restart_syscall = 0;
    task->nanosleep_deadline = task->nanosleep_remaining = 0;
    task->sigwait_deadline = task->sigwait_mask = 0;
    task->sigwait_active = 0;
    for (int sig = 1; sig < LINUX_NSIG; ++sig) {
        if (sched_task_actions(task)[sig].handler != 1)
            sched_task_actions(task)[sig] = (struct kernel_signal_action){0};
    }
    task->sigsuspend_saved_mask = 0;
    task->sigsuspend_active = 0;
}

void kernel_signal_state_snapshot(const struct task *task,
                                  struct kernel_signal_action actions[KERNEL_SIGNAL_ACTION_MAX],
                                  uint64_t *pending, uint64_t *blocked,
                                  uint64_t *ignored)
{
    if (!task) return;
    for (int sig = 0; sig < LINUX_NSIG; ++sig) {
        actions[sig] = sched_task_actions(task)[sig];
    }
    if (pending) *pending = sched_task_pending(task);
    if (blocked) *blocked = task->blocked_signals;
    if (ignored) *ignored = task->ignored_signals;
}
