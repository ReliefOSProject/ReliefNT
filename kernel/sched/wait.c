#include "internal.h"

static void sched_wait_info(struct linux_siginfo *info, const struct task *task, int status);


/**
 * @brief Wake a non-exited task: clear its wait state and set it READY.
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
    task->wait_window_id = 0;
    task->state = TASK_BLOCKED;
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

/**
 * @brief Wake a published interruptible wait without resuming stopped/exited tasks.
 * @param task Task pinned by the kernel execution lock; it may still be retiring on another CPU.
 */
void sched_wake_interruptible(struct task *task)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    if (task && task->state == TASK_BLOCKED && !task->vfork_child) {
        task->wake_tick = 0;
        task->wait_window_id = 0;
        task->state = TASK_READY;
        sched_request_reschedule_all_online();
    }
    /* running_cpu prevents another CPU from claiming a not-yet-retired frame. */
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
    task->wait_window_id = 0;
    task->state = TASK_READY;
    /* A newly runnable task must be considered for scheduling on the next
     * return-to-user boundary; the syscall fast-path skips the scheduler round
     * only while this is clear (plus the per-tick re-evaluation below). */
    sched_request_reschedule_all_online();
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

/**
 * @brief Put the current task to sleep until wake_tick (no-op for the idle or an exited task).
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
    task->wait_window_id = 0;
    task->wake_tick = wake_tick;
    /* Keep the CPU reservation until the interrupt frame is captured by
     * userland_schedule_from_frame().  Clearing it here would make that
     * capture look like a second CPU owns the task. */
    task->state = TASK_BLOCKED;
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

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

/**
 * @brief Observe child events or consume a quiescent zombie, logging a successful reap after unlock.
 * @param waiter_pid Waiting task identifier.
 * @param wanted_pid Child or process-group selector using waitpid semantics.
 * @param options Wait-event selection and WNOWAIT flags.
 * @param status Optional output wait status.
 * @param info Optional output child siginfo.
 * @return Child PID on an observed event, zero without a matching child,
 * -2 for an absent waiter, or -RELIEFOS_EAGAIN while a matching child is pending.
 * The caller must not hold scheduler_lock; resource teardown and diagnostics
 * execute outside it. WNOWAIT observes without consuming or logging a reap.
 */
int64_t sched_wait_reap(uint32_t waiter_pid, int32_t wanted_pid,
                        uint32_t options, int *status, struct linux_siginfo *info)
{
    uint64_t lock_flags;
    struct task *waiter;
    struct task *reap_task = NULL;
    uint32_t reap_pid = 0;
    int reap_status = 0;
    int found_child = 0;
    bool reap_event = false;
    uint32_t wanted_group = 0;
    kernel_spin_lock_irqsave(&scheduler_lock, &lock_flags);
    waiter = sched_find(waiter_pid);
    if (!waiter) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        return -2;
    }
    if (wanted_pid < -1) {
        wanted_group = (uint32_t)(-(int64_t)wanted_pid);
    } else if (wanted_pid == 0) {
        wanted_group = waiter->process_group;
    }
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (task->pid == 0 || task->kind != TASK_KIND_USER) {
            continue;
        }
        if (task->pid != sched_task_tgid(task)) continue;
        if (task->parent_pid != sched_task_tgid(waiter) && waiter_pid != 0) {
            continue;
        }
        if (wanted_pid > 0 && task->pid != (uint32_t)wanted_pid) {
            continue;
        }
        if (wanted_group && task->process_group != wanted_group) {
            continue;
        }
        if (!(options & 0x40000000u) &&
            ((task->parent_exit_signal != 17) != ((options & 0x80000000u) != 0))) continue;
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
            if (options & 0x01000000u) {
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
            if (status) {
                *status = stopped_status;
            }
            if (!(options & 0x01000000u)) task->child_event = TASK_CHILD_EVENT_NONE;
            kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
            return task->pid;
        }
        if (task->child_event == TASK_CHILD_EVENT_CONTINUED && (options & 8U)) {
            sched_wait_info(info, task, 0xffff);
            if (status) {
                *status = 0xffff;
            }
            if (!(options & 0x01000000u)) task->child_event = TASK_CHILD_EVENT_NONE;
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
            reap_task->parent_pid = 0;
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
        if (reap_event) {
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
