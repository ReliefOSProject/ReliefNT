/* Real OOM selection, PID identities, SIGKILL queue, scheduler wake/exit and
 * resource retirement. CPU ownership, page counts and storage are host models. */
#define PRESSURE_WAIT_FIXTURE 1
#define PRESSURE_REAL_SIGNAL 1
#define main old_exit_logging_main
#include "task_exit_logging_test.c"
#undef main
#include "../../mm/reclaim/internal.h"
#include <reliefnt/memory_pressure.h>
#include <linux/errno.h>

static unsigned other_lock_depth;
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    *flags=interrupts_enabled; interrupts_enabled=false;
    if (lock==&scheduler_lock) { assert(!scheduler_lock_depth && !other_lock_depth); scheduler_lock_depth=1; }
    else { assert(!other_lock_depth && !scheduler_lock_depth); other_lock_depth=1; }
}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    if (lock==&scheduler_lock) { assert(scheduler_lock_depth); scheduler_lock_depth=0; }
    else { assert(other_lock_depth); other_lock_depth=0; }
    interrupts_enabled=flags!=0;
}
void kernel_wait_queue_remove(struct kernel_wait_queue *queue, struct task *task)
{ (void)queue; (void)task; abort(); }
bool kernel_execution_is_outermost(void) { return true; }
uint64_t mm_total_memory_kib(void) { return 8192*4; }
/** @brief Model a lifecycle MM containing only owned RAM leaves.
 * @param as Borrowed model MM or NULL. @return Modeled resident KiB.
 */
uint64_t address_space_user_resident_kib(const struct address_space *as)
{ return as ? as->user_page_count * 4 : 0; }
/** @brief Model an MM with no mutable DEVICE leaves.
 * @param as Borrowed model MM. @return Constant zero revision.
 */
uint64_t mm_context_device_revision(const struct address_space *as) { (void)as; return 0; }
/** @brief End the DEVICE-only walk because lifecycle fixtures contain no DEVICE.
 * @param as Borrowed model MM. @param cursor Unchanged in/out address.
 * @param page Unchanged output. @return Zero, indicating scan completion.
 */
int paging_user_pressure_step(struct address_space *as, uint64_t *cursor, uint64_t *page)
{ (void)as; (void)cursor; (void)page; return 0; }
/** @brief Reject a leaf query after the modeled empty DEVICE walk.
 * @param as Borrowed model MM. @param page Unexpected address.
 * @return Never; reaching this path aborts the fixture.
 */
uint64_t address_space_user_page_flags(const struct address_space *as, uint64_t page)
{ (void)as; (void)page; abort(); }
uint64_t mm_swap_count_existing(const struct address_space *as) { (void)as; return 0; }
uint64_t mm_context_generation_existing(const struct address_space *as)
{ return as && as->cr3 ? as->cr3 ^ 0x55 : 0; }

static void identity(struct task *task)
{
    struct task_identity *id=sched_task_identity_prepare(); assert(id);
    sched_task_identity_bind(task,id);
}

#ifndef MEMORY_PRESSURE_OOM_LIFECYCLE_ENTRY
#define MEMORY_PRESSURE_OOM_LIFECYCLE_ENTRY main
#endif
/** @brief Verify real signal/wake/exit behavior with modeled CPU retirement.
 * @return Zero after all shared-MM ownership and quiescence assertions pass.
 */
int MEMORY_PRESSURE_OOM_LIFECYCLE_ENTRY(void)
{
    struct task *init=fixture_task(1,"init"), *service=fixture_task(2,"service");
    struct task *a=fixture_task(3,"victim-a"), *b=fixture_task(4,"victim-b");
    struct task *small=fixture_task(5,"small");
    struct task *late=fixture_task(6,"late-sharer"); late->kind=TASK_KIND_KERNEL;
    struct task *table[]={init,service,a,b,small,late}; install_tasks(table,6);
    sched_task_as(init)->cr3=0x1000; sched_task_as(init)->user_page_count=4000;
    sched_task_as(service)->cr3=0x2000; sched_task_as(service)->user_page_count=3000;
    service->flags|=TASK_FLAG_SERVICE;
    struct task_address_space_state *shared=calloc(1,sizeof(*shared)); assert(shared);
    shared->references=2; shared->as.cr3=0x3000; shared->as.user_page_count=300;
    a->shared_mm=b->shared_mm=shared; a->state=TASK_RUNNING; a->running_cpu=1;
    b->state=TASK_STOPPED; b->running_cpu=SCHED_CPU_NONE;
    sched_task_as(small)->cr3=0x5000; sched_task_as(small)->user_page_count=100;
    for (unsigned i=0;i<5;++i) identity(table[i]);
    identity(late);
    fixture_forbid_malloc=true;
    unsigned calls=0;
    while (!(*sched_task_process_pending(a)&(1ULL<<8)) ||
           !(*sched_task_process_pending(b)&(1ULL<<8))) {
        int rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<200);
    }
    assert(b->state==TASK_READY && a->running_cpu==1);
    assert(!*sched_task_process_pending(init) && !*sched_task_process_pending(service) &&
           !*sched_task_process_pending(small));
    for (unsigned i=0;i<30;++i) {
        int rc=memory_oom_advance(2); assert(rc==-EAGAIN || rc==-EINPROGRESS);
    }
    assert(!*sched_task_process_pending(small) && !(a->flags&TASK_FLAG_RESOURCES_RELEASED));
    /* A new independent CLONE_VM-style owner joins after the first signal
     * scan. It must receive SIGKILL and remain retained until its own CPU
     * has quiesced, without selecting the smaller MM. */
    late->kind=TASK_KIND_USER; late->shared_mm=shared; ++shared->references;
    late->state=TASK_RUNNING; late->running_cpu=2;
    unsigned late_calls=0;
    while (!(*sched_task_process_pending(late)&(1ULL<<8))) {
        int rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++late_calls<100);
        assert(!*sched_task_process_pending(small));
    }
    fixture_forbid_malloc=false;
    sched_exit(late->pid,9);
    sched_release_task_resources(late);
    assert(!(late->flags&TASK_FLAG_RESOURCES_RELEASED) && shared->references==3);
    late->running_cpu=SCHED_CPU_NONE;
    sched_release_task_resources(late);
    assert((late->flags&TASK_FLAG_RESOURCES_RELEASED) && shared->references==2);
    sched_exit(a->pid,9);
    assert(a->state==TASK_EXITED && a->running_cpu==1 && !(a->flags&TASK_FLAG_RESOURCES_RELEASED));
    sched_release_task_resources(a);
    assert(!(a->flags&TASK_FLAG_RESOURCES_RELEASED) && shared->references==2);
    a->running_cpu=SCHED_CPU_NONE;
    sched_release_task_resources(a);
    assert((a->flags&TASK_FLAG_RESOURCES_RELEASED) && shared->references==1);
    sched_exit(b->pid,9);
    assert((b->flags&TASK_FLAG_RESOURCES_RELEASED) && !b->shared_mm);
    struct task_identity *kept=a->identity_token;
    unsigned waited=0;
    while (kept->references>1) {
        int rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++waited<20);
    }
    assert(!*sched_task_process_pending(small));
    for (unsigned i=0;i<5;++i) { sched_task_identity_detach(table[i]); free(table[i]); }
    sched_task_identity_detach(late); free(late);
    assert(!scheduler_lock_depth && !other_lock_depth);
    printf("PASS real OOM SIGKILL queue, shared-MM wake and quiescent exit after %u batches; late=%u\n",calls,late_calls);
    return 0;
}
