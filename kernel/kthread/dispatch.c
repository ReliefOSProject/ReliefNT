/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
static uint64_t user_dispatch_after[SMP_MAX_CPUS];
static uint64_t user_dispatch_started[SMP_MAX_CPUS];

/** @brief Check this CPU's ready-context hint without touching private stacks.
 * @return True if at least one bound context may be dispatched. */
bool kthread_runnable(void)
{uint32_t cpu=smp_current_cpu();return cpu<SMP_MAX_CPUS && __atomic_load_n(&kthread_ready_count[cpu],__ATOMIC_ACQUIRE)!=0;}
/** @brief Wake expired sleepers without acquiring the execution writer.
 * Timer/IRQ safe; the retained thread stack is not entered here. */
void kthread_tick(void)
{
    uint64_t flags;bool woke=false;kernel_spin_lock_irqsave(&kthread_lock,&flags);
    uint64_t now=time_ticks();
    for(struct kthread *t=kthread_all;t;t=t->all_next)
        if(t->started && !t->finished && !t->running && !t->queued && t->deadline &&
           t->deadline!=UINT64_MAX && t->deadline<=now) {
            t->deadline=0;kthread_enqueue_locked(t);woke=true;
        }
    kernel_spin_unlock_irqrestore(&kthread_lock,flags);if(woke)sched_notify_runnable();
}
/** @brief Save one running thread while keeping wake/park publication atomic.
 * @param sleep True for predicate/event sleep. @param deadline Absolute tick.
 * @param irq_return True only after a safe acknowledged interrupt.
 * @return Zero on resume or EPERM/EWOULDBLOCK before changing any state. */
static int park_current(bool sleep,uint64_t deadline,bool irq_return)
{
    struct kthread *t=kthread_current();if(!t)return -EPERM;
    uint64_t saved=kernel_irq_save();
    if((!irq_return && !(saved&512)) || kernel_preempt_disabled()) {
        kernel_irq_restore(saved);return -EWOULDBLOCK;
    }
    uint64_t flags;kernel_spin_lock_irqsave(&kthread_lock,&flags);
    if(sleep && (t->wake_pending || t->stop || deadline<=time_ticks())) {
        t->wake_pending=false;kernel_spin_unlock_irqrestore(&kthread_lock,flags);
        kernel_irq_restore(saved);return 0;
    }
    sched_kernel_task_state(t->task,sleep ? TASK_BLOCKED : TASK_READY);t->deadline=sleep ? deadline : 0;
    kernel_spin_unlock_irqrestore(&kthread_lock,flags);
    arch_kthread_switch(&t->rsp,&kthread_dispatch_sp[t->cpu]);
    kernel_irq_restore(saved);return 0;
}
/** @brief Yield an unlocked kernel continuation to this CPU's dispatcher.
 * @return Zero on resume or an unsafe/non-thread context errno. */
int kthread_yield(void){return park_current(false,0,false);}
/** @brief Sleep a retained continuation without losing an earlier wake.
 * @param deadline Absolute tick or UINT64_MAX. @return Zero or context errno. */
int kthread_sleep_until(uint64_t deadline){return park_current(true,deadline,false);}
/** @brief Enter one admitted module function and retire its returned stack.
 * Core trampoline never returns; the dispatcher publishes final completion
 * only after this stack is no longer active, before dropping the execution pin.
 */
__attribute__((noreturn)) void kthread_entry(void)
{
    struct kthread *t=kthread_current();kernel_irq_restore(512);
    int result=t->function(t->context);
    (void)kernel_irq_save();uint64_t flags;kernel_spin_lock_irqsave(&kthread_lock,&flags);
    t->result=result;t->finished=true;sched_kernel_task_state(t->task,TASK_EXITED);
    kernel_spin_unlock_irqrestore(&kthread_lock,flags);
    arch_kthread_switch(&t->rsp,&kthread_dispatch_sp[t->cpu]);
    __builtin_trap();
}
/** @brief Dispatch retained stacks while restoring scheduler writer ownership.
 * @param budget Maximum contexts. @return Number of turns completed.
 * IRQ-disabled depth-one writer required; no lock crosses a context switch.
 */
uint32_t kthread_dispatch(uint32_t budget)
{
    if(!kernel_execution_is_outermost() || kthread_current())return 0;
    uint32_t cpu=smp_current_cpu(),count=0;if(cpu>=SMP_MAX_CPUS)return 0;
    while(count<budget) {
        uint64_t flags;kernel_spin_lock_irqsave(&kthread_lock,&flags);
        struct kthread *t=kthread_ready[cpu];
        if(t){kthread_unqueue_locked(t);t->running=true;t->started=true;}
        kernel_spin_unlock_irqrestore(&kthread_lock,flags);if(!t)break;
        struct kernel_sched_context previous={0};sched_kernel_context_enter(t->task,&previous);
        __atomic_store_n(&kthread_active[cpu],t,__ATOMIC_RELEASE);
        struct kernel_execution_suspend_token transaction={0};
        if(kernel_execution_suspend_irqrestore(&transaction))__builtin_trap();
        (void)kernel_irq_save();
        arch_kthread_switch(&kthread_dispatch_sp[cpu],&t->rsp);
        if(kernel_execution_resume_irqsave(&transaction))__builtin_trap();
        sched_kernel_context_leave(t->task,&previous);
        __atomic_store_n(&kthread_active[cpu],NULL,__ATOMIC_RELEASE);
        kernel_spin_lock_irqsave(&kthread_lock,&flags);t->running=false;
        if(t->finished) {
            if(t->admitted){module_execution_put(t->owner);t->admitted=false;}
            __atomic_store_n(&t->exited,true,__ATOMIC_RELEASE);
        } else if(t->task->state==TASK_READY || t->wake_pending || t->stop) {
            /* A wake only satisfies an already-published sleep. Preserve it
             * across an intervening IRQ/yield before the sleep predicate. */
            if(t->task->state==TASK_BLOCKED)t->wake_pending=false;
            t->deadline=0;kthread_enqueue_locked(t);
        }
        kernel_spin_unlock_irqrestore(&kthread_lock,flags);++count;
    }
    return count;
}
/** @brief Reserve bounded user progress across repeated fault/syscall returns.
 * @param budget Maximum turns. @return Completed turns or zero while cooling.
 * Execution-owned and IRQ-disabled, so the bound CPU deadline needs no lock.
 * A delayed BSP/PIT must not stall other CPUs' workers beyond one scheduler
 * clock tick. Explicit drain/idle callers bypass this user-class reservation.
 */
uint32_t kthread_dispatch_user(uint32_t budget)
{
    uint32_t cpu=smp_current_cpu();
    if(cpu>=SMP_MAX_CPUS)return 0;
    if(time_ticks()<user_dispatch_after[cpu]) {
        uint64_t now=sched_kernel_clock_us(),start=user_dispatch_started[cpu];
        if(now>=start && now-start<1000000ULL/RELIEFNT_TICK_HZ)return 0;
    }
    uint32_t turns=kthread_dispatch(budget);
    if(turns){uint64_t now=time_ticks();user_dispatch_after[cpu]=now==UINT64_MAX ? now : now+1;
        user_dispatch_started[cpu]=sched_kernel_clock_us();}
    return turns;
}
/** @brief Preempt only a private unlocked thread stack after timer/reschedule EOI.
 * @param frame Borrowed CPL0 frame. @return True after permitted switch/resume.
 * Kernel writers, spinlocks, masked IRQs and foreign stacks retain ownership.
 */
bool kthread_preempt(struct trap_frame *frame)
{
    struct kthread *t=kthread_current();if(!t || !frame || (frame->cs&3) || !(frame->rflags&512) || kernel_preempt_disabled())return false;
    uintptr_t address=(uintptr_t)frame;
    if(address<t->task->stack_low || address>t->task->stack_top-sizeof(*frame))return false;
    return park_current(false,0,true)==0;
}
