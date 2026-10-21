#include "internal.h"



void sched_mark_orphaned_group(uint32_t group)
{
    if (!sched_process_group_orphaned(group)) return;
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (task && task->process_group == group && task->state == TASK_STOPPED) {
            task->flags |= TASK_FLAG_ORPHAN_NOTIFY;
            return;
        }
    }
}

/** @brief Wake signalfd readers attached to the mask updater's shared sighand. */
void sched_signalfd_reconfigure(struct task *updater)
{
    uint64_t flags;
    bool woke = false;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (task->signalfd_waiting && task->state == TASK_BLOCKED && !task->vfork_child &&
            sched_task_actions(task) == sched_task_actions(updater)) {
            if (task->syscall_file && task->syscall_file->kind == TASK_FILE_KIND_SIGNALFD)
                task->signalfd_wait_mask = task->syscall_file->aux;
            task->wake_tick = 0;
            task->state = TASK_READY;
            woke = true;
        }
    }
    if (woke) sched_request_reschedule_all_online();
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

void sched_wake_process_signal(uint32_t tgid, uint64_t bit)
{
    bool woke = false;
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (sched_task_tgid(task) == tgid && task->state == TASK_BLOCKED &&
            (!task->vfork_child || kernel_signal_fatal_pending(task)) &&
            (((~task->blocked_signals | task->sigwait_mask) & bit) || task->signalfd_waiting)) {
            task->wake_tick = 0;
            task->wait_window_id = 0;
            task->state = TASK_READY;
            woke = true;
        }
    }
    if (woke) sched_request_reschedule_all_online();
}

void sched_notify_parent_exit(struct task *task)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    struct task *leader = sched_find(sched_task_tgid(task));
    if (!leader || leader->state != TASK_EXITED || leader->running_cpu != SCHED_CPU_NONE ||
        thread_group_pending(leader->pid, leader) || leader->parent_exit_notified) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
        return;
    }
    leader->parent_exit_notified = 1;
    unsigned sig = leader->parent_exit_signal;
    if (leader->parent_pid && sig && sig < KERNEL_SIGNAL_ACTION_MAX) {
        for (uint32_t i = 0; i < task_count; ++i) {
            struct task *parent = tasks[i];
            if (sched_task_tgid(parent) != leader->parent_pid || parent->state == TASK_EXITED) continue;
            if (sched_task_actions(parent)[sig].handler != 1) {
                uint64_t bit = 1ULL << (sig - 1);
                __atomic_fetch_or(sched_task_process_pending(parent), bit, __ATOMIC_SEQ_CST);
                sched_wake_process_signal(leader->parent_pid, bit);
            }
            break;
        }
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

/**
 * @brief Publish signal-wait sleep before rechecking pending notifications.
 * @param deadline Absolute tick deadline, or zero for no timeout.
 */
void sched_signal_wait_current(uint64_t deadline)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    struct task *task = sched_current_task();
    if (task && task->pid && task->state != TASK_EXITED) {
        task->wake_tick = deadline;
        task->wait_window_id = 0;
        __atomic_store_n(&task->state, TASK_BLOCKED, __ATOMIC_SEQ_CST);
        uint64_t pending = __atomic_load_n(&task->pending_signals, __ATOMIC_SEQ_CST) |
            __atomic_load_n(sched_task_process_pending(task), __ATOMIC_SEQ_CST);
        if ((pending & (~task->blocked_signals | task->sigwait_mask | task->signalfd_wait_mask)) ||
            (deadline && scheduler_ticks >= deadline)) {
            task->wake_tick = 0;
            task->state = TASK_READY;
        }
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

/**
 * @brief Terminate a user task by pid, returning a negative error for idle/services/exited/self.
 */
int sched_kill_user_task(uint32_t pid, uint64_t code)
{
    struct task *task = sched_find(pid);
    if (!task || task->pid == 0) {
        return -2;
    }
    if (task->kind != TASK_KIND_USER || task->state == TASK_EXITED ||
        (task->flags & TASK_FLAG_SERVICE) || pid == scheduler_current_pid()) {
        return -1;
    }
    sched_exit(pid, code);
    return 0;
}

/**
 * @brief Record a POSIX signal on a user task, handling STOP/CONT state changes and fatal signals.
 */
int sched_signal_user_task(uint32_t pid, int signal_number)
{
    struct task *task = sched_find(pid);
    if (!task || task->pid == 0 || task->kind != TASK_KIND_USER ||
        task->state == TASK_EXITED || signal_number < 0 || (unsigned)signal_number >= KERNEL_SIGNAL_ACTION_MAX) {
        return -1;
    }
    if (signal_number == 0) {
        return 0;
    }
    /* User-handler delivery now lives beside the signal-frame ABI. Default,
     * ignored, STOP/CONT, blocked and wakeup behavior are all applied here so
     * wait/kill/PTY/console paths do not implement their own signal policy. */
    return kernel_signal_queue_task(task, signal_number);
}

void sched_signal_discard(struct task *task, int signal_number)
{
    uint64_t bit = 1ULL << (signal_number - 1);
    kernel_signal_flush(task, true, bit);
    for (uint32_t i = 0; i < task_count; ++i)
        if (sched_task_tgid(tasks[i]) == sched_task_tgid(task)) kernel_signal_flush(tasks[i], false, bit);
}

void sched_signal_job_control(uint32_t tgid, int signal_number)
{
    const uint64_t stopped = (1ULL << 18) | (1ULL << 19) | (1ULL << 20) | (1ULL << 21);
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        uint64_t flags;
        kernel_spin_lock_irqsave(&scheduler_lock, &flags);
        if (sched_task_tgid(task) != tgid || task->state == TASK_EXITED) {
            kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
            continue;
        }
        uint64_t discard = signal_number == 18 ? stopped : 1ULL << 17;
        kernel_signal_flush(task, false, discard);
        kernel_signal_flush(task, true, discard);
        if (signal_number == 18) {
            if (task->state != TASK_STOPPED) {
                kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
                continue;
            }
            task->state = task->vfork_child ? TASK_BLOCKED : TASK_READY;
            if (task->state == TASK_READY) sched_request_reschedule_all_online();
            task->stop_signal = 0;
            task->child_event = TASK_CHILD_EVENT_CONTINUED;
        } else {
            if (task->vfork_child) {
                /* TASK_KILLABLE does not enter a group stop until completion. */
                kernel_signal_enqueue(task, false, signal_number, NULL);
                kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
                continue;
            }
            task->state = TASK_STOPPED;
            task->stop_signal = (uint32_t)signal_number;
            task->child_event = TASK_CHILD_EVENT_STOPPED;
        }
        task->wake_tick = 0;
        task->wait_window_id = 0;
        kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
        if (signal_number != 18) task_sysv_sem_stop(task);
    }
}

int sched_signal_user_process(uint32_t tgid, int signal_number)
{
    struct task *leader = sched_find(tgid);
    if (!leader || sched_task_tgid(leader) != tgid) return -1;
    return sched_signal_user_process_info(tgid, signal_number, NULL);
}

/**
 * @brief Queue process-directed siginfo using the addressed PID or TID's thread group.
 * @param pid Addressed task whose credentials and limits govern the send.
 * @param signal_number Native signal number, zero for an existence probe.
 * @param info Explicit signal data, or NULL for a kernel notification.
 * @return Zero on success, or a negative target/queue error.
 */
int sched_signal_user_process_info(uint32_t pid, int signal_number,
                                   const struct linux_siginfo *info)
{
    struct task *leader = sched_find(pid), *live = NULL, *eligible = NULL;
    if (!leader || leader->kind != TASK_KIND_USER ||
        signal_number < 0 || (unsigned)signal_number >= KERNEL_SIGNAL_ACTION_MAX) return -1;
    uint32_t tgid = sched_task_tgid(leader);
    if (!signal_number) return 0;
    uint64_t bit = 1ULL << (signal_number - 1);
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (sched_task_tgid(task) != tgid || task->state == TASK_EXITED) continue;
        if (!live) live = task;
        if (!eligible && ((~task->blocked_signals | task->sigwait_mask) & bit)) eligible = task;
    }
    if (!live) return 0; /* A zombie still owns its PID until wait reaps it. */
    if (signal_number == 18) sched_signal_job_control(tgid, signal_number);
    if (signal_number >= 19 && signal_number <= 22) sched_signal_discard(live, 18);
    struct kernel_signal_action *action = &sched_task_actions(live)[signal_number];
    /* Ignore only when the addressed task has neither blocked nor waited for
     * this signal, matching Linux sig_ignored() and real_blocked semantics. */
    if (!((leader->blocked_signals | leader->sigwait_mask) & bit) &&
        (action->handler == 1 || (!action->handler && kernel_signal_default_ignored(signal_number))))
        return 0;
    if (signal_number == 9 || signal_number == 19)
        return info ? kernel_signal_queue_task_info(eligible ? eligible : live, signal_number, info) :
            kernel_signal_queue_task(eligible ? eligible : live, signal_number);
    int result = kernel_signal_enqueue(leader, true, signal_number, info);
    if (result < 0) return result;
    /* All eligible sleepers may recheck; only one consumes the process signal. */
    sched_wake_process_signal(tgid, bit);
    return 0;
}

/**
 * @brief Read or update a task's default/ignored signal disposition.
 * @param pid Target user-task ID.
 * @param signal_number POSIX signal number.
 * @param operation Query or update operation.
 * @param disposition Requested value for update and previous value on return.
 * @return Zero on success or a negative scheduler error.
 */
int sched_signal_action(uint32_t pid, int signal_number, uint32_t operation,
                        uint32_t *disposition)
{
    struct task *task = sched_find(pid);
    uint32_t bit;
    if (!task || task->pid == 0 || task->kind != TASK_KIND_USER ||
        task->state == TASK_EXITED || !disposition || signal_number <= 0 ||
        signal_number >= 32 || signal_number == 9 || signal_number == 17) {
        return -1;
    }
    bit = 1u << (uint32_t)signal_number;
    if (operation == 1U) {
        *disposition = (task->ignored_signals & bit) ? 1U : 0U;
        return 0;
    }
    if (operation != 2U || *disposition > 1U) {
        return -1;
    }
    {
        uint32_t previous = (task->ignored_signals & bit) ? 1U : 0U;
        if (*disposition) {
            task->ignored_signals |= bit;
            task->pending_signals &= ~bit;
        } else {
            task->ignored_signals &= ~bit;
        }
        *disposition = previous;
    }
    return 0;
}

/**
 * @brief Signal every user task in a group the caller owns; returns the count or a negative error.
 */
int sched_signal_kernel_group(uint32_t process_group, int signal_number)
{
    int count = 0;
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (task && task->kind == TASK_KIND_USER && task->state != TASK_EXITED &&
            task->process_group == process_group && task->pid == sched_task_tgid(task) &&
            sched_signal_user_process(task->pid, signal_number) == 0)
            ++count;
    }
    return count ? count : -LINUX_ESRCH;
}

int sched_signal_process_group(uint32_t sender_pid, uint32_t process_group,
                               int signal_number)
{
    struct task *sender = sched_find(sender_pid);
    int count = 0;
    int error = -LINUX_ESRCH;
    if (!sender) return -LINUX_EPERM;
    struct linux_siginfo info = {.signo = signal_number, .code = LINUX_SI_USER,
        .fields.sender = {.pid = (int32_t)sched_task_tgid(sender), .uid = sender->uid}};
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (!task || !task->pid || task->kind != TASK_KIND_USER ||
            task->process_group != process_group || task->pid != sched_task_tgid(task) ||
            (task->state == TASK_EXITED && !(task->flags & TASK_FLAG_WAITABLE_CHILD))) continue;
        if (sched_task_tgid(sender) != sched_task_tgid(task) &&
            !(sender->cap_effective & (1ULL << CAP_KILL)) &&
            sender->uid != task->uid && sender->uid != task->suid &&
            sender->euid != task->uid && sender->euid != task->suid &&
            !(signal_number == 18 && sender->process_session == task->process_session)) {
            if (error == -LINUX_ESRCH) error = -LINUX_EPERM;
            continue;
        }
        int result = sched_signal_user_process_info(task->pid, signal_number, &info);
        if (result >= 0) ++count;
        else error = result;
    }
    return count ? count : error;
}

/**
 * @brief Kill every user task (except keep_pid) attached to pty_id and clear its PTY fds.
 */
int sched_kill_user_tasks_for_pty(uint32_t pty_id, uint32_t keep_pid,
                                  uint64_t code)
{
    int killed = 0;
    if (!pty_id) {
        return 0;
    }
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (task->pid == 0 || task->pid == keep_pid ||
            task->kind != TASK_KIND_USER || task->state == TASK_EXITED ||
            (task->flags & TASK_FLAG_SERVICE) || task->pty_id != pty_id) {
            continue;
        }
        /* This task may still be executing on another CPU.  It becomes
         * reclaimable only after that CPU observes TASK_EXITED and clears
         * running_cpu at its next scheduling boundary. */
        sched_exit(task->pid, code);
        ++killed;
    }
    return killed;
}

int sched_hangup_user_tasks_for_pty(uint32_t pty_id, uint32_t keep_pid)
{
    int detached = 0;
    (void)keep_pid;
    if (!pty_id) {
        return 0;
    }
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (!task || task->kind != TASK_KIND_USER) continue;
        if (task->controlling_pty_id == pty_id) {
            task->controlling_pty_id = 0;
            ++detached;
        }
        if (task->pty_id == pty_id) {
            task->pty_id = 0;
            task->wait_pty_id = 0;
            /* Open endpoints retain their descriptions through hangup. */
        }
    }
    pty_reap_hungup(pty_id);
    return detached;
}

/**
 * @brief Kill every non-service user task in this uid/session except keep_pid.
 */
int sched_kill_user_tasks_for_logout(uint32_t uid, uint32_t session_id,
                                     uint32_t keep_pid, uint64_t code)
{
    int killed = 0;
    if (!uid || !session_id) {
        return 0;
    }
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (task->pid == 0 || task->pid == keep_pid ||
            task->kind != TASK_KIND_USER || task->state == TASK_EXITED ||
            task->uid != uid || task->session_id != session_id ||
            (task->flags & TASK_FLAG_SERVICE)) {
            continue;
        }
        sched_exit(task->pid, code);
        ++killed;
    }
    return killed;
}
