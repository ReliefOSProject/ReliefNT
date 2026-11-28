#include <reliefnt/export.h>
#include "internal.h"
#include <reliefnt/cgroup.h>
#include <linux/resource.h>
#include <linux/wait.h>

static void sched_wait_info(struct linux_siginfo *info, const struct task *task, int status);


/**
 * @brief Mark the current live non-idle task blocked with no timed wakeup.
 * Execution-owned task context. Updates state under scheduler_lock; idle, absent or exited tasks are ignored. Scheduling occurs at the normal return boundary, not inside this function.
 */
void sched_block_current(void)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    struct task *task = sched_current_task();
    if (!task || task->pid == 0 || task->state == TASK_EXITED) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
        return;
    }
    task->wake_tick = 0;
    task->state = TASK_BLOCKED;
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

/** @brief Wake a blocked task without reviving stopped/exited/vfork-wait tasks.
 * @param task Borrowed task pinned by execution/wait lifetime, nullable.
 * IRQ/task safe scheduler-lock transaction; requests remote rescheduling only
 * after publishing readiness. No allocation, callback or ownership transfer;
 * running_cpu keeps an unretired frame from being claimed by another CPU.
 */
void sched_wake_interruptible(struct task *task)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    if (task && task->state == TASK_BLOCKED && !task->vfork_child) {
        task->wake_tick = 0;
        task->state = TASK_READY;
        sched_request_reschedule_all_online();
    }
    /* running_cpu prevents another CPU from claiming a not-yet-retired frame. */
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

/** @brief Make a stopped or blocked task runnable for an admitted fatal signal.
 * @param task Execution-owned task. Never releases a running CPU reservation.
 */
void sched_wake_fatal(struct task *task)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    if (task && !task->vfork_child &&
        (task->state == TASK_BLOCKED || task->state == TASK_STOPPED)) {
        task->wake_tick = 0;
        task->state = TASK_READY;
        sched_request_reschedule_all_online();
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

void sched_mark_ready(uint32_t pid)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    struct task *task = sched_find(pid);
    if (!task || task->state == TASK_EXITED || task->vfork_child) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
        return;
    }
    /* A running task is already runnable.  In particular, an event emitted
     * by CPU0 must not publish a task currently executing on CPU1 as READY:
     * doing so lets a third CPU claim its address space and eventually return
     * through a corrupted trap frame. */
    if (task->state == TASK_RUNNING) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
        return;
    }
    /* A wakeup may race the owner's syscall epilogue. Publish readiness,
     * but never release another CPU's still-live frame/CR3 reservation. */
    task->wake_tick = 0;
    task->state = TASK_READY;
    /* A newly runnable task must be considered for scheduling on the next
     * return-to-user boundary; the syscall fast-path skips the scheduler round
     * only while this is clear (plus the per-tick re-evaluation below). */
    sched_request_reschedule_all_online();
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

/**
 * @brief Block the current live non-idle task until an absolute scheduler tick.
 * @param wake_tick Absolute wake tick; zero retains the untimed-blocked convention.
 * Execution-owned task context. Scheduler lock protects state/wake_tick; absent, idle or exited tasks are ignored. CPU ownership stays reserved until the normal saved-frame scheduling boundary.
 */
void sched_sleep_current_until(uint64_t wake_tick)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    struct task *task = sched_current_task();
    if (!task || task->pid == 0 || task->state == TASK_EXITED) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
        return;
    }
    task->wake_tick = wake_tick;
    sched_deadline_include(&scheduler_next_wake_tick, wake_tick);
    /* Keep the CPU reservation until the interrupt frame is captured by
     * userland_schedule_from_frame().  Clearing it here would make that
     * capture look like a second CPU owns the task. */
    task->state = TASK_BLOCKED;
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

/** @brief Format a child event as native SIGCHLD siginfo without user access.
 * @param info Optional kernel output; NULL leaves the event unformatted.
 * @param task Borrowed child whose PID and UID identify the event.
 * @param status Encoded native wait status, including stop/continue events.
 * Caller holds scheduler_lock; performs no allocation or reference transfer.
 */
static void sched_wait_info(struct linux_siginfo *info, const struct task *task, int status)
{
    if (!info) return;
    *info = (struct linux_siginfo){.signo = 17};
    info->fields.child.pid = (int32_t)task->pid;
    info->fields.child.uid = task->uid;
    if (status == 0xffff) {
        info->code = 6; /* CLD_CONTINUED */
        info->fields.child.status = 18;
    } else if ((status & 0xff) == 0x7f) {
        info->code = 5; /* CLD_STOPPED */
        info->fields.child.status = status >> 8;
    } else if (status & 0x7f) {
        info->code = status & 0x80 ? 3 : 2;
        info->fields.child.status = status & 0x7f;
    } else {
        info->code = 1; /* CLD_EXITED */
        info->fields.child.status = status >> 8;
    }
}

/** @brief Snapshot the CPU counters available for a waitable child's group.
 * @param out Optional kernel rusage destination; untracked counters remain zero.
 * @param task Waitable child under scheduler_lock; no user memory is accessed.
 */
static void sched_wait_usage(struct linux_rusage *out, const struct task *task)
{
    if (!out) return;
    uint64_t user, system;
    cpu_accounting_task_times_locked(task, true, &user, &system);
    *out = (struct linux_rusage){
        .ru_utime = {(int64_t)(user / RELIEFNT_TICK_HZ),
                     (int64_t)((user % RELIEFNT_TICK_HZ) * (1000000ULL / RELIEFNT_TICK_HZ))},
        .ru_stime = {(int64_t)(system / RELIEFNT_TICK_HZ),
                     (int64_t)((system % RELIEFNT_TICK_HZ) * (1000000ULL / RELIEFNT_TICK_HZ))},
    };
}

/**
 * @brief Observe child events or consume a quiescent zombie, logging a successful reap after unlock.
 * @param waiter_pid Waiting task identifier.
 * @param wanted_pid Child or process-group selector using waitpid semantics.
 * @param identity Optional retained PID object that overrides the numeric selector.
 * @param options Wait-event selection and WNOWAIT flags.
 * @param status Optional output wait status.
 * @param info Optional output child siginfo.
 * @param usage Optional output resource usage, written only for a child event.
 * @return Child PID on an observed event, zero without a matching child,
 * -2 for an absent waiter, or -RELIEFOS_EAGAIN while a matching child is pending.
 * The caller must not hold scheduler_lock; resource teardown and diagnostics
 * execute outside it. WNOWAIT observes without consuming or logging a reap.
 * Consuming a zombie releases its cgroup charge and invalidates its retained
 * PID identity before PID unpublish.
 */
static int64_t sched_wait_child(uint32_t waiter_pid, int32_t wanted_pid,
                                const struct task_identity *identity,
                                uint32_t options, int *status, struct linux_siginfo *info,
                                struct linux_rusage *usage)
{
    uint64_t lock_flags;
    struct task *waiter;
    struct task *reap_task = NULL;
    uint32_t reap_pid = 0;
    int reap_status = 0;
    int found_child = 0;
    bool reap_event = false;
    bool notify_pidfd = false;
    uint32_t wanted_group = 0;
    kernel_spin_lock_irqsave(&scheduler_lock, &lock_flags);
    waiter = sched_find(waiter_pid);
    if (!waiter) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        return -2;
    }
    const struct task *exact = sched_task_identity_task(identity);
    if (identity && !exact) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        return 0;
    }
    if (!identity && wanted_pid < -1) {
        wanted_group = (uint32_t)(-(int64_t)wanted_pid);
    } else if (!identity && wanted_pid == 0) {
        wanted_group = waiter->process_group;
    }
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (exact && task != exact) continue;
        if (task->pid == 0 || task->kind != TASK_KIND_USER || (task->flags & TASK_FLAG_LAUNCH_PREPARING)) {
            continue;
        }
        if (task->pid != sched_task_tgid(task)) continue;
        if (task->parent_pid != sched_task_tgid(waiter) && waiter_pid != 0) {
            continue;
        }
        if ((options & LINUX_WNOTHREAD) && task->pdeath_parent_pid != waiter_pid) continue;
        if (wanted_pid > 0 && task->pid != (uint32_t)wanted_pid) {
            continue;
        }
        if (wanted_group && task->process_group != wanted_group) {
            continue;
        }
        if (!(options & LINUX_WALL) &&
            ((task->parent_exit_signal != 17) != ((options & LINUX_WCLONE) != 0))) continue;
        if (task->state == TASK_EXITED && !(options & 4u) &&
            !thread_group_pending(sched_task_tgid(task), task)) continue;
        found_child = 1;
        if ((options & 4u) && task->state == TASK_EXITED && task->running_cpu == SCHED_CPU_NONE &&
            !thread_group_pending(sched_task_tgid(task), task) &&
            !(task->flags & TASK_FLAG_REAP_IN_PROGRESS)) {
            reap_task = task;
            reap_pid = task->pid;
            reap_status = task->exit_signal ? (int)(task->exit_signal & 0x7fU) :
                (int)((task->exit_code & 0xffU) << 8);
            sched_wait_info(info, task, reap_status);
            if ((options & LINUX_WNOWAIT)
#ifdef CONFIG_SWAP
                && (task->flags & TASK_FLAG_RESOURCES_RELEASED)
#endif
               ) {
                sched_wait_usage(usage, task);
                if (status) *status = reap_status;
                kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
                return reap_pid;
            }
            task->flags |= TASK_FLAG_REAP_IN_PROGRESS;
            break;
        }
        if (task->child_event == TASK_CHILD_EVENT_STOPPED && (options & 2U)) {
            int stopped_status = (int)(((task->stop_signal & 0xffU) << 8) | 0x7fU);
            sched_wait_info(info, task, stopped_status);
            sched_wait_usage(usage, task);
            if (status) {
                *status = stopped_status;
            }
            if (!(options & LINUX_WNOWAIT)) task->child_event = TASK_CHILD_EVENT_NONE;
            kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
            return task->pid;
        }
        if (task->child_event == TASK_CHILD_EVENT_CONTINUED && (options & 8U)) {
            sched_wait_info(info, task, 0xffff);
            sched_wait_usage(usage, task);
            if (status) {
                *status = 0xffff;
            }
            if (!(options & LINUX_WNOWAIT)) task->child_event = TASK_CHILD_EVENT_NONE;
            kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
            return task->pid;
        }
    }
    if (reap_task) {
        /* Address-space, storage, and pipe teardown can take other locks and
         * may touch page tables.  Never perform that work while holding the
         * scheduler spinlock with interrupts disabled. */
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        sched_release_task_resources(reap_task);

        kernel_spin_lock_irqsave(&scheduler_lock, &lock_flags);
        if (reap_task->pid == reap_pid &&
            (reap_task->flags & TASK_FLAG_REAP_IN_PROGRESS)) {
            if (!(reap_task->flags & TASK_FLAG_RESOURCES_RELEASED)) {
                reap_task->flags &= ~TASK_FLAG_REAP_IN_PROGRESS;
                kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
                return -RELIEFOS_EAGAIN;
            }
            sched_wait_usage(usage, reap_task);
            if (options & LINUX_WNOWAIT) {
                reap_task->flags &= ~TASK_FLAG_REAP_IN_PROGRESS;
                if (status) *status = reap_status;
                kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
                return reap_pid;
            }
            if (cgroup2_task_reap) cgroup2_task_reap(reap_task);
            reap_task->parent_pid = 0;
            notify_pidfd = sched_task_pidfd_watched(reap_task);
            sched_task_identity_detach(reap_task);
            task_release_limits(reap_task);
            reap_task->flags &= ~(TASK_FLAG_WAITABLE_CHILD | TASK_FLAG_REAP_IN_PROGRESS);
            task_copy_name(reap_task, "reaped");
            reap_task->image = NULL;
            reap_task->image_len = 0;
            reap_task->pty_id = 0;
            task_clear_identity(reap_task);
            task_copy_cwd(reap_task, "/");
            for (size_t j = 0; j < SCHED_TASK_FILE_MAX; ++j) {
                sched_task_fds(reap_task)->files[j].used = 0;
                sched_task_fds(reap_task)->files[j].offset = 0;
                sched_task_fds(reap_task)->files[j].aux = 0;
            }
            /* A zombie owns its PID until wait consumes it, not until this
             * storage slot happens to be reused. Unpublish under the same
             * scheduler lock as reaping so kill(pid, 0) returns ESRCH now. */
            reap_task->pid = 0;
            reap_event = true;
        }
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        if (notify_pidfd) kernel_poll_readiness_notify();
        if (reap_event) {
            if (cgroup2_kick) cgroup2_kick();
            console_debug("[reliefnt] scheduler wait reaped pid=%u by pid=%u\n",
                           reap_pid, waiter_pid);
        }
        if (status) {
            *status = reap_status;
        }
        return reap_pid;
    }
    /* The syscall trap retries EAGAIN after parking the caller.  This is
     * distinct from ECHILD, which means no matching child exists at all. */
    kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
    return found_child ? -RELIEFOS_EAGAIN : 0;
}

/** @brief Observe or reap a child selected by native waitpid numeric rules.
 * @param waiter_pid Calling task ID.
 * @param wanted_pid Child PID or waitpid process-group selector.
 * @param options Wait-event flags; WNOWAIT observes without consuming.
 * @param status Optional kernel output for encoded wait status.
 * @param info Optional kernel output for child-event siginfo.
 * @return Child PID, zero without an eligible child, -2 for an absent waiter,
 * or -RELIEFOS_EAGAIN while a matching child has no selected event.
 * Caller holds execution ownership without scheduler_lock. Teardown and reap
 * diagnostics occur outside that lock; no task references escape.
 */
int64_t sched_wait_reap(uint32_t waiter_pid, int32_t wanted_pid,
                        uint32_t options, int *status, struct linux_siginfo *info)
{
    return sched_wait_child(waiter_pid, wanted_pid, NULL, options, status, info, NULL);
}

/** @brief Observe or consume a child event without resolving a numeric PID.
 * @param waiter_pid Calling task ID. @param identity Borrowed PID object.
 * @param options Linux wait-event flags. @param info Optional kernel siginfo output.
 * @param usage Optional kernel resource usage output, written only for an event.
 * @return Child PID, zero when no eligible child exists, or negative errno.
 */
int64_t sched_wait_identity(uint32_t waiter_pid, const struct task_identity *identity,
                            uint32_t options, struct linux_siginfo *info,
                            struct linux_rusage *usage)
{
    return identity ? sched_wait_child(waiter_pid, -1, identity, options, NULL, info, usage) : 0;
}

EXPORT_SYMBOL(sched_wake_interruptible);

EXPORT_SYMBOL(sched_block_current);
EXPORT_SYMBOL(sched_sleep_current_until);
