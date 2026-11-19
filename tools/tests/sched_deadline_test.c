/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "modules/scheduler.h"

static struct task members[3];
static struct task *table[3];

uint32_t smp_current_cpu(void) { return 0; }
uint32_t smp_cpu_count(void) { return 2; }
bool smp_cpu_online(uint32_t cpu) { return cpu < 2; }
uint64_t time_uptime_us(void) { return scheduler_ticks * 10000; }
void kernel_spin_init(struct kernel_spinlock *lock) { lock->state = 0; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state = 0; }
int kernel_signal_fatal_pending(const struct task *task)
{ (void)task; return 0; }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }

static void reset_world(void)
{
    sched_init();
    memset(members, 0, sizeof(members));
    tasks = table;
    task_count = 3;
    for (unsigned i = 0; i < 3; ++i) {
        table[i] = &members[i];
        members[i].pid = members[i].tgid = i + 1;
        members[i].state = TASK_RUNNING;
        members[i].kind = TASK_KIND_USER;
        members[i].running_cpu = i ? SCHED_CPU_NONE : 0;
    }
    current_task[0] = &members[0];
    current_pid[0] = members[0].pid;
    sched_on_tick(); /* Establish empty hints through the production scanner. */
    assert(scheduler_next_wake_tick == UINT64_MAX);
    assert(scheduler_next_alarm_tick == UINT64_MAX);
    assert(scheduler_next_posix_timer_tick == UINT64_MAX);
}

static void tick_to(uint64_t tick)
{
    assert(tick >= scheduler_ticks);
    while (scheduler_ticks < tick) sched_on_tick();
}

static void test_sleep_deadlines(void)
{
    reset_world();
    sched_sleep_current_until(20);
    assert(scheduler_next_wake_tick == 20);
    current_task[0] = &members[1]; current_pid[0] = members[1].pid;
    members[1].running_cpu = 0;
    sched_sleep_current_until(5);
    assert(scheduler_next_wake_tick == 5);
    tick_to(4);
    assert(members[0].state == TASK_BLOCKED && members[1].state == TASK_BLOCKED);
    tick_to(5);
    assert(members[1].state == TASK_READY && members[1].running_cpu == 0);
    assert(members[0].state == TASK_BLOCKED && scheduler_next_wake_tick == 20);
    sched_wake_interruptible(&members[0]);
    assert(members[0].state == TASK_READY && !members[0].wake_tick);
    tick_to(20); /* Stale cancellation hint only triggers a harmless scan. */
    assert(scheduler_next_wake_tick == UINT64_MAX);

    current_task[0] = &members[2]; current_pid[0] = members[2].pid;
    members[2].running_cpu = 0;
    sched_signal_wait_current(25);
    assert(members[2].state == TASK_BLOCKED && scheduler_next_wake_tick == 25);
    tick_to(25);
    assert(members[2].state == TASK_READY && !members[2].wake_tick);
    members[2].pending_signals = 1;
    sched_signal_wait_current(30);
    assert(members[2].state == TASK_READY && !members[2].wake_tick);
    tick_to(30);
    assert(scheduler_next_wake_tick == UINT64_MAX);
    sched_sleep_current_until(0);
    tick_to(40);
    assert(members[2].state == TASK_BLOCKED && !members[2].wake_tick);
}

static void test_alarm_deadlines(void)
{
    reset_world();
    /* Ignored disposition still expires/rearms, without signal side effects. */
    for (unsigned i = 0; i < 3; ++i) members[i].signal_actions[14].handler = 1;
    struct linux_itimerval one = {.it_value.tv_usec = 50000};
    sched_itimer_task(&members[0], &one, NULL);
    assert(members[0].alarm_deadline == 6 && scheduler_next_alarm_tick == 6);
    one.it_value.tv_usec = 20000;
    sched_itimer_task(&members[1], &one, NULL);
    assert(scheduler_next_alarm_tick == 3);
    tick_to(3);
    assert(!members[1].alarm_deadline && members[0].alarm_deadline == 6);
    assert(scheduler_next_alarm_tick == 6);
    struct linux_itimerval disarm = {0};
    sched_itimer_task(&members[0], &disarm, NULL);
    tick_to(6);
    assert(scheduler_next_alarm_tick == UINT64_MAX);

    one = (struct linux_itimerval){.it_value.tv_usec = 10000,
                                  .it_interval.tv_usec = 30000};
    sched_itimer_task(&members[0], &one, NULL);
    tick_to(7);
    assert(!members[0].alarm_deadline);
    tick_to(15);
    sched_alarm_rearm(&members[0]);
    assert(members[0].alarm_deadline == 16 && scheduler_next_alarm_tick == 16);
    tick_to(16);
    assert(!members[0].alarm_deadline);
}

static void test_vfork_deadline_retirement(void)
{
    reset_world();
    sched_sleep_current_until(5);
    struct task *parent = &members[0], *child = &members[1];
    parent->vfork_child = child;
    child->vfork_parent = parent;
    /* A stale timeout hint must not wake a vfork parent. Real clone clears
     * wake_tick; retaining it here also exercises the scanner's exclusion. */
    tick_to(5);
    assert(parent->state == TASK_BLOCKED && parent->vfork_child == child);
    assert(scheduler_next_wake_tick == UINT64_MAX);
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    vfork_child_done_locked(child);
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    assert(!parent->vfork_child && !parent->wake_tick && parent->state == TASK_READY);
    assert(!child->vfork_parent && parent->running_cpu == 0);
    sched_sleep_current_until(8);
    assert(scheduler_next_wake_tick == 8);
    tick_to(8);
    assert(parent->state == TASK_READY && !parent->wake_tick);

    parent->state = TASK_STOPPED;
    parent->vfork_child = child;
    child->vfork_parent = parent;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    vfork_child_done_locked(child);
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    assert(parent->state == TASK_STOPPED && !parent->vfork_child);
}

static void test_posix_timer_deadlines(void)
{
    reset_world();
    int32_t a, b;
    assert(!sched_timer_create(&members[0], LINUX_CLOCK_MONOTONIC, 1, 0, 0, 0, &a));
    assert(!sched_timer_create(&members[1], LINUX_CLOCK_MONOTONIC, 1, 0, 0, 0, &b));
    struct linux_itimerspec spec = {.it_value.tv_nsec = 50000000,
                                   .it_interval.tv_nsec = 30000000};
    assert(!sched_timer_settime(&members[0], a, 0, &spec, NULL));
    assert(scheduler_next_posix_timer_tick == 6);
    spec = (struct linux_itimerspec){.it_value.tv_nsec = 20000000};
    assert(!sched_timer_settime(&members[1], b, 0, &spec, NULL));
    assert(scheduler_next_posix_timer_tick == 3);
    tick_to(3);
    assert(!members[1].timers[b].expiry_tick && scheduler_next_posix_timer_tick == 6);
    tick_to(6);
    assert(members[0].timers[a].expiry_tick == 9 && scheduler_next_posix_timer_tick == 9);
    scheduler_ticks = 16; /* A delayed scan retains periodic catch-up semantics. */
    sched_posix_timer_expire();
    assert(members[0].timers[a].expiry_tick == 18 && members[0].timers[a].overrun == 2);
    assert(scheduler_next_posix_timer_tick == 18);
    assert(!sched_timer_delete(&members[0], a));
    tick_to(18);
    assert(scheduler_next_posix_timer_tick == UINT64_MAX);

    assert(!sched_timer_create(&members[2], LINUX_CLOCK_MONOTONIC, 1, 0, 0, 0, &a));
    spec = (struct linux_itimerspec){.it_value.tv_nsec = 10000000};
    assert(!sched_timer_settime(&members[2], a, 0, &spec, NULL));
    members[2].state = TASK_EXITED;
    tick_to(19);
    assert(scheduler_next_posix_timer_tick == UINT64_MAX);

    members[2].state = TASK_RUNNING;
    current_task[0] = &members[2]; current_pid[0] = members[2].pid;
    members[2].running_cpu = 0;
    sched_sleep_current_until(100);
    assert(!sched_timer_create(&members[2], LINUX_CLOCK_MONOTONIC, 4, 10,
                               (int32_t)members[2].pid, 0, &b));
    assert(!sched_timer_settime(&members[2], b, 0, &spec, NULL));
    tick_to(20);
    assert(members[2].state == TASK_READY && !members[2].wake_tick);
    assert(members[2].pending_signals & (1ULL << 9));
    assert(members[2].running_cpu == 0);
    assert(scheduler_next_posix_timer_tick == UINT64_MAX);

    /* An absolute deadline in the past must tighten an empty hint, too. */
    assert(!sched_timer_create(&members[0], LINUX_CLOCK_MONOTONIC, 1, 0, 0, 0, &a));
    spec = (struct linux_itimerspec){.it_value.tv_nsec = 10000000};
    assert(!sched_timer_settime(&members[0], a, LINUX_TIMER_ABSTIME, &spec, NULL));
    assert(scheduler_next_posix_timer_tick == 1);
    tick_to(21);
    assert(!members[0].timers[a].expiry_tick);
}

static void test_no_due_deadline_does_not_scan(void)
{
    reset_world();
    current_task[0] = NULL; current_pid[0] = 0;
    void *guard = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(guard != MAP_FAILED);
    /* Any table dereference faults, proving that empty/future timer windows
     * do not walk sleeping tasks, alarms or all 32 POSIX timer slots. */
    tasks = guard;
    task_count = 4096;
    scheduler_ticks = 100; /* Avoid the independent five-second load sample. */
    scheduler_next_wake_tick = 200;
    scheduler_next_alarm_tick = 200;
    scheduler_next_posix_timer_tick = 200;
    sched_on_tick();
    assert(scheduler_ticks == 101);
    scheduler_next_wake_tick = UINT64_MAX;
    scheduler_next_alarm_tick = UINT64_MAX;
    scheduler_next_posix_timer_tick = UINT64_MAX;
    sched_on_tick();
    tasks = table; task_count = 3;
    assert(!munmap(guard, 4096));
    puts("PASS no-due tick: zero task-table accesses versus 139264 timeout checks for 4096 tasks");
}

int main(void)
{
    test_sleep_deadlines();
    test_alarm_deadlines();
    test_vfork_deadline_retirement();
    test_posix_timer_deadlines();
    test_no_due_deadline_does_not_scan();
    puts("PASS scheduler deadline hints: earlier insertion, cancellation, periodic catch-up, exit, signal waits, CPU reservations");
    return 0;
}
