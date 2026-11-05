/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef WORKQUEUE_MISSING
int main(void) { fputs("FAIL: no cancellable deferred execution queue\n",stderr);return 1; }
#else
#include <reliefnt/workqueue.h>
#include <reliefnt/lock.h>
#include <reliefnt/module.h>
#include <reliefnt/smp.h>

static _Thread_local uint64_t irq=512;
static _Thread_local uint32_t cpu;
static atomic_uint_fast64_t ticks;
static atomic_uint notifications,pins;
static atomic_bool live=true,entered,release_callback,cancel_returned;
static int (*quiesce)(void *);
static void (*release_resource)(void *);
static void *resource;
static struct module owner;
static unsigned calls;
uint64_t kernel_irq_save(void) { uint64_t old=irq;irq=0;return old; }
void kernel_irq_restore(uint64_t flags) { irq=flags; }
uint32_t smp_current_cpu(void) { return cpu; }
void smp_membarrier_poll(void) { }
uint64_t time_ticks(void) { return atomic_load(&ticks); }
uint64_t time_work_counter_us(void) { return 0; }
void sched_notify_runnable(void) { ++notifications; }
void kernel_workqueue_wake_workers(void) {sched_notify_runnable();}
struct kthread *kthread_current(void) {return NULL;}
int kthread_wait_tick(void) {usleep(100);return 0;}
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *memory) { free(memory); }
bool module_execution_get(struct module *module)
{ if(!module)return true;assert(module==&owner);if(!live)return false;++pins;return true; }
void module_execution_put(struct module *module)
{ if(module) { assert(module==&owner && pins);--pins; } }
int module_add_resource(struct module *module,int (*stop)(void *),void (*drop)(void *),void *context)
{ assert(module==&owner && !resource);quiesce=stop;release_resource=drop;resource=context;return 0; }

/** @brief Count a callback and require queue locks to have been released. */
static void counted(struct kernel_work *work,void *context)
{ (void)work;assert(kernel_execution_is_outermost());++*(unsigned *)context; }
/** @brief Queue exactly one subsequent generation while the callback runs. */
static void repeats(struct kernel_work *work,void *context)
{
    unsigned *count=context;++*count;
    if(*count==1) {
        assert(kernel_queue_work(work));assert(!kernel_queue_work(work));
        assert(!kernel_workqueue_due()); /* A successor cannot run until we return. */
    }
}
/** @brief Run a bounded queue batch under the production service contract. */
static unsigned pump(void)
{ uint64_t flags;kernel_execution_lock_irqsave(&flags);unsigned n=kernel_workqueue_run(8,2000);kernel_execution_unlock_irqrestore(flags);return n; }
/** @brief Keep module code pinned while allowing another CPU to cancel it. */
static void blocked(struct kernel_work *work,void *context)
{
    (void)context;assert(kernel_work_cancel_sync(work)==-35);
    struct kernel_execution_suspend_token token={0};
    assert(!kernel_execution_suspend_irqrestore(&token));entered=true;
    while(!release_callback)usleep(100);
    assert(!kernel_execution_resume_irqsave(&token));
}
static void *worker(void *unused) { (void)unused;cpu=1;assert(pump()==1);return NULL; }
static void *canceller(void *opaque)
{ cpu=2;assert(!kernel_work_cancel_sync(opaque));cancel_returned=true;return NULL; }

int main(void)
{
    struct kernel_work first=KERNEL_WORK_INIT(counted,&calls);
    assert(!kernel_workqueue_due() && !pump());
    assert(kernel_queue_work(&first) && !kernel_queue_work(&first));
    assert(notifications==1);
    assert(kernel_workqueue_due() && pump()==1 && calls==1 && !kernel_workqueue_due());
    assert(kernel_queue_delayed_work(&first,3));assert(!pump());
    ticks=2;kernel_workqueue_tick();assert(!kernel_workqueue_due());
    ticks=3;kernel_workqueue_tick();assert(kernel_workqueue_due() && pump()==1 && calls==2);
    assert(kernel_queue_delayed_work(&first,10));assert(!kernel_work_cancel_sync(&first));
    ticks=30;assert(!pump() && calls==2);
    assert(kernel_queue_delayed_work(&first,100));assert(!kernel_work_flush(&first));
    assert(calls==3 && !kernel_workqueue_due());
    struct kernel_work again=KERNEL_WORK_INIT(repeats,&calls);calls=0;
    assert(kernel_queue_work(&again) && pump()==2 && calls==2);

    struct kernel_work *dynamic=NULL;calls=0;
    assert(!kernel_work_create(blocked,NULL,&owner,&dynamic) && dynamic && resource==dynamic);
    assert(kernel_queue_work(dynamic) && pins==1);
    pthread_t a,b;assert(!pthread_create(&a,NULL,worker,NULL));
    while(!entered)usleep(100);
    assert(!pthread_create(&b,NULL,canceller,dynamic));usleep(10000);
    assert(!cancel_returned && pins==1);
    release_callback=true;assert(!pthread_join(a,NULL) && !pthread_join(b,NULL));
    assert(cancel_returned && !pins);
    assert(kernel_queue_work(dynamic) && pins==1);live=false;
    assert(!quiesce(resource) && !pins && !kernel_queue_work(dynamic));
    release_resource(resource);resource=NULL;
    assert(!pump());
    puts("PASS work coalescing, delayed execution, running requeue, flush, cancel wait, self-deadlock rejection and module drain");
    return 0;
}
#endif
