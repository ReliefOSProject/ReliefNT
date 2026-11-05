/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/sched_kernel.h>

/** @brief Read fairness time without letting a delayed PIT extend a user budget.
 * @return Nondecreasing scheduler microseconds. Execution-owned dispatcher
 * context, no scheduler lock held; the short clock sample acquires that lock.
 */
uint64_t sched_kernel_clock_us(void)
{
    uint64_t flags;kernel_spin_lock_irqsave(&scheduler_lock,&flags);
    uint64_t now=fair_now();kernel_spin_unlock_irqrestore(&scheduler_lock,flags);return now;
}
/** @brief Publish a kernel thread state without racing the timer/proc readers.
 * @param task Owned stable kernel record. @param state Internal task state.
 * IRQ/task safe, never acquires the execution transaction or calls a provider.
 */
void sched_kernel_task_state(struct task *task,unsigned state)
{
    uint64_t flags;kernel_spin_lock_irqsave(&scheduler_lock,&flags);
    task->state=(enum task_state)state;
    kernel_spin_unlock_irqrestore(&scheduler_lock,flags);
}
/** @brief Switch CPU identity/accounting to a retained kernel context.
 * @param task Owned kernel record. @param context Saved reserved user/idle slot.
 * IRQ-disabled depth-one execution writer. The old task keeps its CPU
 * reservation, preventing reaping or concurrent user entry while its caller's
 * stack is suspended. GPR-only kernel contexts leave user FPU state untouched.
 */
void sched_kernel_context_enter(struct task *task,struct kernel_sched_context *context)
{
    uint32_t cpu=scheduler_cpu_index();uint64_t flags;
    __asm__ volatile("mov %%cr3,%0":"=r"(context->cr3));
    context->fs=arch_get_user_fs();
    storage_kernel_context_enter(&context->storage);
    paging_load_cr3_if_changed(paging_kernel_cr3());arch_set_user_fs(0);
    kernel_spin_lock_irqsave(&scheduler_lock,&flags);
    context->previous=current_task[cpu];context->kernel_mode=cpu_accounting[cpu].kernel_mode;
    context->restore_fair=fair_running[cpu]!=NULL;
    fair_stop_cpu(cpu);cpu_accounting_settle(cpu);
    current_task[cpu]=task;current_pid[cpu]=task->pid;
    task->state=TASK_RUNNING;task->running_cpu=cpu;task->last_cpu=cpu;
    cpu_accounting[cpu].kernel_mode=true;++proc_switches;
    kernel_spin_unlock_irqrestore(&scheduler_lock,flags);
}
/** @brief Restore a reserved scheduler caller after a private kernel stack yields.
 * @param task Exact running kernel owner. @param context Matching saved slot.
 * Caller holds IRQ-disabled execution ownership; no old reservation was freed.
 */
void sched_kernel_context_leave(struct task *task,struct kernel_sched_context *context)
{
    uint32_t cpu=scheduler_cpu_index();uint64_t flags;
    storage_kernel_context_leave(&context->storage);
    paging_load_cr3_if_changed(context->cr3);arch_set_user_fs(context->fs);
    kernel_spin_lock_irqsave(&scheduler_lock,&flags);cpu_accounting_settle(cpu);
    task->running_cpu=SCHED_CPU_NONE;
    current_task[cpu]=context->previous;current_pid[cpu]=context->previous ? context->previous->pid : 0;
    cpu_accounting[cpu].kernel_mode=context->kernel_mode;
    if(context->restore_fair && context->previous && context->previous->state==TASK_RUNNING) {
        fair_running[cpu]=context->previous;fair_started[cpu]=fair_now();
    }
    ++proc_switches;kernel_spin_unlock_irqrestore(&scheduler_lock,flags);
}
/** @brief Release a joined kernel record for later stable-slot reuse.
 * @param task Owned record with no live stack/CPU; outer execution protects it.
 * No user MM/files exist, and no module code or scheduler callback is entered.
 */
void sched_destroy_kernel_task(struct task *task)
{
    uint64_t flags;kernel_spin_lock_irqsave(&scheduler_lock,&flags);
    if(task && task->kind==TASK_KIND_KERNEL && task->running_cpu==SCHED_CPU_NONE) {
        task->state=TASK_EXITED;task->stack_low=task->stack_top=0;
        task->flags|=TASK_FLAG_RESOURCES_RELEASED;
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock,flags);
}
