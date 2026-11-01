/* Real pressure wait, intrusive queue, scheduler block/wake and exit cleanup.
 * Reclaim outcomes and CPU/clock/spin boundaries are explicit host models. */
#define PRESSURE_WAIT_FIXTURE 1
#define main old_exit_logging_main
#include "task_exit_logging_test.c"
#undef main
#include "../../mm/reclaim/internal.h"
#include <reliefnt/memory_pressure.h>
#include <linux/errno.h>

static unsigned queue_depth;
static uint64_t free_pages = 64, ticks = 100;
static bool outer = true;
static int reclaim_result = -EAGAIN;
static unsigned reclaim_calls;
static enum pressure_stall_cause stall_cause;

void kernel_spin_init(struct kernel_spinlock *lock) { lock->state = 0; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    *flags = interrupts_enabled; interrupts_enabled = false;
    if (lock == &scheduler_lock) { assert(!scheduler_lock_depth); scheduler_lock_depth = 1; }
    else { assert(!queue_depth && !scheduler_lock_depth); queue_depth = 1; }
}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    if (lock == &scheduler_lock) { assert(scheduler_lock_depth); scheduler_lock_depth = 0; }
    else { assert(queue_depth && !scheduler_lock_depth); queue_depth = 0; }
    interrupts_enabled = flags != 0;
}
bool kernel_execution_is_outermost(void) { return outer; }
uint64_t time_ticks(void) { return ticks; }
uint64_t mm_free_memory_kib(void) { return free_pages * 4; }
uint64_t mm_emergency_reserve_pages(void) { return 64; }
int memory_try_reclaim_fault(uint32_t pages)
{ assert(pages == 1 && outer && !scheduler_lock_depth && !queue_depth); ++reclaim_calls; return reclaim_result; }
int memory_try_reclaim_no_oom(uint64_t pages)
{ assert(pages > 5 && outer && !scheduler_lock_depth && !queue_depth); ++reclaim_calls; return reclaim_result; }
enum pressure_stall_cause memory_reclaim_stall_cause(void) { return stall_cause; }

static void run_as(struct task *task)
{
    current_pid[0] = task->pid; current_task[0] = task;
    task->state = TASK_RUNNING; task->running_cpu = 0;
}

int main(void)
{
    struct task *a = fixture_task(61, "pressure-a");
    struct task *b = fixture_task(62, "pressure-b");
    struct task *table[] = {a, b}; install_tasks(table, 2);
    run_as(a);
    assert(memory_fault_oom(a, 0x5000) == -EAGAIN);
    assert(a->state == TASK_BLOCKED && a->wake_tick == ticks + 1 && a->waiting_queue);
    memory_pressure_notify();
    assert(a->state == TASK_READY && !a->waiting_queue && !a->wake_tick);
    run_as(a);
    assert(memory_fault_oom(a, 0x5000) == -EAGAIN && a->waiting_queue);
    run_as(a); reclaim_result = 0;
    assert(!memory_fault_oom(a, 0x5000) && !a->waiting_queue);

    reclaim_result = -EAGAIN; stall_cause = PRESSURE_STALL_STORAGE;
    for (unsigned i = 0; i < 12; ++i) {
        run_as(a); assert(memory_fault_oom(a, 0x6000) == -EAGAIN && a->waiting_queue);
    }
    memory_wait_cancel(a);
    for (unsigned i = 0; i < 500; ++i) {
        ++ticks; run_as(a); assert(memory_fault_oom(a, 0x6000) == -EAGAIN && a->waiting_queue);
    }
    ++ticks;
    run_as(a); assert(memory_fault_oom(a, 0x6000) == -EBUSY && !a->waiting_queue);
    unsigned before_reclaim = reclaim_calls;
    for (unsigned i = 0; i < 500; ++i) {
        ++ticks; run_as(a); assert(memory_fault_storage_wait(a, 0x6100) == -EAGAIN && a->waiting_queue);
    }
    ++ticks;
    run_as(a); assert(memory_fault_storage_wait(a, 0x6100) == -EBUSY && !a->waiting_queue);
    assert(reclaim_calls == before_reclaim);
    stall_cause = PRESSURE_STALL_VICTIM;
    for (unsigned i = 0; i < 3000; ++i) {
        ++ticks; run_as(a); assert(memory_fault_oom(a, 0x7000) == -EAGAIN && a->waiting_queue);
    }
    ++ticks;
    run_as(a); assert(memory_fault_oom(a, 0x7000) == -ENOMEM && !a->waiting_queue);

    stall_cause = PRESSURE_STALL_NONE;
    run_as(a); assert(memory_fault_oom(a, 0x8000) == -EAGAIN && a->waiting_queue);
    run_as(b); assert(memory_fault_oom(b, 0x9000) == -EAGAIN && b->waiting_queue);
    a->state = TASK_EXITED; a->running_cpu = SCHED_CPU_NONE;
    sched_release_task_resources(a);
    assert((a->flags & TASK_FLAG_RESOURCES_RELEASED) && !a->waiting_queue);
    assert(b->state == TASK_READY && !b->waiting_queue);
    assert(!scheduler_lock_depth && !queue_depth);
    free(a); free(b);
    puts("PASS actual queue sleep/wake, bounded storage/victim waits and quiescent exit notification");
    return 0;
}
