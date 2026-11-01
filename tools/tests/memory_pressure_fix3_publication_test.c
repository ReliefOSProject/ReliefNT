/* Actual selector, PID/policy, SIGKILL queue and scheduler resource release.
 * CPU quiescence, MM page counts, storage and address-space destruction are
 * host models. The destroy hook counts modeled pages only at last release. */
#define PRESSURE_DESTROY_HOOK 1
#define MEMORY_PRESSURE_OOM_LIFECYCLE_ENTRY inherited_lifecycle_main
#include "memory_pressure_oom_lifecycle_test.c"
#include "../../mm/reclaim/oom.c"

static uint64_t retired_pages;
static unsigned destroyed_roots;

/** @brief Count the model MM's pages only when real cleanup destroys its root.
 * @param as Borrowed address space at the modeled destruction endpoint.
 */
void pressure_destroy_hook(struct address_space *as)
{
    if (as->cr3 == 0x3000) {
        ++destroyed_roots;
        retired_pages += as->user_page_count;
    }
}

/** @brief Retain an in-flight shared root across release or repeated changes.
 * @param churn True to invalidate a partially signaled scan eight times.
 * Every selector call uses budget 32. Independent tasks in slots 28 and 30
 * share an MM; an equally scored different root occupies intervening slot 29.
 */
static void partial_publication(bool churn)
{
    struct task *table[31];
    for (unsigned i=0;i<28;++i) {
        table[i]=fixture_task(100+i,"filler"); table[i]->kind=TASK_KIND_KERNEL;
    }
    struct task *first=table[28]=fixture_task(3,"first");
    struct task *other=table[29]=fixture_task(5,"other");
    struct task *peer=table[30]=fixture_task(4,"independent-peer");
    install_tasks(table,31);
    struct task_address_space_state *shared=calloc(1,sizeof(*shared)); assert(shared);
    shared->references=2; shared->as.cr3=0x3000; shared->as.user_page_count=300;
    first->shared_mm=peer->shared_mm=shared;
    first->state=TASK_RUNNING; first->running_cpu=1;
    peer->state=TASK_RUNNING; peer->running_cpu=2;
    sched_task_as(other)->cr3=0x5000; sched_task_as(other)->user_page_count=300;
    identity(first); identity(peer); identity(other);

    unsigned calls=0;
    int rc;
    while (!(*sched_task_process_pending(first)&(1ULL<<8))) {
        rc=memory_oom_advance(32);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<2000);
        assert(!*sched_task_process_pending(peer) && !*sched_task_process_pending(other));
    }
    assert(rc==-EAGAIN && oom.sent && oom.phase==OOM_SIGNAL && oom.member==29);
    uint64_t epoch=oom.victim_epoch;
    assert(sched_oom_victim_marked(first,epoch));
    assert(!sched_oom_victim_marked(peer,epoch));
    assert(!*sched_task_process_pending(peer) && !*sched_task_process_pending(other));

    if (churn) {
        for (unsigned i=0;i<8;++i) {
            sched_oom_policy_set(other,(i&1)?0:1,0);
            rc=memory_oom_advance(32);
            assert(!*sched_task_process_pending(peer) && !*sched_task_process_pending(other));
            assert(i==7 ? rc==-ENOMEM : rc==-EAGAIN);
        }
        assert(oom.sent && oom.victim_epoch==epoch);
    }

    uint64_t revision=sched_oom_membership_revision();
    sched_exit(first->pid,9);
    sched_release_task_resources(first);
    assert(!(first->flags&TASK_FLAG_RESOURCES_RELEASED) && shared->references==2);
    assert(!retired_pages && !destroyed_roots);
    first->running_cpu=SCHED_CPU_NONE;
    sched_release_task_resources(first);
    assert((first->flags&TASK_FLAG_RESOURCES_RELEASED) && shared->references==1);
    assert(sched_oom_membership_revision()!=revision && !retired_pages && !destroyed_roots);

    unsigned peer_batches=0;
    while (!(*sched_task_process_pending(peer)&(1ULL<<8))) {
        rc=memory_oom_advance(32);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++peer_batches<300);
        assert(!*sched_task_process_pending(other));
        assert(!retired_pages && !destroyed_roots);
    }
    assert(sched_oom_victim_marked(peer,epoch) && peer->running_cpu==2);
    assert(!*sched_task_process_pending(first) && !*sched_task_process_pending(other));
    for (unsigned i=0;i<40;++i) {
        rc=memory_oom_advance(32);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && !*sched_task_process_pending(other));
        assert(!retired_pages && !destroyed_roots && shared->references==1);
    }
    sched_exit(peer->pid,9); sched_release_task_resources(peer);
    assert(!(peer->flags&TASK_FLAG_RESOURCES_RELEASED) && shared->references==1);
    for (unsigned i=0;i<40;++i) {
        rc=memory_oom_advance(32);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && !*sched_task_process_pending(other));
        assert(!retired_pages && !destroyed_roots);
    }
    peer->running_cpu=SCHED_CPU_NONE; sched_release_task_resources(peer);
    assert((peer->flags&TASK_FLAG_RESOURCES_RELEASED) && !peer->shared_mm);
    assert(retired_pages==300 && destroyed_roots==1);

    unsigned drain=0;
    while (first->identity_token->references>1) {
        rc=memory_oom_advance(32);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++drain<100);
        assert(!*sched_task_process_pending(other));
    }
    assert(!oom.sent && !oom.best);
    unsigned other_batches=0;
    while (!(*sched_task_process_pending(other)&(1ULL<<8))) {
        rc=memory_oom_advance(32);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++other_batches<300);
    }
    assert(retired_pages==300 && destroyed_roots==1);
    oom_reset(); sched_exit(other->pid,9);
    sched_task_identity_detach(first); sched_task_identity_detach(peer);
    sched_task_identity_detach(other);
    for (unsigned i=0;i<31;++i) free(table[i]);
    assert(!scheduler_lock_depth && !other_lock_depth);
    printf("PASS partial shared MM: budget=32 churn=%u first_calls=%u peer_batches=%u "
           "drain=%u next_root=%u modeled_pages_after_last_release=%llu\n",
           churn,calls,peer_batches,drain,other_batches,(unsigned long long)retired_pages);
}

#ifndef MEMORY_PRESSURE_FIX3_PUBLICATION_ENTRY
#define MEMORY_PRESSURE_FIX3_PUBLICATION_ENTRY main
#endif
/** @brief Run a single release or eight-invalidation publication control.
 * @param argc Argument count. @param argv Case name.
 * @return Zero after all signal, ownership, quiescence and retirement checks.
 */
int MEMORY_PRESSURE_FIX3_PUBLICATION_ENTRY(int argc, char **argv)
{
    assert(argc==2);
    if (!strcmp(argv[1],"release")) partial_publication(false);
    else if (!strcmp(argv[1],"churn")) partial_publication(true);
    else abort();
    return 0;
}
