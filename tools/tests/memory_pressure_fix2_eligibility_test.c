/* Actual OOM selector, scheduler PID/policy, exec MM commit and vfork helper.
 * Task slots, CPU/lock, graphics and the old shared MM storage are modeled. */
#define main inherited_oom_test_main
#include "memory_pressure_oom_test.c"
#undef main
#ifndef MEMORY_PRESSURE_FIX2_OOM_SOURCE
#define MEMORY_PRESSURE_FIX2_OOM_SOURCE "../../mm/reclaim/oom.c"
#endif
#include MEMORY_PRESSURE_FIX2_OOM_SOURCE
#include <reliefnt/graphics_service.h>

struct kernel_spinlock scheduler_lock=KERNEL_SPINLOCK_INIT;
static unsigned exec_cancels, reschedules;

/** @brief Enforce the real helper's scheduler-lock nesting in the host model.
 * @param lock Required scheduler lock. @param flags Saved interrupt output.
 */
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(lock==&scheduler_lock && !lock->state);lock->state=1;*flags=0; }
/** @brief Finish the modeled scheduler-lock section.
 * @param lock Required held lock. @param flags Saved host interrupt value.
 */
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ assert(lock==&scheduler_lock && lock->state && !flags);lock->state=0; }
/** @brief Count the real vfork helper's wakeup request in the CPU model. */
void sched_request_reschedule_all_online(void) { ++reschedules; }
/** @brief Model an exec with no outstanding graphics continuation.
 * @param task Borrowed execution-owned task whose commit is being tested.
 */
void graphics_service_cancel_task_continuation(struct task *task)
{ assert(task && !scheduler_lock.state);++exec_cancels; }
/** @brief Supply a modeled kernel root for the actual exec commit.
 * @return Fixed nonzero kernel CR3.
 */
uint64_t paging_kernel_cr3(void) { return 0x1000; }
/** @brief Validate the actual exec commit switches to the kernel root.
 * @param cr3 Expected modeled kernel CR3.
 */
void paging_load_cr3(uint64_t cr3) { assert(cr3==0x1000 && !scheduler_lock.state); }
/** @brief Reject destruction of the shared MM still held by the fixture peer.
 * @param as Unexpected old root; this deliberately does not fake release.
 */
void address_space_destroy(struct address_space *as) { (void)as;abort(); }
/** @brief Reject final VMA release while another old-MM holder remains.
 * @param task Unexpected final owner; this deliberately does not fake release.
 */
void sched_task_vma_release(struct task *task) { (void)task;abort(); }

/** @brief Commit a new root with the actual exec and vfork-completion helpers.
 * @param task Borrowed child or departing protected owner.
 */
static void commit_other_mm(struct task *task)
{
    struct address_space replacement={.cr3=0x2000,.user_page_count=40};
    sched_exec_replace_mm(task,&replacement);
    assert(!task->shared_mm && sched_task_as(task)->cr3==0x2000 && exec_cancels==1);
}

/** @brief Require a still-eligible old root to be signalled, never false ENOMEM.
 * @param calls Completed batch counter, updated with every selector call.
 */
static void require_old_root_victim(unsigned *calls)
{
    while (!sent[3] && !sent[5]) {
        inspections=0; int rc=memory_oom_advance(1);
        assert(inspections<=1 && ++*calls<800);
        if (rc==-ENOMEM) fprintf(stderr,"false ENOMEM after eligibility change calls=%u\n",*calls);
        assert(rc==-EAGAIN || rc==-EINPROGRESS);
    }
}

/** @brief Revalidate a stale veto after actual child exec in OOM_VERIFY. */
static void verify_exec(void)
{
    struct task *parent=item(0,3,0,300), *child=item(1,4,0,300), *peer=item(2,5,0,300);
    spaces[0].references=3; parent->vfork_child=child; parent->state=TASK_BLOCKED;
    child->vfork_parent=parent; child->state=TASK_READY;
    unsigned calls=0;
    while (!(oom.phase==OOM_VERIFY && oom.candidate==peer->identity_token && oom.member==2)) {
        inspections=0; assert(memory_oom_advance(1)==-EAGAIN && inspections<=1 && ++calls<300);
    }
    assert(oom.denied);
    uint64_t revision=sched_oom_membership_revision();
    commit_other_mm(child);
    assert(!parent->vfork_child && !child->vfork_parent && parent->state==TASK_READY &&
           spaces[0].references==2 && reschedules==1);
    uint64_t changed=sched_oom_membership_revision()-revision;
    require_old_root_victim(&calls);
    assert(changed && !sent[4]);
    oom_reset(); release_identities();
    printf("PASS real vfork exec clears stale VERIFY veto, revision=%llu batches=%u\n",
           (unsigned long long)changed,calls);
}

/** @brief Revisit two earlier skipped vfork slots after actual child exec. */
static void primary_exec(void)
{
    struct task *parent=item(0,3,0,300), *child=item(1,4,0,300);
    spaces[0].references=2; parent->vfork_child=child; parent->state=TASK_BLOCKED;
    child->vfork_parent=parent; child->state=TASK_READY;
    unsigned calls=0;
    while (!(oom.phase==OOM_PICK && oom.next==2))
        assert(memory_oom_advance(1)==-EAGAIN && ++calls<100);
    uint64_t revision=sched_oom_membership_revision();
    commit_other_mm(child);
    require_old_root_victim(&calls);
    assert(sched_oom_membership_revision()!=revision && sent[3]==1);
    oom_reset(); release_identities();
    printf("PASS real vfork exec revisits primary cursor after %u batches\n",calls);
}

/** @brief Reconsider an old veto after the actual vfork-completion helper. */
static void complete_only(void)
{
    struct task *parent=item(0,3,0,300), *child=item(1,4,0,300), *peer=item(2,5,0,300);
    spaces[0].references=3;parent->vfork_child=child;parent->state=TASK_BLOCKED;
    child->vfork_parent=parent;child->state=TASK_READY;
    unsigned calls=0;
    while (!(oom.phase==OOM_VERIFY && oom.candidate==peer->identity_token && oom.member==2))
        assert(memory_oom_advance(1)==-EAGAIN && ++calls<300);
    assert(oom.denied);
    uint64_t revision=sched_oom_membership_revision();
    sched_vfork_child_done(child);
    assert(!parent->vfork_child && !child->vfork_parent && parent->state==TASK_READY &&
           spaces[0].references==3 && reschedules==1 && !exec_cancels);
    require_old_root_victim(&calls);
    assert(sched_oom_membership_revision()!=revision);
    oom_reset();release_identities();
    printf("PASS actual vfork completion alone invalidates stale veto after %u batches\n",calls);
}

/** @brief Reconsider a shared root after a protected owner execs away. */
static void protected_exec(void)
{
    struct task *protected=item(0,3,0,300), *peer=item(1,5,0,300);
    spaces[0].references=2; sched_oom_policy_set(protected,-1000,-1000);
    unsigned calls=0;
    while (!(oom.phase==OOM_VERIFY && oom.candidate==peer->identity_token && oom.member==1))
        assert(memory_oom_advance(1)==-EAGAIN && ++calls<200);
    assert(oom.denied);
    uint64_t revision=sched_oom_membership_revision();
    commit_other_mm(protected);
    require_old_root_victim(&calls);
    assert(sched_oom_membership_revision()!=revision && sent[5]==1 && !sent[3]);
    oom_reset(); release_identities();
    printf("PASS real non-vfork exec removes stale protected-MM veto after %u batches\n",calls);
}

/** @brief Select one actual-exec eligibility regression.
 * @param argc Argument count. @param argv Requested case name.
 * @return Zero after the relevant real selector and exec assertions.
 */
int main(int argc,char **argv)
{
    assert(argc==2);
    if (!strcmp(argv[1],"verify-exec")) verify_exec();
    else if (!strcmp(argv[1],"primary-exec")) primary_exec();
    else if (!strcmp(argv[1],"complete-only")) complete_only();
    else if (!strcmp(argv[1],"protected-exec")) protected_exec();
    else abort();
    return 0;
}
