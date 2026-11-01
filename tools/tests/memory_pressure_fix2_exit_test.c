/* Actual OOM selector, PID, SIGKILL queue and quiescent scheduler cleanup.
 * CPU execution, PTE counts and storage are the inherited host models. */
#define MEMORY_PRESSURE_OOM_LIFECYCLE_ENTRY inherited_lifecycle_main
#include "memory_pressure_oom_lifecycle_test.c"
#ifndef MEMORY_PRESSURE_FIX2_OOM_SOURCE
#define MEMORY_PRESSURE_FIX2_OOM_SOURCE "../../mm/reclaim/oom.c"
#endif
#include MEMORY_PRESSURE_FIX2_OOM_SOURCE

/** @brief Remove an exited protected holder after its veto was observed.
 */
static void exited_holder(void)
{
    struct task *protected=fixture_task(3,"protected-exited"), *peer=fixture_task(4,"peer");
    struct task *table[]={protected,peer};install_tasks(table,2);
    struct task_address_space_state *shared=calloc(1,sizeof(*shared));assert(shared);
    shared->references=2;shared->as.cr3=0x3000;shared->as.user_page_count=300;
    protected->shared_mm=peer->shared_mm=shared;protected->flags|=TASK_FLAG_SERVICE;
    protected->state=TASK_EXITED;protected->running_cpu=SCHED_CPU_NONE;
    peer->state=TASK_READY;
    identity(protected);identity(peer);
    unsigned calls=0;
    while (!(oom.phase==OOM_VERIFY && oom.candidate==peer->identity_token && oom.member==1))
        assert(memory_oom_advance(1)==-EAGAIN && ++calls<100);
    assert(oom.denied);
    uint64_t revision=sched_oom_membership_revision();
    sched_release_task_resources(protected);
    assert((protected->flags&TASK_FLAG_RESOURCES_RELEASED) && shared->references==1);
    while (!(*sched_task_process_pending(peer)&(1ULL<<8))) {
        int rc=memory_oom_advance(1);assert(++calls<500);
        if (rc==-ENOMEM) fprintf(stderr,"false ENOMEM after actual holder cleanup\n");
        assert(rc==-EAGAIN || rc==-EINPROGRESS);
    }
    assert(sched_oom_membership_revision()!=revision);
    oom_reset();sched_exit(peer->pid,9);
    sched_task_identity_detach(protected);sched_task_identity_detach(peer);
    free(protected);free(peer);
    assert(!scheduler_lock_depth && !other_lock_depth);
    printf("PASS actual exited-holder cleanup invalidates veto after %u batches\n",calls);
}

/** @brief Retire a vfork parent through the actual scheduler cleanup path. */
static void exited_vfork_parent(void)
{
    struct task *parent=fixture_task(3,"exited-parent"), *child=fixture_task(4,"vfork-child");
    struct task *peer=fixture_task(5,"peer");
    struct task *table[]={parent,child,peer};install_tasks(table,3);
    struct task_address_space_state *shared=calloc(1,sizeof(*shared));assert(shared);
    shared->references=3;shared->as.cr3=0x3000;shared->as.user_page_count=300;
    parent->shared_mm=child->shared_mm=peer->shared_mm=shared;
    parent->state=TASK_EXITED;parent->running_cpu=SCHED_CPU_NONE;
    parent->vfork_child=child;child->vfork_parent=parent;
    identity(parent);identity(child);identity(peer);
    unsigned calls=0;
    while (!(oom.phase==OOM_VERIFY && oom.candidate==peer->identity_token && oom.member==2))
        assert(memory_oom_advance(1)==-EAGAIN && ++calls<200);
    assert(oom.denied);
    uint64_t revision=sched_oom_membership_revision();
    sched_release_task_resources(parent);
    assert((parent->flags&TASK_FLAG_RESOURCES_RELEASED) && shared->references==2 &&
           !parent->vfork_child && !child->vfork_parent);
    uint64_t changes=sched_oom_membership_revision()-revision;
    while (!*sched_task_process_pending(child) && !*sched_task_process_pending(peer)) {
        int rc=memory_oom_advance(1);assert(++calls<500);
        if (rc==-ENOMEM) fprintf(stderr,"false ENOMEM after actual vfork parent cleanup\n");
        assert(rc==-EAGAIN || rc==-EINPROGRESS);
    }
    assert(changes>=2);
    oom_reset();sched_exit(child->pid,9);sched_exit(peer->pid,9);
    for (unsigned i=0;i<3;++i) {sched_task_identity_detach(table[i]);free(table[i]);}
    assert(!scheduler_lock_depth && !other_lock_depth);
    printf("PASS actual vfork-parent exit clears relation and stale veto changes=%llu batches=%u\n",
           (unsigned long long)changes,calls);
}

/** @brief Keep a published victim until its actual quiescent MM release. */
static void published_victim_churn(void)
{
    struct task *victim=fixture_task(3,"running-victim"), *other=fixture_task(4,"other");
    struct task *table[]={victim,other};install_tasks(table,2);
    sched_task_as(victim)->cr3=0x3000;sched_task_as(victim)->user_page_count=300;
    sched_task_as(other)->cr3=0x4000;sched_task_as(other)->user_page_count=100;
    victim->state=TASK_RUNNING;victim->running_cpu=1;other->state=TASK_READY;
    identity(victim);identity(other);
    unsigned calls=0;
    while (!(*sched_task_process_pending(victim)&(1ULL<<8))) {
        int rc=memory_oom_advance(1);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<200 &&
               !*sched_task_process_pending(other));
    }
    assert(oom.sent && sched_oom_victim_marked(victim,oom.victim_epoch));
    bool bounded=false;
    for (unsigned i=0;i<40;++i) {
        sched_oom_policy_set(other,(i&1)?0:1,0);
        int rc=memory_oom_advance(1);
        assert(!*sched_task_process_pending(other));
        if (rc==-ENOMEM) { bounded=true;break; }
        assert(rc==-EAGAIN || rc==-EINPROGRESS);
    }
    if (!bounded) fprintf(stderr,"published victim: churn did not terminate within 40 batches\n");
    /* Invalidation alone must retain the published root for revalidation. */
    assert(bounded && oom.sent && !oom.abandon &&
           sched_oom_victim_marked(victim,oom.victim_epoch));
    for (unsigned i=0;i<30;++i) {
        int rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && !*sched_task_process_pending(other));
    }
    sched_exit(victim->pid,9);sched_release_task_resources(victim);
    assert(victim->running_cpu==1 && !(victim->flags&TASK_FLAG_RESOURCES_RELEASED));
    for (unsigned i=0;i<20;++i) {
        int rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && !*sched_task_process_pending(other));
    }
    victim->running_cpu=SCHED_CPU_NONE;sched_release_task_resources(victim);
    assert(victim->flags&TASK_FLAG_RESOURCES_RELEASED);
    while (!(*sched_task_process_pending(other)&(1ULL<<8))) {
        int rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<500);
    }
    oom_reset();sched_exit(other->pid,9);
    sched_task_identity_detach(victim);sched_task_identity_detach(other);
    free(victim);free(other);
    assert(!scheduler_lock_depth && !other_lock_depth);
    printf("PASS bounded churn preserves actual published victim until quiescent release (%u batches)\n",calls);
}

/** @brief Select actual scheduler-cleanup or in-flight membership-churn control.
 * @param argc Argument count. @param argv Optional control name.
 * @return Zero after the real selector/signal/retirement assertions.
 */
int main(int argc,char **argv)
{
    if (argc==1) exited_holder();
    else if (argc==2 && !strcmp(argv[1],"wait-churn")) published_victim_churn();
    else if (argc==2 && !strcmp(argv[1],"vfork-exit")) exited_vfork_parent();
    else abort();
    return 0;
}
