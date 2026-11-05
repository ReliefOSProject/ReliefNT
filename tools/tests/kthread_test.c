/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#ifdef KTHREAD_MISSING
int main(void){fputs("FAIL: kernel tasks have no resumable stack or sleep/wakeup lifecycle\n",stderr);return 1;}
#else
#include <stdlib.h>
#include <reliefnt/kthread.h>
#include <reliefnt/workqueue.h>
#include <reliefnt/sched_kernel.h>
#include <reliefnt/sched.h>
#include <reliefnt/lock.h>
#include <reliefnt/module.h>
#include <reliefnt/smp.h>
#include <linux/errno.h>
static uint64_t irq=512,ticks,scheduler_us;
static unsigned phase,body_calls,pins,allocations;
static bool live=true;
static struct task *identity;
static struct module owner;
static int (*quiesce)(void *);
static void (*release_resource)(void *);
static void *resource;
uint64_t kernel_irq_save(void){uint64_t old=irq;irq=0;return old;}
void kernel_irq_restore(uint64_t flags){irq=flags;}
uint32_t smp_current_cpu(void){return 0;}
uint32_t smp_cpu_count(void){return 1;}
bool smp_cpu_online(uint32_t cpu){return cpu==0;}
void smp_membarrier_poll(void){}
uint64_t time_ticks(void){return ticks;}
uint64_t time_work_counter_us(void){return 0;}
uint64_t sched_kernel_clock_us(void){return scheduler_us;}
void sched_notify_runnable(void){}
void kernel_workqueue_wake_workers(void){sched_notify_runnable();}
void *kernel_malloc(size_t bytes){void *p=malloc(bytes);if(p)++allocations;return p;}
void kernel_free(void *p){if(p){assert(allocations);--allocations;free(p);}}
bool module_execution_get(struct module *m){if(!m)return true;assert(m==&owner);if(!live)return false;++pins;return true;}
void module_execution_put(struct module *m){if(m){assert(m==&owner && pins);--pins;}}
int module_add_resource(struct module *m,int (*stop)(void *),void (*drop)(void *),void *p)
{assert(m==&owner && !resource);quiesce=stop;release_resource=drop;resource=p;return 0;}
struct task *sched_create_kernel_task(const char *name,uint64_t entry,uint64_t low,uint64_t top,uint32_t cpu)
{(void)name;assert(entry && low && top>low && !cpu);struct task *t=calloc(1,sizeof(*t));if(t){t->pid=77;t->kind=TASK_KIND_KERNEL;t->state=TASK_BLOCKED;t->stack_low=low;t->stack_top=top;}return t;}
void sched_kernel_task_state(struct task *t,unsigned state){t->state=state;}
void sched_destroy_kernel_task(struct task *t){assert(t && t!=identity);free(t);}
void sched_kernel_context_enter(struct task *t,struct kernel_sched_context *c)
{assert(!identity && kernel_execution_is_outermost());c->previous=identity;identity=t;}
void sched_kernel_context_leave(struct task *t,struct kernel_sched_context *c)
{assert(identity==t && kernel_execution_is_outermost());identity=c->previous;}
void arch_kthread_idle(void){++ticks;kthread_tick();}
static unsigned dispatch(void)
{uint64_t f;kernel_execution_lock_irqsave(&f);unsigned n=kthread_dispatch(1);kernel_execution_unlock_irqrestore(f);return n;}
/** @brief A stack local and register state must survive yielding and timed sleep. */
static int sequence(void *context)
{
    volatile unsigned retained=1234;assert(context==&phase && kthread_current() && identity);
    assert(!kernel_execution_is_outermost() && irq==512);phase=1;
    assert(!kthread_yield());assert(retained==1234);phase=2;
    assert(!kthread_sleep_until(10));assert(retained==1234);phase=3;return 37;
}
/** @brief A wake published before sleep must not disappear between predicate and park. */
static int early_wake(void *unused)
{(void)unused;assert(kthread_wake(kthread_current()));assert(!kthread_sleep_until(100));++body_calls;return 0;}
/** @brief Stop a blocked thread without freeing its retained module instruction stream. */
static int stoppable(void *unused)
{(void)unused;assert(pins==1);++body_calls;while(!kthread_should_stop())assert(!kthread_sleep_until(100));assert(pins==1);return 19;}
/** @brief Stop/join oneself must fail instead of blocking the only execution context. */
static int self_wait(void *unused)
{(void)unused;int result;assert(kthread_join(kthread_current(),&result)==-EDEADLK);assert(kthread_stop(kthread_current(),&result)==-EDEADLK);return 0;}
/** @brief An unlocked timer return saves this stack, while locked ones cannot switch. */
static int interruptible(void *unused)
{
    (void)unused;struct trap_frame frame={.cs=8,.rflags=512};
    struct kernel_spinlock lock=KERNEL_SPINLOCK_INIT;
    kernel_spin_lock(&lock);uint64_t saved=kernel_irq_save();
    assert(!kthread_preempt(&frame));kernel_irq_restore(saved);kernel_spin_unlock(&lock);
    uint64_t gate;kernel_execution_lock_irqsave(&gate);
    assert(!kthread_preempt(&frame));kernel_execution_unlock_irqrestore(gate);
    phase=4;saved=kernel_irq_save();assert(kthread_preempt(&frame));kernel_irq_restore(saved);
    phase=5;return 0;
}
/** @brief Cancel a preempted/sleeping callback from a different context on its CPU. */
static void sleeping_work(struct kernel_work *work,void *context)
{
    (void)context;assert(kernel_work_cancel_sync(work)==-EDEADLK);
    struct kernel_execution_suspend_token token={0};assert(!kernel_execution_suspend_irqrestore(&token));
    phase=6;assert(!kthread_sleep_until(time_ticks()+2));phase=7;
    assert(!kernel_execution_resume_irqsave(&token));
}
/** @brief Run production queue dispatch in an independently schedulable context. */
static int queue_worker(void *unused)
{(void)unused;uint64_t flags;kernel_execution_lock_irqsave(&flags);assert(kernel_workqueue_run(1,2000)==1);kernel_execution_unlock_irqrestore(flags);return 0;}
/** @brief A wake must survive while the thread is already ready after yielding. */
static int ready_wake(void *unused)
{(void)unused;phase=8;assert(!kthread_yield());assert(!kthread_sleep_until(time_ticks()+100));phase=9;return 0;}
/** @brief A wake between predicate and park must survive an intervening IRQ switch. */
static int wake_then_preempt(void *unused)
{
    (void)unused;struct trap_frame frame={.cs=8,.rflags=512};phase=10;
    assert(kthread_wake(kthread_current()));uint64_t saved=kernel_irq_save();
    assert(kthread_preempt(&frame));kernel_irq_restore(saved);phase=11;
    assert(!kthread_sleep_until(UINT64_MAX));phase=12;return 0;
}
extern uint32_t kthread_dispatch_user(uint32_t) __attribute__((weak));
/** @brief Repeated user fault/syscall returns must get a user tick after a worker turn. */
static unsigned user_dispatch(void)
{uint64_t f;kernel_execution_lock_irqsave(&f);unsigned n=kthread_dispatch_user ? kthread_dispatch_user(1) : kthread_dispatch(1);kernel_execution_unlock_irqrestore(f);return n;}
static int budget_thread(void *unused)
{(void)unused;phase=13;++ticks;assert(!kthread_yield());phase=14;return 0;}
int main(void)
{
    struct kthread *t=NULL;int result=-1;
    assert(!kthread_create(sequence,&phase,"test",NULL,0,&t));
    assert(!kthread_runnable());assert(kthread_wake(t));assert(dispatch()==1 && phase==1);
    assert(dispatch()==1 && phase==2);assert(!kthread_runnable() && !dispatch());
    ticks=9;kthread_tick();assert(!dispatch());ticks=10;kthread_tick();assert(dispatch()==1 && phase==3);
    assert(!kthread_join(t,&result) && result==37);assert(!kthread_destroy(t));assert(!allocations);
    assert(!kthread_create(early_wake,NULL,"early",NULL,0,&t));
    assert(kthread_wake(t));assert(dispatch()==1 && body_calls==1);assert(!kthread_destroy(t));
    assert(!kthread_create(early_wake,NULL,"cold",NULL,0,&t));
    assert(!kthread_stop(t,&result) && result==-EINTR && body_calls==1);assert(!kthread_destroy(t));
    assert(!kthread_create(self_wait,NULL,"self",NULL,0,&t));
    assert(kthread_wake(t));assert(dispatch()==1);assert(!kthread_destroy(t));
    assert(!kthread_create(interruptible,NULL,"irq",NULL,0,&t));
    assert(kthread_wake(t));assert(dispatch()==1 && phase==4);assert(dispatch()==1 && phase==5);assert(!kthread_destroy(t));
    assert(!kthread_create(stoppable,NULL,"owner",&owner,0,&t));
    assert(kthread_wake(t) && pins==1);assert(dispatch()==1 && pins==1);
    live=false;assert(!quiesce(resource) && !pins);release_resource(resource);resource=NULL;
    assert(!allocations && !identity && !kthread_runnable());
    struct kernel_work item=KERNEL_WORK_INIT(sleeping_work,NULL);
    assert(kernel_queue_work(&item));
    assert(!kthread_create(queue_worker,NULL,"cancel",NULL,0,&t));assert(kthread_wake(t));
    assert(dispatch()==1 && phase==6);
    assert(!kernel_work_cancel_sync(&item) && phase==7);
    assert(!kthread_destroy(t) && !allocations);
    assert(!kthread_create(ready_wake,NULL,"ready-wake",NULL,0,&t));assert(kthread_wake(t));
    assert(dispatch()==1 && phase==8);assert(kthread_wake(t));
    assert(dispatch()==1 && phase==9);assert(!kthread_destroy(t) && !allocations);
    assert(!kthread_create(wake_then_preempt,NULL,"wake-irq",NULL,0,&t));assert(kthread_wake(t));
    assert(dispatch()==1 && phase==10);assert(dispatch()==1 && phase==12);
    assert(!kthread_destroy(t) && !allocations);
    assert(!kthread_create(budget_thread,NULL,"user-budget",NULL,0,&t));assert(kthread_wake(t));
    assert(user_dispatch()==1 && phase==13);
    assert(user_dispatch()==0 && phase==13);++ticks;
    assert(user_dispatch()==1 && phase==14);assert(!kthread_destroy(t) && !allocations);
    /* A delayed BSP/PIT interrupt must not extend one user window indefinitely. */
    ++ticks;scheduler_us=1000000;
    assert(!kthread_create(budget_thread,NULL,"stalled-pit",NULL,0,&t));assert(kthread_wake(t));
    assert(user_dispatch()==1 && phase==13);
    assert(user_dispatch()==0 && phase==13);scheduler_us+=10000;
    assert(user_dispatch()==1 && phase==14);assert(!kthread_destroy(t) && !allocations);
    puts("PASS real retained stacks, yield, timed sleep, early wake, stop/join, lock-safe preemption and module drain");return 0;
}
#endif
