/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "modules/scheduler.h"
#include "sysv_shm_fixture_stubs.h"

/** @brief Supply the fixture's only CPU index. @return CPU zero. */
uint32_t smp_current_cpu(void) { return 0; }
/** @brief Supply the fixture CPU count. @return One. */
uint32_t smp_cpu_count(void) { return 1; }
/** @brief Identify online fixture CPUs. @param cpu Index. @return True for CPU zero. */
bool smp_cpu_online(uint32_t cpu) { return cpu == 0; }
/** @brief Supply fixture uptime. @return Microseconds. */
uint64_t time_uptime_us(void) { return 0; }
/** @brief Enter the fixture scheduler lock. @param lock Lock. @param flags Saved flags. */
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ (void)lock; *flags = 0; }
/** @brief Leave the fixture scheduler lock. @param lock Lock. @param flags Saved flags. */
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)lock; (void)flags; }
/** @brief Allocate fixture metadata. @param size Bytes. @return Allocation. */
void *kernel_malloc(size_t size) { return malloc(size); }
/** @brief Release fixture metadata. @param memory Allocation. */
void kernel_free(void *memory) { free(memory); }
/** @brief This fixture delivers no fatal signal. @param task Candidate. @return Zero. */
int kernel_signal_fatal_pending(const struct task *task) { (void)task; return 0; }

/** @brief Verify real timed blocking, timer wake, and exit exclusion.
 * @return Zero once the blocked task is woken and an exited task stays exited.
 * CPU ownership survives the block until the real frame-scheduling boundary.
 */
int main(void)
{
    task_capacity = 2; task_count = 1;
    tasks = calloc(task_capacity, sizeof(*tasks)); assert(tasks);
    struct sched_task_object *object = calloc(1, sizeof(*object)); assert(object);
    struct task *task = &object->task; tasks[0] = task; task_zero(task);
    task->pid = task->tgid = 123; task->kind = TASK_KIND_USER;
    task->state = TASK_RUNNING; task->running_cpu = 0; task->affinity_mask = 1;
    current_task[0] = task; current_pid[0] = task->pid; scheduler_ticks = 10;
    sched_sleep_current_until(11);
    assert(task->state == TASK_BLOCKED && task->wake_tick == 11 && task->running_cpu == 0);
    sched_on_tick();
    assert(task->state == TASK_READY && !task->wake_tick && task->running_cpu == 0);
    task->state = TASK_RUNNING; sched_sleep_current_until(12);
    assert(task->state == TASK_BLOCKED && task->wake_tick == 12);
    task->state = TASK_EXITED; sched_on_tick();
    assert(task->state == TASK_EXITED);
    free(object); free(tasks); tasks = NULL; task_count = task_capacity = 0;
    puts("PASS production scheduler: timed contention wait wakes, exited task stays exited");
    return 0;
}
