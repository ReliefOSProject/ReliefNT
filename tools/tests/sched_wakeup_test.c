/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#define RELIEFNT_TEST_RESCHEDULE_IPI
#include "modules/scheduler.h"

static uint32_t fixture_cpu, fixture_cpus = 2;
static uint64_t online = 3;
static unsigned ipis[SMP_MAX_CPUS];
uint32_t smp_current_cpu(void) { return fixture_cpu; }
uint32_t smp_cpu_count(void) { return fixture_cpus; }
bool smp_cpu_online(uint32_t cpu) { return cpu < 64 && (online & (1ULL << cpu)); }
void smp_send_reschedule(uint32_t cpu)
{
    assert(cpu < SMP_MAX_CPUS && cpu != fixture_cpu && smp_cpu_online(cpu));
    assert(sched_resched_pending[cpu]);
    ++ipis[cpu];
}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ (void)lock; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)lock; (void)flags; }

int main(void)
{
    /* A wakeup must interrupt remote CPUs without waiting for a timer. */
    sched_notify_runnable();
    assert(ipis[0] == 0 && ipis[1] == 1);
    sched_notify_runnable();
    assert(ipis[1] == 1); /* Coalesce until the destination consumes its flag. */
    sched_resched_pending[0] = 0;
    fixture_cpu = 1;
    sched_notify_runnable();
    assert(ipis[0] == 1 && ipis[1] == 1);
    fixture_cpu = 0;
    online = 1;
    sched_resched_pending[1] = 0;
    sched_notify_runnable();
    assert(ipis[1] == 1 && !sched_resched_pending[1]);

    online = 3;
    struct task task = {.pid = 7, .state = TASK_BLOCKED,
                        .running_cpu = 1, .wake_tick = 123};
    sched_wake_interruptible(&task);
    assert(task.state == TASK_READY && !task.wake_tick && task.running_cpu == 1);
    assert(ipis[1] == 2); /* A still-live frame keeps its CPU reservation. */
    sched_wake_interruptible(&task);
    assert(ipis[1] == 2);
    task.state = TASK_STOPPED;
    sched_wake_interruptible(&task);
    assert(task.state == TASK_STOPPED && ipis[1] == 2);
    task.state = TASK_EXITED;
    sched_wake_interruptible(&task);
    assert(task.state == TASK_EXITED && ipis[1] == 2);
    task.state = TASK_BLOCKED;
    task.vfork_child = &task;
    sched_wake_interruptible(&task);
    assert(task.state == TASK_BLOCKED && ipis[1] == 2);

    sched_resched_pending[1] = 0;
    sched_on_cpu_tick();
    assert(!sched_resched_pending[1] && ipis[1] == 2); /* Local tick never broadcasts. */
    fixture_cpus = 1;
    sched_notify_runnable();
    assert(ipis[1] == 2);
    struct task *table[]={&task};tasks=table;task_count=1;
    task.affinity_mask=1;task.kind=TASK_KIND_KERNEL;
    assert(sched_set_task_affinity(7,1)==-22 && task.affinity_mask==1);
    task.kind=TASK_KIND_USER;assert(!sched_set_task_affinity(7,1));
    tasks=NULL;task_count=0;
    puts("PASS scheduler wake: remote interrupt, coalescing, offline/UP, ownership and stopped/vfork state");
}
