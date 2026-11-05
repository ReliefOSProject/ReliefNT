/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>
#include <reliefnt/export.h>

struct kernel_spinlock kthread_lock=KERNEL_SPINLOCK_INIT;
struct kthread *kthread_all,*kthread_ready[SMP_MAX_CPUS],*kthread_tail[SMP_MAX_CPUS];
struct kthread *kthread_active[SMP_MAX_CPUS];
uint32_t kthread_ready_count[SMP_MAX_CPUS];
uint64_t kthread_dispatch_sp[SMP_MAX_CPUS];

/** @brief Append an eligible context to its bound CPU FIFO under kthread_lock.
 * @param thread Stable non-running handle; duplicates and finished work ignored. */
void kthread_enqueue_locked(struct kthread *thread)
{
    if(thread->queued || thread->running || thread->finished)return;
    uint32_t cpu=thread->cpu;thread->ready_next=NULL;
    if(kthread_tail[cpu])kthread_tail[cpu]->ready_next=thread;else kthread_ready[cpu]=thread;
    kthread_tail[cpu]=thread;thread->queued=true;sched_kernel_task_state(thread->task,TASK_READY);
    __atomic_fetch_add(&kthread_ready_count[cpu],1,__ATOMIC_RELEASE);
}
/** @brief Remove exactly one ready context while retaining its module pin.
 * @param thread Stable handle, kthread_lock held and IRQs disabled. */
void kthread_unqueue_locked(struct kthread *thread)
{
    if(!thread->queued)return;
    uint32_t cpu=thread->cpu;struct kthread **at=&kthread_ready[cpu],*previous=NULL;
    while(*at && *at!=thread){previous=*at;at=&(*at)->ready_next;}
    if(*at){*at=thread->ready_next;if(kthread_tail[cpu]==thread)kthread_tail[cpu]=previous;}
    thread->ready_next=NULL;thread->queued=false;
    __atomic_fetch_sub(&kthread_ready_count[cpu],1,__ATOMIC_RELEASE);
}
/** @brief Borrow the CPU's current retained kernel context without taking a pin.
 * @return Thread or NULL; caller must not retain it across scheduling. */
struct kthread *kthread_current(void)
{ uint32_t cpu=smp_current_cpu();return cpu<SMP_MAX_CPUS ? __atomic_load_n(&kthread_active[cpu],__ATOMIC_ACQUIRE) : NULL; }
/** @brief Read a cooperative stop request in its owning thread context.
 * @return True only for a current thread whose stop request is published. */
bool kthread_should_stop(void)
{struct kthread *t=kthread_current();return t && __atomic_load_n(&t->stop,__ATOMIC_ACQUIRE);}
/** @brief Wake a stable context with LIVE admission and a retained wake event.
 * @param thread Stable handle. @return True on admission; IRQ/task safe. */
bool kthread_wake(struct kthread *thread)
{
    if(!thread)return false;
    uint64_t flags;kernel_spin_lock_irqsave(&kthread_lock,&flags);
    bool accepted=!thread->closed && !thread->finished;
    if(accepted && !thread->admitted) {
        accepted=module_execution_get(thread->owner);if(accepted)thread->admitted=true;
    }
    if(accepted){if(thread->started)thread->wake_pending=true;thread->deadline=0;kthread_enqueue_locked(thread);}
    kernel_spin_unlock_irqrestore(&kthread_lock,flags);
    if(accepted)sched_notify_runnable();return accepted;
}
/** @brief Stop/join a ledger-owned thread before its module can release code.
 * @param context Core-owned handle. @return Zero or a safe drain errno. */
static int thread_quiesce(void *context)
{
    struct kthread *thread=context;uint64_t flags;
    kernel_spin_lock_irqsave(&kthread_lock,&flags);thread->closed=true;
    kernel_spin_unlock_irqrestore(&kthread_lock,flags);return kthread_stop(thread,NULL);
}
/** @brief Retire a fully joined handle and its scheduler/stack allocations.
 * @param context Quiescent owned handle; all external producers stopped. */
static void thread_release(void *context)
{
    struct kthread *thread=context;uint64_t execution,flags;
    kernel_execution_lock_irqsave(&execution);
    kernel_spin_lock_irqsave(&kthread_lock,&flags);
    struct kthread **at=&kthread_all;while(*at && *at!=thread)at=&(*at)->all_next;
    if(*at)*at=thread->all_next;
    kernel_spin_unlock_irqrestore(&kthread_lock,flags);
    sched_destroy_kernel_task(thread->task);kernel_free(thread->stack);kernel_free(thread);
    kernel_execution_unlock_irqrestore(execution);
}
/** @brief Allocate a stopped retained-stack thread and register owner cleanup.
 * @param function Required entry. @param context Borrowed argument until join.
 * @param name Required task name. @param owner COMING/LIVE owner or NULL.
 * @param cpu Discovered fixed CPU. @param out Required owned handle output.
 * @return Zero or validation/allocation/ledger errno. No partial publication. */
int kthread_create(kthread_fn function,void *context,const char *name,struct module *owner,uint32_t cpu,struct kthread **out)
{
    if(!out)return -EINVAL;*out=NULL;
    if(!function || !name || cpu>=SMP_MAX_CPUS || cpu>=smp_cpu_count())return -EINVAL;
    struct kthread *thread=kernel_malloc(sizeof(*thread));if(!thread)return -ENOMEM;
    *thread=(struct kthread){.function=function,.context=context,.owner=owner,.cpu=cpu};
    thread->stack=kernel_malloc(65536+16);if(!thread->stack){kernel_free(thread);return -ENOMEM;}
    uint64_t low=((uintptr_t)thread->stack+15)&~15ULL,top=low+65536;
    uint64_t *initial=(uint64_t *)(uintptr_t)(top-64);
    for(unsigned i=0;i<8;++i)initial[i]=0;
    initial[6]=(uintptr_t)kthread_entry;thread->rsp=(uintptr_t)initial;
    uint64_t execution;kernel_execution_lock_irqsave(&execution);
    thread->task=sched_create_kernel_task(name,(uintptr_t)function,low,top,cpu);
    int rc=thread->task ? 0 : -ENOMEM;
    if(!rc && owner)rc=module_add_resource(owner,thread_quiesce,thread_release,thread);
    if(rc){if(thread->task)sched_destroy_kernel_task(thread->task);kernel_free(thread->stack);kernel_free(thread);}
    else {
        uint64_t flags;kernel_spin_lock_irqsave(&kthread_lock,&flags);
        thread->all_next=kthread_all;kthread_all=thread;
        kernel_spin_unlock_irqrestore(&kthread_lock,flags);*out=thread;
    }
    kernel_execution_unlock_irqrestore(execution);return rc;
}
/** @brief Progress local retained contexts during an explicit unlocked wait.
 * @return Zero or unsafe-context errno; never runs ordinary syscall maintenance. */
int kthread_wait_tick(void)
{
    uint64_t flags=kernel_irq_save();kernel_irq_restore(flags);
    if(!(flags&512) || kernel_preempt_disabled())return -EWOULDBLOCK;
    if(kthread_current())return kthread_sleep_until(time_ticks()+1);
    uint64_t execution;kernel_execution_lock_irqsave(&execution);
    uint32_t turns=kthread_dispatch(1);kernel_execution_unlock_irqrestore(execution);
    smp_membarrier_poll();
    if(!turns){flags=kernel_irq_save();arch_kthread_idle();kernel_irq_restore(flags);}
    return 0;
}
/** @brief Join a stable handle after its stack and module entry have retired.
 * @param thread Stable handle. @param result Optional exit code.
 * @return Zero or validation/context/self-wait errno. Suspends one writer. */
int kthread_join(struct kthread *thread,int *result)
{
    if(!thread)return -EINVAL;if(thread==kthread_current())return -EDEADLK;
    struct kernel_execution_suspend_token token={0};int rc=0;
    bool outer=kernel_execution_is_outermost();
    if(outer){rc=kernel_execution_suspend_irqrestore(&token);if(rc)return rc;}
    while(!__atomic_load_n(&thread->exited,__ATOMIC_ACQUIRE)){rc=kthread_wait_tick();if(rc)break;}
    if(!rc && result)*result=thread->result;
    if(outer){int resumed=kernel_execution_resume_irqsave(&token);if(resumed)rc=resumed;}
    return rc;
}
/** @brief Request stop without forcibly discarding an active context or lock.
 * @param thread Stable handle. @param result Optional body/cold-stop exit code.
 * @return Zero or join errno. Cold stop never enters an owner function. */
int kthread_stop(struct kthread *thread,int *result)
{
    if(!thread)return -EINVAL;if(thread==kthread_current())return -EDEADLK;
    uint64_t flags;kernel_spin_lock_irqsave(&kthread_lock,&flags);
    __atomic_store_n(&thread->stop,true,__ATOMIC_RELEASE);
    if(!thread->started && !thread->running && !thread->finished) {
        kthread_unqueue_locked(thread);thread->finished=true;thread->result=-EINTR;
        sched_kernel_task_state(thread->task,TASK_EXITED);
        if(thread->admitted){module_execution_put(thread->owner);thread->admitted=false;}
        __atomic_store_n(&thread->exited,true,__ATOMIC_RELEASE);
    } else if(!thread->finished){thread->wake_pending=true;thread->deadline=0;kthread_enqueue_locked(thread);}
    kernel_spin_unlock_irqrestore(&kthread_lock,flags);sched_notify_runnable();return kthread_join(thread,result);
}
/** @brief Disable and stop owned work, releasing only permanent-code storage.
 * @param thread Owned handle. @return Zero or stop errno; failures retain it. */
int kthread_destroy(struct kthread *thread)
{
    if(!thread)return -EINVAL;int rc=kthread_stop(thread,NULL);if(rc)return rc;
    if(thread->owner){uint64_t f;kernel_spin_lock_irqsave(&kthread_lock,&f);thread->closed=true;kernel_spin_unlock_irqrestore(&kthread_lock,f);}
    else thread_release(thread);return 0;
}
EXPORT_SYMBOL(kthread_create);
EXPORT_SYMBOL(kthread_wake);
EXPORT_SYMBOL(kthread_current);
EXPORT_SYMBOL(kthread_should_stop);
EXPORT_SYMBOL(kthread_yield);
EXPORT_SYMBOL(kthread_sleep_until);
EXPORT_SYMBOL(kthread_join);
EXPORT_SYMBOL(kthread_stop);
EXPORT_SYMBOL(kthread_destroy);
