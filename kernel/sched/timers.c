#include "internal.h"

static uint64_t timeval_ns(struct linux_timeval value);

static uint64_t timer_timespec_ticks(const struct linux_timespec *value);

static void timer_ticks_timespec(uint64_t ticks, struct linux_timespec *value);

static struct task_posix_timer *task_timer(struct task *task, int32_t timerid);


static uint64_t timeval_ns(struct linux_timeval value)
{
    if ((uint64_t)value.tv_sec > INT64_MAX / 1000000000ULL) return INT64_MAX;
    uint64_t ns = (uint64_t)value.tv_sec * 1000000000ULL;
    uint64_t fraction = (uint64_t)value.tv_usec * 1000;
    return fraction > INT64_MAX - ns ? INT64_MAX : ns + fraction;
}

void sched_itimer_task(struct task *task, const struct linux_itimerval *value, struct linux_itimerval *old)
{
    struct task *leader = sched_find(sched_task_tgid(task));
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    uint64_t remaining = leader->alarm_deadline > scheduler_ticks ? leader->alarm_deadline - scheduler_ticks : 0;
    if (old) {
        old->it_value = (struct linux_timeval){remaining / RELIEFNT_TICK_HZ,
            (remaining % RELIEFNT_TICK_HZ) * (1000000 / RELIEFNT_TICK_HZ)};
        old->it_interval = (struct linux_timeval){leader->alarm_interval_ns / 1000000000,
            (leader->alarm_interval_ns % 1000000000) / 1000};
    }
    if (value) {
        const uint64_t tick_ns = 1000000000ULL / RELIEFNT_TICK_HZ;
        uint64_t ns = timeval_ns(value->it_value);
        uint64_t duration = (ns + tick_ns - 1) / tick_ns;
        const uint64_t maximum = (INT64_MAX + tick_ns - 1) / tick_ns;
        leader->alarm_deadline = !duration ? 0 : duration > maximum - scheduler_ticks
            ? maximum : scheduler_ticks + duration;
        leader->alarm_interval_ns = duration ? timeval_ns(value->it_interval) : 0;
        leader->alarm_last_expiry = leader->alarm_deadline;
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

uint32_t sched_alarm_task(struct task *task, uint32_t seconds)
{
    struct linux_itimerval value = {.it_value.tv_sec = seconds}, old;
    sched_itimer_task(task, &value, &old);
    uint64_t result = (uint64_t)old.it_value.tv_sec;
    uint64_t fraction = (uint64_t)old.it_value.tv_usec;
    /* Native Linux alarm rounds to nearest second, with a minimum of one while armed. */
    if ((!result && fraction) || fraction >= 500000) ++result;
    return (uint32_t)result;
}

void sched_alarm_rearm(struct task *task)
{
    struct task *leader = sched_find(sched_task_tgid(task));
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    if (leader && !leader->alarm_deadline && leader->alarm_interval_ns) {
        const uint64_t tick_ns = 1000000000ULL / RELIEFNT_TICK_HZ;
        uint64_t interval = (leader->alarm_interval_ns + tick_ns - 1) / tick_ns;
        uint64_t expired = leader->alarm_last_expiry;
        uint64_t periods = scheduler_ticks >= expired ? (scheduler_ticks - expired) / interval + 1 : 1;
        leader->alarm_deadline = expired + periods * interval;
        leader->alarm_last_expiry = leader->alarm_deadline;
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

void sched_alarm_expire(void)
{
    const uint64_t bit = 1ULL << 13;
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *leader = tasks[i];
        if (leader->pid != sched_task_tgid(leader) || !leader->alarm_deadline ||
            leader->alarm_deadline > scheduler_ticks) continue;
        leader->alarm_deadline = 0;
        for (uint32_t j = 0; j < task_count; ++j) {
            struct task *live = tasks[j];
            if (sched_task_tgid(live) != leader->pid || live->state == TASK_EXITED) continue;
            if (sched_task_actions(live)[14].handler != 1) {
                __atomic_fetch_or(sched_task_process_pending(live), bit, __ATOMIC_SEQ_CST);
                sched_wake_process_signal(leader->pid, bit);
            }
            break;
        }
    }
}

static uint64_t timer_timespec_ticks(const struct linux_timespec *value)
{
    uint64_t seconds, ns;
    uint64_t tick_ns = 1000000000ULL / RELIEFNT_TICK_HZ;
    if (!value || value->tv_sec < 0 || value->tv_nsec < 0 || value->tv_nsec >= 1000000000LL)
        return UINT64_MAX;
    seconds = (uint64_t)value->tv_sec;
    if (seconds > UINT64_MAX / 1000000000ULL) return UINT64_MAX;
    ns = seconds * 1000000000ULL + (uint64_t)value->tv_nsec;
    if (ns < seconds * 1000000000ULL) return UINT64_MAX;
    return ns / tick_ns + (ns % tick_ns != 0);
}

static void timer_ticks_timespec(uint64_t ticks, struct linux_timespec *value)
{
    if (!value) return;
    value->tv_sec = (int64_t)(ticks / RELIEFNT_TICK_HZ);
    value->tv_nsec = (int64_t)((ticks % RELIEFNT_TICK_HZ) * (1000000000ULL / RELIEFNT_TICK_HZ));
}

static struct task_posix_timer *task_timer(struct task *task, int32_t timerid)
{
    struct task *leader = task ? sched_find(sched_task_tgid(task)) : NULL;
    if (!leader || timerid < 0 || timerid >= (int32_t)SCHED_TASK_TIMER_MAX) return NULL;
    struct task_posix_timer *timer = &leader->timers[timerid];
    return timer->used ? timer : NULL;
}

int sched_timer_create(struct task *task, int32_t clockid, int32_t notify,
                       int32_t signo, int32_t target_pid, uint64_t value,
                       int32_t *timerid)
{
    uint64_t flags;
    if (!task || !timerid || task->state == TASK_EXITED ||
        (clockid != LINUX_CLOCK_REALTIME && clockid != LINUX_CLOCK_MONOTONIC &&
         clockid != LINUX_CLOCK_BOOTTIME) ||
        (notify != 0 && notify != 1 && notify != 4) ||
        (notify != 1 && (signo <= 0 || signo >= LINUX_NSIG))) return -LINUX_EINVAL;
    if (notify == 4) {
        struct task *target = sched_find((uint32_t)target_pid);
        if (!target || target->state == TASK_EXITED || sched_task_tgid(target) != sched_task_tgid(task))
            return -LINUX_EINVAL;
    }
    struct task *owner = sched_find(sched_task_tgid(task));
    if (!owner) return -LINUX_ESRCH;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    for (int32_t id = 0; id < (int32_t)SCHED_TASK_TIMER_MAX; ++id) {
        if (owner->timers[id].used) continue;
        owner->timers[id] = (struct task_posix_timer){
            .used = 1, .id = (uint32_t)id, .clockid = clockid,
            .notify = notify, .signo = signo,
            .target_pid = notify == 4 ? target_pid : (int32_t)task->pid,
            .value_data = value,
        };
        *timerid = id;
        kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
        return 0;
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return -LINUX_EAGAIN;
}

int sched_timer_delete(struct task *task, int32_t timerid)
{
    uint64_t flags;
    if (!task) return -LINUX_EINVAL;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    struct task_posix_timer *timer = task_timer(task, timerid);
    if (!timer) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
        return -LINUX_EINVAL;
    }
    *timer = (struct task_posix_timer){0};
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return 0;
}

int sched_timer_settime(struct task *task, int32_t timerid, uint32_t flags,
                        const struct linux_itimerspec *value,
                        struct linux_itimerspec *old)
{
    uint64_t lock_flags;
    if (!task || !value || (flags & ~LINUX_TIMER_ABSTIME) ||
        value->it_value.tv_sec < 0 || value->it_value.tv_nsec < 0 ||
        value->it_value.tv_nsec >= 1000000000LL || value->it_interval.tv_sec < 0 ||
        value->it_interval.tv_nsec < 0 || value->it_interval.tv_nsec >= 1000000000LL)
        return -LINUX_EINVAL;
    uint64_t initial = timer_timespec_ticks(&value->it_value);
    uint64_t interval = timer_timespec_ticks(&value->it_interval);
    if (initial == UINT64_MAX || interval == UINT64_MAX) return -LINUX_EINVAL;
    kernel_spin_lock_irqsave(&scheduler_lock, &lock_flags);
    struct task_posix_timer *timer = task_timer(task, timerid);
    if (!timer) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        return -LINUX_EINVAL;
    }
    if (old) {
        uint64_t remaining = timer->expiry_tick > scheduler_ticks ? timer->expiry_tick - scheduler_ticks : 0;
        timer_ticks_timespec(remaining, &old->it_value);
        timer_ticks_timespec(timer->interval_ticks, &old->it_interval);
    }
    timer->interval_ticks = interval;
    timer->overrun = 0;
    timer->expiry_tick = initial ? ((flags & LINUX_TIMER_ABSTIME) ? initial : scheduler_ticks + initial) : 0;
    kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
    return 0;
}

int sched_timer_gettime(struct task *task, int32_t timerid,
                        struct linux_itimerspec *value)
{
    uint64_t flags;
    if (!task || !value) return -LINUX_EINVAL;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    struct task_posix_timer *timer = task_timer(task, timerid);
    if (!timer) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
        return -LINUX_EINVAL;
    }
    uint64_t remaining = timer->expiry_tick > scheduler_ticks ? timer->expiry_tick - scheduler_ticks : 0;
    timer_ticks_timespec(remaining, &value->it_value);
    timer_ticks_timespec(timer->interval_ticks, &value->it_interval);
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return 0;
}

int sched_timer_getoverrun(struct task *task, int32_t timerid)
{
    uint64_t flags;
    if (!task) return -LINUX_EINVAL;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    struct task_posix_timer *timer = task_timer(task, timerid);
    int result = timer ? (timer->overrun > INT32_MAX ? INT32_MAX : (int)timer->overrun) : -LINUX_EINVAL;
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return result;
}

void sched_posix_timer_expire(void)
{
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *owner = tasks[i];
        if (!owner || owner->state == TASK_EXITED) continue;
        for (uint32_t j = 0; j < SCHED_TASK_TIMER_MAX; ++j) {
            struct task_posix_timer *timer = &owner->timers[j];
            if (!timer->used || !timer->expiry_tick || timer->expiry_tick > scheduler_ticks) continue;
            if (timer->notify != 1) {
                uint64_t bit = 1ULL << (uint32_t)(timer->signo - 1);
                if (timer->notify == 4) {
                    /* SIGEV_THREAD_ID is the one notification mode that is
                     * thread-directed on Linux. */
                    struct task *target = sched_find((uint32_t)timer->target_pid);
                    if (target && target->state != TASK_EXITED &&
                        sched_task_tgid(target) == sched_task_tgid(owner)) {
                        if (sched_task_pending(target) & bit) ++timer->overrun;
                        target->timer_pending_signals |= bit;
                        __atomic_fetch_or(&target->pending_signals, bit, __ATOMIC_SEQ_CST);
                        if (target->state == TASK_BLOCKED &&
                            (!target->vfork_child || kernel_signal_fatal_pending(target))) {
                            target->wake_tick = 0;
                            target->state = TASK_READY;
                            sched_request_reschedule_all_online();
                        }
                    }
                } else {
                    /* SIGEV_SIGNAL is process-directed.  Keep it in the
                     * shared process queue so any eligible thread can
                     * receive it, matching kill(2)/Linux signal selection. */
                    uint64_t *pending = sched_task_process_pending(owner);
                    if ((*pending & bit) != 0) ++timer->overrun;
                    owner->timer_pending_signals |= bit;
                    __atomic_fetch_or(pending, bit, __ATOMIC_SEQ_CST);
                    sched_wake_process_signal(sched_task_tgid(owner), bit);
                }
            }
            if (!timer->interval_ticks) {
                timer->expiry_tick = 0;
            } else {
                uint64_t periods = (scheduler_ticks - timer->expiry_tick) / timer->interval_ticks + 1;
                if (periods > 1) timer->overrun += periods - 1;
                timer->expiry_tick += periods * timer->interval_ticks;
            }
        }
    }
}
