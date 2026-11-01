/* Compile the actual userland timer admission; model one synchronous stack call. */
#include <assert.h>
#include <stdio.h>
#include "../../kernel/exec/userland.c"

static struct task task = {.pid=3,.kind=TASK_KIND_USER,.state=TASK_RUNNING};
static uint32_t real_pid = 3, real_cpu;
static bool writer = true, busy, nested;
static unsigned calls, stack_calls;
struct task *sched_current_task(void) { return &task; }
uint32_t sched_current_pid(void) { return real_pid; }
uint32_t smp_current_cpu(void) { return real_cpu; }
bool kernel_execution_is_outermost(void) { return writer; }
bool swap_storage_busy(void) { return busy; }
void memory_pressure_poll(uint32_t budget)
{ assert(nested && budget == 16 && writer && !busy && task.pid == real_pid); ++calls; }
void arch_call_on_stack(void *top, void (*callback)(void *), void *argument)
{
    assert(!nested && top == &sched_idle_service_stacks[real_cpu][SCHED_IDLE_SERVICE_STACK_BYTES]);
    nested = true; ++stack_calls; callback(argument); nested = false;
}

int main(void)
{
    struct trap_frame frame = {.cs=3};
    sched_run_pressure_maintenance(&frame);
    assert(calls == 1 && stack_calls == 1 && !nested);
    sched_run_pressure_maintenance(NULL);
    frame.cs = 0; sched_run_pressure_maintenance(&frame); frame.cs = 3;
    task.pid = 0; sched_run_pressure_maintenance(&frame); task.pid = 3;
    task.kind = TASK_KIND_KERNEL; sched_run_pressure_maintenance(&frame); task.kind = TASK_KIND_USER;
    task.state = TASK_EXITED; sched_run_pressure_maintenance(&frame); task.state = TASK_RUNNING;
    real_pid = 4; sched_run_pressure_maintenance(&frame); real_pid = 3;
    writer = false; sched_run_pressure_maintenance(&frame); writer = true;
    busy = true; sched_run_pressure_maintenance(&frame); busy = false;
    real_cpu = SMP_MAX_CPUS; sched_run_pressure_maintenance(&frame); real_cpu = 0;
    assert(calls == 1 && stack_calls == 1 && !nested);
    puts("PASS actual timer consumer admission and balanced modeled stack callback");
    return 0;
}
