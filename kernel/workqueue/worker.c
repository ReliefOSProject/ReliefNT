/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/workqueue.h>
#include <reliefnt/kthread.h>
#include <reliefnt/smp.h>
#include <reliefnt/lock.h>
#include <reliefnt/input.h>
static struct kthread *workers[SMP_MAX_CPUS];
static uint32_t worker_count,worker_cursor;
static bool worker_busy[SMP_MAX_CPUS];
/** @brief Drain bounded work batches on a retained, schedulable kernel stack.
 * @param context Bound CPU index. @return Zero after a cooperative pool shutdown.
 * Legacy service callbacks retain their writer/wait scopes; unlocked waits can
 * be timer-preempted. No writer or spinlock spans the worker's sleep/yield.
 */
static int workqueue_worker(void *context)
{
    uint32_t cpu=(uint32_t)(uintptr_t)context;
    while(!kthread_should_stop()) {
        __atomic_store_n(&worker_busy[cpu],true,__ATOMIC_RELEASE);
        uint64_t flags;kernel_execution_lock_irqsave(&flags);
        input_process_pending();(void)kernel_workqueue_run(4,2000);input_process_pending();
        bool due=kernel_workqueue_due();kernel_execution_unlock_irqrestore(flags);
        __atomic_store_n(&worker_busy[cpu],false,__ATOMIC_RELEASE);
        if(due)(void)kthread_yield();else (void)kthread_sleep_until(UINT64_MAX);
    }
    return 0;
}
/** @brief Notify one online consumer without a global execution-lock herd.
 * IRQ/task safe; rotate CPUs and coalesce wakes of a running/ready consumer.
 * The queue timer is the single delayed-work source; idle workers have no
 * duplicate shared-deadline timer, so one job cannot wake the entire pool.
 */
void kernel_workqueue_wake_workers(void)
{
    uint32_t count=__atomic_load_n(&worker_count,__ATOMIC_ACQUIRE);
    if(!count)return;
    uint32_t start=__atomic_fetch_add(&worker_cursor,1,__ATOMIC_RELAXED)%count;
    /* A suspended callback may depend on another queued callback; prefer an
     * unoccupied consumer instead of only reawakening the waiting stack. */
    for(unsigned pass=0;pass<2;++pass)
        for(uint32_t i=0;i<count;++i) {
            uint32_t cpu=(start+i)%count;
            if(!pass && __atomic_load_n(&worker_busy[cpu],__ATOMIC_ACQUIRE))continue;
            if(smp_cpu_online(cpu) && kthread_wake(workers[cpu]))return;
        }
}
/** @brief Build the permanent per-CPU worker pool before user scheduling starts.
 * @return Zero or allocation error, retaining no partial pool on failure.
 * Serialized boot context; a failed CPU startup leaves its bound worker idle.
 */
int kernel_workqueue_start_workers(void)
{
    if(worker_count)return 0;
    uint32_t count=smp_cpu_count();if(count>SMP_MAX_CPUS)count=SMP_MAX_CPUS;
    for(uint32_t i=0;i<count;++i) {
        char name[24]="kworker/";unsigned n=8;
        if(i>=10)name[n++]=(char)('0'+i/10);
        name[n++]=(char)('0'+i%10);name[n++]=':';name[n++]='0';name[n]=0;
        int rc=kthread_create(workqueue_worker,(void *)(uintptr_t)i,name,NULL,i,&workers[i]);
        if(rc){while(i)(void)kthread_destroy(workers[--i]);return rc;}
    }
    __atomic_store_n(&worker_count,count,__ATOMIC_RELEASE);
    kernel_workqueue_wake_workers();return 0;
}
