/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/workqueue.h>
#include <reliefnt/lock.h>
#include <reliefnt/module.h>
#include <reliefnt/export.h>
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/smp.h>
#include <reliefnt/time.h>
#include <reliefnt/kthread.h>
#include <linux/errno.h>

enum { PENDING=1, RUNNING=2, CANCELLING=4, DISABLED=8 };
static struct kernel_spinlock work_lock=KERNEL_SPINLOCK_INIT;
static struct kernel_work *work_head;
static uint64_t next_deadline=UINT64_MAX;
static bool queued,notified;

/** @brief Publish the earliest deadline while work_lock stabilizes the list. */
static void publish_queue(void)
{
    struct kernel_work *eligible=work_head;
    while(eligible && (__atomic_load_n(&eligible->state,__ATOMIC_RELAXED)&RUNNING))
        eligible=eligible->next;
    __atomic_store_n(&next_deadline,eligible ? eligible->deadline : UINT64_MAX,__ATOMIC_RELEASE);
    __atomic_store_n(&queued,eligible!=NULL,__ATOMIC_RELEASE);
    if(!eligible || eligible->deadline>time_ticks())
        __atomic_store_n(&notified,false,__ATOMIC_RELEASE);
}
/** @brief Remove one pending link under work_lock without dropping its pin.
 * @param work Linked handle; its execution pin remains caller-owned. */
static void unlink_work(struct kernel_work *work)
{
    struct kernel_work **at=&work_head;
    while(*at && *at!=work)at=&(*at)->next;
    if(*at)*at=work->next;
    work->next=NULL;
    __atomic_fetch_and(&work->state,~(uint32_t)PENDING,__ATOMIC_RELEASE);
    publish_queue();
}
/** @brief Check the published earliest ready deadline without locking.
 * @return True when queued work is due; no callback/reference changes. */
bool kernel_workqueue_due(void)
{
    return __atomic_load_n(&queued,__ATOMIC_ACQUIRE) &&
           __atomic_load_n(&next_deadline,__ATOMIC_ACQUIRE)<=time_ticks();
}
/** @brief Read the queue's earliest eligible deadline without claiming work.
 * @return Absolute tick, or UINT64_MAX for an empty/entirely running queue. */
uint64_t kernel_workqueue_deadline(void)
{return __atomic_load_n(&next_deadline,__ATOMIC_ACQUIRE);}
/** @brief Wake scheduler service consumers once for a due deadline.
 * IRQ/task safe; no queue lock or callback spans the notification. */
void kernel_workqueue_tick(void)
{
    if(kernel_workqueue_due() && !__atomic_exchange_n(&notified,true,__ATOMIC_ACQ_REL))
        kernel_workqueue_wake_workers();
}
/** @brief Queue a pinned generation with a monotonic tick delay.
 * @param work Stable handle. @param delay_ticks Delay from the current tick.
 * @return True on admission, false on duplicate/overflow/closed owner.
 * IRQ/task safe, no allocation or callback under the queue lock. */
bool kernel_queue_delayed_work(struct kernel_work *work,uint64_t delay_ticks)
{
    if(!work || !work->function)return false;
    uint64_t now=time_ticks();if(delay_ticks>UINT64_MAX-now)return false;
    uint64_t flags;kernel_spin_lock_irqsave(&work_lock,&flags);
    uint32_t state=__atomic_load_n(&work->state,__ATOMIC_RELAXED);
    bool accepted=!(state&(PENDING|CANCELLING|DISABLED)) && work->issued!=UINT64_MAX &&
                  module_execution_get(work->owner);
    if(accepted) {
        work->deadline=now+delay_ticks;work->pending_sequence=++work->issued;
        struct kernel_work **at=&work_head;
        while(*at && (*at)->deadline<=work->deadline)at=&(*at)->next;
        work->next=*at;*at=work;
        __atomic_store_n(&work->state,state|PENDING,__ATOMIC_RELEASE);publish_queue();
    }
    kernel_spin_unlock_irqrestore(&work_lock,flags);
    if(accepted)kernel_workqueue_tick();
    return accepted;
}
/** @brief Coalesce one immediate generation with normal owner admission.
 * @param work Stable handle. @return True only when a generation is queued. */
bool kernel_queue_work(struct kernel_work *work)
{ return kernel_queue_delayed_work(work,0); }

/** @brief Claim one generation while preventing concurrent execution of itself.
 * @param target Optional exact handle for explicit flush. @param force Ignore its delay.
 * @return Claimed work with its original queue pin transferred to execution.
 * Requires execution ownership; only list/state changes hold work_lock.
 */
static struct kernel_work *take_work(struct kernel_work *target,bool force)
{
    uint64_t flags;kernel_spin_lock_irqsave(&work_lock,&flags);
    struct kernel_work *work=target ? target : work_head;
    while(work) {
        uint32_t state=__atomic_load_n(&work->state,__ATOMIC_RELAXED);
        if((state&(PENDING|RUNNING|CANCELLING|DISABLED))==PENDING &&
           (force || work->deadline<=time_ticks()))break;
        if(target || (!force && work->deadline>time_ticks())) {work=NULL;break;}
        work=work->next;
    }
    if(work) {
        unlink_work(work);work->running_sequence=work->pending_sequence;
        work->running_cpu=smp_current_cpu();
        work->running_thread=kthread_current();
        __atomic_fetch_or(&work->state,RUNNING,__ATOMIC_RELEASE);
    }
    kernel_spin_unlock_irqrestore(&work_lock,flags);return work;
}
/** @brief Enter one pinned callback without the queue lock and finish its fence.
 * @param work Exclusively claimed generation; execution ownership remains
 * caller-owned and may be suspended only by the callback's normal wait scope.
 * No reference to work is accessed after its RUNNING flag is cleared.
 */
static void execute_work(struct kernel_work *work)
{
    work->function(work,work->context);
    uint64_t flags;kernel_spin_lock_irqsave(&work_lock,&flags);
    work->completed=work->running_sequence>work->cancelled ? work->running_sequence : work->cancelled;
    module_execution_put(work->owner);
    work->running_thread=NULL;
    __atomic_fetch_and(&work->state,~(uint32_t)RUNNING,__ATOMIC_RELEASE);
    publish_queue();
    kernel_spin_unlock_irqrestore(&work_lock,flags);
    kernel_workqueue_tick();
}
/** @brief Run ready callbacks under caller-owned execution lifetime protection.
 * @param budget Maximum callbacks. @param budget_us Time between callbacks.
 * @return Number completed; single callbacks are not preempted by this budget.
 * Dedicated scheduler stack, depth-one writer, no queue lock spans callbacks. */
uint32_t kernel_workqueue_run(uint32_t budget,uint64_t budget_us)
{
    if(!budget || !kernel_workqueue_due() || !kernel_execution_is_outermost())return 0;
    uint64_t start=time_work_counter_us();uint32_t count=0;
    while(count<budget) {
        struct kernel_work *work=take_work(NULL,false);if(!work)break;
        execute_work(work);++count;
        uint64_t now=time_work_counter_us();
        if(budget_us && (now<start || now-start>=budget_us))break;
    }
    __atomic_store_n(&notified,false,__ATOMIC_RELEASE);kernel_workqueue_tick();return count;
}
/** @brief Wait for a callback without retaining its required execution owner.
 * @param work Stable handle. @return Zero or a suspension/context errno.
 * No queue lock or callback-required caller lock may span this task wait.
 */
static int wait_running(struct kernel_work *work)
{
    struct kernel_execution_suspend_token token={0};
    bool outer=kernel_execution_is_outermost();
    uint64_t flags=kernel_irq_save();kernel_irq_restore(flags);
    if(!outer && !(flags&(1ULL<<9)))return -EWOULDBLOCK;
    if(outer) {int rc=kernel_execution_suspend_irqrestore(&token);if(rc)return rc;}
    while(__atomic_load_n(&work->state,__ATOMIC_ACQUIRE)&RUNNING) {
        smp_membarrier_poll();
        int rc=kthread_wait_tick();
        if(rc){if(outer)(void)kernel_execution_resume_irqsave(&token);return rc;}
    }
    return outer ? kernel_execution_resume_irqsave(&token) : 0;
}
/** @brief Detect the exact running context instead of merely sharing its CPU.
 * @param work Stable handle, work_lock held. @return True for an actual
 * self-wait; explicit synchronous flushes outside threads retain CPU exclusion.
 */
static bool work_is_current(struct kernel_work *work)
{return work->running_thread ? work->running_thread==kthread_current() : work->running_cpu==smp_current_cpu();}
/** @brief Close one handle's queue admission and cancel all visible generations.
 * @param work Stable handle. @param disable Keep admission closed permanently.
 * @return Zero or cancellation/context/self-wait errno; failures never free it.
 */
static int cancel_work(struct kernel_work *work,bool disable)
{
    if(!work)return -EINVAL;
    uint64_t flags;kernel_spin_lock_irqsave(&work_lock,&flags);
    uint32_t state=__atomic_load_n(&work->state,__ATOMIC_RELAXED);
    if((state&RUNNING) && work_is_current(work)) {
        kernel_spin_unlock_irqrestore(&work_lock,flags);return -EDEADLK;
    }
    if(state&CANCELLING) {kernel_spin_unlock_irqrestore(&work_lock,flags);return -EBUSY;}
    __atomic_fetch_or(&work->state,CANCELLING|(disable ? DISABLED : 0),__ATOMIC_RELEASE);
    if(state&PENDING) {unlink_work(work);module_execution_put(work->owner);}
    work->cancelled=work->issued;
    if(!(state&RUNNING))work->completed=work->issued;
    kernel_spin_unlock_irqrestore(&work_lock,flags);
    int rc=(state&RUNNING) ? wait_running(work) : 0;
    kernel_spin_lock_irqsave(&work_lock,&flags);
    __atomic_fetch_and(&work->state,~(uint32_t)CANCELLING,__ATOMIC_RELEASE);
    kernel_spin_unlock_irqrestore(&work_lock,flags);return rc;
}
/** @brief Cancel pending work and wait without retaining execution ownership.
 * @param work Stable caller-retained handle. @return Zero or context/drain errno. */
int kernel_work_cancel_sync(struct kernel_work *work)
{ return cancel_work(work,false); }
/** @brief Complete the observed generation, bringing a pending deadline forward.
 * @param work Stable caller-retained handle. @return Zero or context/self-wait errno.
 * Task context with no callback-required locks; later requeues are not awaited. */
int kernel_work_flush(struct kernel_work *work)
{
    if(!work)return -EINVAL;
    uint64_t flags;kernel_spin_lock_irqsave(&work_lock,&flags);
    uint64_t target=work->issued;
    bool self=(work->state&RUNNING) && work_is_current(work);
    kernel_spin_unlock_irqrestore(&work_lock,flags);if(self)return -EDEADLK;
    bool acquired=!kernel_execution_is_outermost();uint64_t execution_flags=0;
    if(acquired) {
        uint64_t irq=kernel_irq_save();kernel_irq_restore(irq);
        if(!(irq&(1ULL<<9)))return -EWOULDBLOCK;
        kernel_execution_lock_irqsave(&execution_flags);
    }
    int rc=0;
    for(;;) {
        kernel_spin_lock_irqsave(&work_lock,&flags);
        bool done=work->completed>=target,busy=(work->state&CANCELLING)!=0;
        bool running=(work->state&RUNNING)!=0;
        kernel_spin_unlock_irqrestore(&work_lock,flags);
        if(done || busy) {if(busy)rc=-EBUSY;break;}
        if(running) {rc=wait_running(work);if(rc)break;}
        else {struct kernel_work *claimed=take_work(work,true);if(claimed)execute_work(claimed);}
    }
    if(acquired)kernel_execution_unlock_irqrestore(execution_flags);return rc;
}
/** @brief Disable a module-owned work handle before the image may drain.
 * @param context Ledger-owned work. @return Zero or a safe cancellation errno. */
static int work_quiesce(void *context) {return cancel_work(context,true);}
/** @brief Free a fully quiesced module-ledger-owned work handle.
 * @param context Owned stopped work with no queued/running generation. */
static void work_release(void *context) {kernel_free(context);}
/** @brief Allocate work with module-ledger cancellation before owner destruction.
 * @param function Required callback. @param context Borrowed retained state.
 * @param owner Borrowed COMING/LIVE owner or NULL. @param out Owned handle result.
 * @return Zero or validation/allocation/ledger errno; no partial publication. */
int kernel_work_create(kernel_work_fn function,void *context,struct module *owner,struct kernel_work **out)
{
    if(!out)return -EINVAL;*out=NULL;if(!function)return -EINVAL;
    struct kernel_work *work=kernel_malloc(sizeof(*work));if(!work)return -ENOMEM;
    *work=(struct kernel_work){.function=function,.context=context,.owner=owner,.allocated=true};
    int rc=owner ? module_add_resource(owner,work_quiesce,work_release,work) : 0;
    if(rc){kernel_free(work);return rc;}*out=work;return 0;
}
/** @brief Disable allocated work before freeing or retaining its ledger storage.
 * @param work Owned allocated handle. @return Zero or cancellation errno.
 * Module-owned storage is freed only by the final module ledger release. */
int kernel_work_destroy(struct kernel_work *work)
{
    if(!work || !work->allocated)return -EINVAL;
    int rc=cancel_work(work,true);if(!rc && !work->owner)kernel_free(work);return rc;
}
EXPORT_SYMBOL(kernel_work_create);
EXPORT_SYMBOL(kernel_queue_work);
EXPORT_SYMBOL(kernel_queue_delayed_work);
EXPORT_SYMBOL(kernel_work_cancel_sync);
EXPORT_SYMBOL(kernel_work_flush);
EXPORT_SYMBOL(kernel_work_destroy);
