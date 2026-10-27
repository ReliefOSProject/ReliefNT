/* Real PID-object lifetime, with only the heap allocation boundary replaced. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <linux/errno.h>
#include <reliefnt/sched.h>

static int allocation_budget=-1;
static unsigned live_allocations;
void *kernel_malloc(uint64_t length)
{
    if(!allocation_budget)return NULL;
    if(allocation_budget>0)--allocation_budget;
    void *value=malloc(length);if(value)++live_allocations;return value;
}
void kernel_free(void *value)
{if(value){assert(live_allocations);--live_allocations;}free(value);}

int main(void)
{
    struct task a={.pid=1},b={.pid=2},empty={0};
    struct task_identity *old=NULL,*alias=NULL,*next=NULL,*other=NULL;
    assert(sched_task_identity_get(&a,NULL)==-EINVAL);
    assert(sched_task_identity_get(&empty,&old)==-ESRCH && !old);
    allocation_budget=0;
    assert(sched_task_identity_get(&a,&old)==-ENOMEM && !old && !a.identity_token && !live_allocations);
    allocation_budget=-1;assert(!sched_task_identity_get(&a,&old));
    allocation_budget=0;assert(!sched_task_identity_get(&a,&alias) && old==alias);
    allocation_budget=-1;
    sched_task_identity_detach(&a);assert(!sched_task_identity_task(old));
    assert(!sched_task_identity_get(&a,&next) && next!=old);
    sched_task_identity_put(old);sched_task_identity_put(alias);
    assert(live_allocations==1 && sched_task_identity_task(next)==&a);
    assert(!sched_task_identity_get(&b,&other));
    sched_task_identity_swap(&a,&b);a.pid=2;b.pid=1;
    assert(sched_task_identity_task(next)==&b && sched_task_identity_task(other)==&a);
    sched_task_identity_detach(&a);assert(!sched_task_identity_task(other));
    sched_task_identity_put(other);sched_task_identity_detach(&b);sched_task_identity_put(next);
    assert(!live_allocations);
    a=(struct task){.pid=7,.state=TASK_EXITED,.flags=TASK_FLAG_WAITABLE_CHILD};
    assert(!sched_task_identity_get(&a,&old));
    assert(sched_task_identity_task(old)==&a);
    a.flags=0;
    assert(sched_task_identity_get(&a,&alias)==-ESRCH && !alias);
    sched_task_identity_detach(&a);assert(!sched_task_identity_task(old));
    sched_task_identity_put(old);assert(!live_allocations);
    puts("PASS PID identity: allocation rollback, stable aliases, recycled slots, exec PID swap and final release");
}
