/* Extend the real selector/signal/cleanup fixture with owners joining after
 * the original representative retires. CPU, PTE and destroy endpoints are
 * modeled as documented by the included fixture. */
#define MEMORY_PRESSURE_FIX3_PUBLICATION_ENTRY initial_publication_main
#include "memory_pressure_fix3_publication_test.c"

enum late_kind { LATE_ELIGIBLE, LATE_PROTECTED, LATE_VFORK };

/** @brief Revalidate a new MM holder after the representative actually leaves.
 * @param kind Eligible, policy-protected, or modeled vfork-participant holder.
 * Actual PID binding notifies membership; policy uses the real scheduler API.
 * Unsafe holders must stay unsignalled and cannot cause an unbounded wait.
 */
static void late_holder(enum late_kind kind)
{
    struct task *first=fixture_task(3,"first"), *other=fixture_task(5,"other");
    struct task *peer=fixture_task(4,"peer"), *late=fixture_task(6,"late");
    late->kind=TASK_KIND_KERNEL;
    struct task *table[]={first,other,peer,late}; install_tasks(table,4);
    struct task_address_space_state *shared=calloc(1,sizeof(*shared)); assert(shared);
    shared->references=2; shared->as.cr3=0x3000; shared->as.user_page_count=300;
    first->shared_mm=peer->shared_mm=shared;
    first->state=peer->state=TASK_RUNNING; first->running_cpu=1; peer->running_cpu=2;
    sched_task_as(other)->cr3=0x5000; sched_task_as(other)->user_page_count=100;
    identity(first); identity(peer); identity(other);
    unsigned calls=0;
    while (!(*sched_task_process_pending(first)&(1ULL<<8))) {
        int rc=memory_oom_advance(1);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<500);
        assert(!*sched_task_process_pending(peer) && !*sched_task_process_pending(other));
    }
    uint64_t epoch=oom.victim_epoch;
    assert(oom.sent && oom.phase==OOM_SIGNAL && sched_oom_victim_marked(first,epoch));
    sched_exit(first->pid,9); first->running_cpu=SCHED_CPU_NONE;
    sched_release_task_resources(first);
    assert((first->flags&TASK_FLAG_RESOURCES_RELEASED) && shared->references==1);
    assert(!retired_pages && !destroyed_roots);

    late->kind=TASK_KIND_USER; late->shared_mm=shared; ++shared->references;
    if (kind==LATE_VFORK) {
        peer->state=TASK_BLOCKED; peer->running_cpu=SCHED_CPU_NONE;
        peer->vfork_child=late; late->vfork_parent=peer;
    } else if (kind==LATE_ELIGIBLE) {
        late->state=TASK_RUNNING; late->running_cpu=3;
    }
    identity(late);
    if (kind==LATE_PROTECTED) sched_oom_policy_set(late,-1000,-1000);

    unsigned after=0;
    if (kind==LATE_ELIGIBLE) {
        while (!(*sched_task_process_pending(peer)&(1ULL<<8)) ||
               !(*sched_task_process_pending(late)&(1ULL<<8))) {
            int rc=memory_oom_advance(32);
            assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++after<200);
            assert(!*sched_task_process_pending(other) && !retired_pages);
        }
        assert(sched_oom_victim_marked(peer,epoch) && sched_oom_victim_marked(late,epoch));
        sched_exit(peer->pid,9); sched_release_task_resources(peer);
        assert(!(peer->flags&TASK_FLAG_RESOURCES_RELEASED) && shared->references==2);
        peer->running_cpu=SCHED_CPU_NONE; sched_release_task_resources(peer);
        assert(shared->references==1 && !retired_pages);
        for (unsigned i=0;i<20;++i) {
            int rc=memory_oom_advance(32);
            assert((rc==-EAGAIN || rc==-EINPROGRESS) && !*sched_task_process_pending(other));
            assert(!(late->flags&TASK_FLAG_RESOURCES_RELEASED) && !retired_pages);
        }
        sched_exit(late->pid,9); sched_release_task_resources(late);
        assert(!(late->flags&TASK_FLAG_RESOURCES_RELEASED) && shared->references==1);
        late->running_cpu=SCHED_CPU_NONE; sched_release_task_resources(late);
        assert((late->flags&TASK_FLAG_RESOURCES_RELEASED) && !late->shared_mm);
        assert(retired_pages==300 && destroyed_roots==1);
    } else {
        while (!(*sched_task_process_pending(other)&(1ULL<<8))) {
            int rc=memory_oom_advance(32);
            assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++after<200);
            assert(!*sched_task_process_pending(peer) && !*sched_task_process_pending(late));
            assert(shared->references==2 && !retired_pages);
        }
        assert(!(peer->flags&TASK_FLAG_RESOURCES_RELEASED) &&
               !(late->flags&TASK_FLAG_RESOURCES_RELEASED));
        peer->running_cpu=SCHED_CPU_NONE; sched_exit(peer->pid,9);
        assert(shared->references==1 && !retired_pages);
        sched_exit(late->pid,9);
        assert((late->flags&TASK_FLAG_RESOURCES_RELEASED) && retired_pages==300 && destroyed_roots==1);
    }
    if (kind==LATE_ELIGIBLE) {
        unsigned drain=0;
        while (first->identity_token->references>1) {
            int rc=memory_oom_advance(32);
            assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++drain<100);
            assert(!*sched_task_process_pending(other));
        }
        while (!(*sched_task_process_pending(other)&(1ULL<<8))) {
            int rc=memory_oom_advance(32);
            assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++after<300);
        }
    }
    oom_reset(); sched_exit(other->pid,9);
    for (unsigned i=0;i<4;++i) { sched_task_identity_detach(table[i]); free(table[i]); }
    assert(!scheduler_lock_depth && !other_lock_depth);
    printf("PASS late holder after representative release: kind=%u batches=%u modeled_retired_pages=%llu\n",
           (unsigned)kind,after,(unsigned long long)retired_pages);
}

/** @brief Wait for an unmarked exited sibling and a live independent MM peer.
 * Uses the real group-exit, SIGKILL, identity and quiescent release paths;
 * CPU residency and the final address-space destruction endpoint are modeled.
 */
static void exited_sibling(void)
{
    struct task *first=fixture_task(3,"first"), *other=fixture_task(5,"other");
    struct task *sibling=fixture_task(6,"sibling"), *peer=fixture_task(4,"peer");
    struct task *table[]={first,other,sibling,peer}; install_tasks(table,4);
    struct task_address_space_state *shared=calloc(1,sizeof(*shared)); assert(shared);
    shared->references=3; shared->as.cr3=0x3000; shared->as.user_page_count=300;
    first->shared_mm=sibling->shared_mm=peer->shared_mm=shared;
    sibling->tgid=first->tgid;
    sibling->shared_process_pending=&first->process_pending_signals;
    sibling->shared_process_signal_queue=&first->process_signal_queue;
    first->state=sibling->state=peer->state=TASK_RUNNING;
    first->running_cpu=1; sibling->running_cpu=2; peer->running_cpu=3;
    sched_task_as(other)->cr3=0x5000; sched_task_as(other)->user_page_count=100;
    for (unsigned i=0;i<4;++i) identity(table[i]);
    unsigned calls=0;
    while (!(*sched_task_process_pending(first)&(1ULL<<8))) {
        int rc=memory_oom_advance(1);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<500);
        assert(!*sched_task_process_pending(peer) && !*sched_task_process_pending(other));
    }
    uint64_t epoch=oom.victim_epoch;
    assert(!sched_oom_victim_marked(sibling,epoch));
    sched_exit_group(first->tgid,9);
    assert(first->state==TASK_EXITED && sibling->state==TASK_EXITED && sibling->running_cpu==2);
    first->running_cpu=SCHED_CPU_NONE; sched_release_task_resources(first);
    assert((first->flags&TASK_FLAG_RESOURCES_RELEASED) && shared->references==2 && !retired_pages);
    unsigned after=0;
    while (!(*sched_task_process_pending(peer)&(1ULL<<8))) {
        int rc=memory_oom_advance(32);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++after<200);
        assert(!*sched_task_process_pending(other) && !retired_pages);
    }
    assert(sched_oom_victim_marked(peer,epoch) && !sched_oom_victim_marked(sibling,epoch));
    sched_exit(peer->pid,9); peer->running_cpu=SCHED_CPU_NONE; sched_release_task_resources(peer);
    assert(shared->references==1 && !retired_pages);
    sched_release_task_resources(sibling);
    assert(!(sibling->flags&TASK_FLAG_RESOURCES_RELEASED));
    for (unsigned i=0;i<30;++i) {
        int rc=memory_oom_advance(32);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && !*sched_task_process_pending(other));
        assert(shared->references==1 && !retired_pages);
    }
    sibling->running_cpu=SCHED_CPU_NONE; sched_release_task_resources(sibling);
    assert((sibling->flags&TASK_FLAG_RESOURCES_RELEASED) && retired_pages==300 && destroyed_roots==1);
    while (first->identity_token->references>1) {
        int rc=memory_oom_advance(32);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++after<300 && !*sched_task_process_pending(other));
    }
    while (!(*sched_task_process_pending(other)&(1ULL<<8))) {
        int rc=memory_oom_advance(32); assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++after<500);
    }
    oom_reset(); sched_exit(other->pid,9);
    for (unsigned i=0;i<4;++i) { sched_task_identity_detach(table[i]); free(table[i]); }
    assert(!scheduler_lock_depth && !other_lock_depth);
    printf("PASS actual group exit: unmarked running sibling retired before another MM; batches=%u\n",after);
}

/** @brief Run a late-holder or group-exit regression.
 * @param argc Argument count. @param argv One documented case name.
 * @return Zero when real signaling and release assertions pass.
 */
int main(int argc, char **argv)
{
    assert(argc==2);
    if (!strcmp(argv[1],"late-eligible")) late_holder(LATE_ELIGIBLE);
    else if (!strcmp(argv[1],"late-protected")) late_holder(LATE_PROTECTED);
    else if (!strcmp(argv[1],"late-vfork")) late_holder(LATE_VFORK);
    else if (!strcmp(argv[1],"exited-sibling")) exited_sibling();
    else abort();
    return 0;
}
