/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/kthread.h>
#include <reliefnt/workqueue.h>
#include <reliefnt/lock.h>
#include <reliefnt/smp.h>
struct kthread { kthread_fn function;void *context;uint32_t cpu,wakes; };
static struct kthread pool[4];
static unsigned created,wakes,inputs,batches,sleeps,yields;
static bool held,stopped,due,online[4]={true,true,true,true};
uint32_t smp_cpu_count(void){return 4;}
bool smp_cpu_online(uint32_t cpu){return cpu<4 && online[cpu];}
int kthread_create(kthread_fn fn,void *context,const char *name,struct module *owner,uint32_t cpu,struct kthread **out)
{
    char expected[24];snprintf(expected,sizeof(expected),"kworker/%u:0",cpu);
    assert(created<4 && cpu==created && !strcmp(name,expected) && !owner);
    pool[created]=(struct kthread){.function=fn,.context=context,.cpu=cpu};
    *out=&pool[created++];return 0;
}
bool kthread_wake(struct kthread *thread){assert(online[thread->cpu]);++thread->wakes;++wakes;return true;}
int kthread_destroy(struct kthread *thread){(void)thread;assert(!"unexpected pool failure");return 0;}
bool kthread_should_stop(void){return stopped;}
int kthread_yield(void){assert(!held);++yields;stopped=true;return 0;}
int kthread_sleep_until(uint64_t deadline)
{assert(!held && deadline==UINT64_MAX);++sleeps;stopped=true;return 0;}
void kernel_execution_lock_irqsave(uint64_t *flags){assert(!held);held=true;*flags=512;}
void kernel_execution_unlock_irqrestore(uint64_t flags){assert(held && flags==512);held=false;}
void input_process_pending(void){assert(held);++inputs;}
uint32_t kernel_workqueue_run(uint32_t count,uint64_t us)
{
    assert(held && count && us);++batches;
    if(batches==1) {
        unsigned before=pool[1].wakes;
        for(unsigned i=0;i<4;++i)kernel_workqueue_wake_workers();
        assert(pool[1].wakes==before); /* Work may need an idle peer to unblock this batch. */
    }
    return 1;
}
uint64_t kernel_workqueue_deadline(void){return 100;}
bool kernel_workqueue_due(void){assert(held);return due;}
int main(void)
{
    assert(!kernel_workqueue_start_workers() && created==4);
    assert(wakes==1); /* One notification must not broadcast to four lock contenders. */
    for(unsigned i=0;i<7;++i)kernel_workqueue_wake_workers();
    assert(wakes==8);
    for(unsigned i=0;i<4;++i)assert(pool[i].wakes==2);
    online[0]=false;
    for(unsigned i=0;i<4;++i){unsigned before=wakes;kernel_workqueue_wake_workers();assert(wakes==before+1);}
    assert(pool[0].wakes==2);
    /* A drained worker waits for queue notification, not a shared deadline that
     * independently wakes every idle CPU when only one generation is due. */
    assert(!pool[1].function(pool[1].context));
    assert(sleeps==1 && yields==0 && inputs==2 && batches==1);
    stopped=false;due=true;
    assert(!pool[2].function(pool[2].context));
    assert(sleeps==1 && yields==1 && inputs==4 && batches==2);
    puts("PASS one online worker per notification, CPU rotation and event-only idle sleep");return 0;
}
