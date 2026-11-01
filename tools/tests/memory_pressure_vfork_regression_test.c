/* Real selector and PID/policy objects; task slots, signals and CPUs modeled. */
#define main inherited_oom_test_main
#include "memory_pressure_oom_test.c"
#undef main
#include "../../mm/reclaim/oom.c"

/** @brief Drive the bounded selector until a full wait scan has completed.
 * @return Number of batches. Fixture assertions bound each table inspection.
 */
static unsigned wait_for_inflight(void)
{
    unsigned calls = 0;
    for (;;) {
        inspections = 0;
        int rc = memory_oom_advance(2);
        assert(inspections <= 2 && ++calls < 500);
        if (rc == -EINPROGRESS) return calls;
        assert(rc == -EAGAIN);
    }
}

/** @brief Drain the selector after all eligible fixture victims retired. */
static void drain_selector(void)
{
    unsigned calls = 0;
    int rc;
    do { rc = memory_oom_advance(2); assert(++calls < 500); }
    while (rc == -EAGAIN || rc == -EINPROGRESS);
    assert(rc == -ENOMEM);
}

/** @brief Do not choose an MM containing blocked vfork participants. */
static void vfork_alternative(void)
{
    struct task *parent=item(0,3,0,300), *child=item(1,4,0,300);
    struct task *peer=item(2,5,0,300), *alternative=item(3,6,1,100);
    parent->vfork_child=child; parent->state=TASK_BLOCKED;
    child->vfork_parent=parent; child->state=TASK_BLOCKED;
    unsigned calls=0;
    while (!sent[6]) {
        inspections=0; int rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && inspections<=2 && ++calls<500);
        assert(!sent[3] && !sent[4] && !sent[5]);
    }
    assert(sent[6]==1 && peer->state==TASK_READY);
    alternative->state=TASK_EXITED; alternative->flags|=TASK_FLAG_RESOURCES_RELEASED;
    drain_selector(); release_identities();
    printf("PASS vfork MM skipped, alternate victim signalled after %u batches\n", calls);
}

/** @brief Signal an eligible sharer that appears after the first signal scan. */
static void late_sharer(void)
{
    struct task *a=item(0,3,0,300), *b=item(1,4,0,300);
    struct task *alternative=item(2,5,1,100);
    a->state=TASK_RUNNING; a->running_cpu=1;
    wait_for_inflight();
    assert(sent[3]==1 && sent[4]==1 && !sent[5]);
    struct task *late=item(3,6,0,300);
    unsigned calls=0;
    while (!sent[6]) {
        int rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<300 && !sent[5]);
    }
    assert(sent[6]==1);
    b->state=late->state=TASK_EXITED;
    b->flags|=TASK_FLAG_RESOURCES_RELEASED; late->flags|=TASK_FLAG_RESOURCES_RELEASED;
    for (unsigned i=0;i<30;++i) {
        int rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && !sent[5]);
    }
    assert(a->running_cpu==1 && !(a->flags&TASK_FLAG_RESOURCES_RELEASED));
    a->running_cpu=SCHED_CPU_NONE; a->state=TASK_EXITED; a->flags|=TASK_FLAG_RESOURCES_RELEASED;
    alternative->flags|=TASK_FLAG_RESOURCES_RELEASED;
    drain_selector(); release_identities();
    printf("PASS late same-MM sharer signalled once; running victim retained (%u batches)\n",calls);
}

/** @brief Wait for already signalled owners, then abandon a new unsafe MM. */
static void late_vfork(void)
{
    struct task *owner=item(0,3,0,300), *alternative=item(1,4,1,100);
    owner->state=TASK_RUNNING; owner->running_cpu=1;
    wait_for_inflight(); assert(sent[3]==1 && !sent[4]);
    struct task *child=item(2,5,0,300);
    owner->vfork_child=child; child->vfork_parent=owner; child->state=TASK_BLOCKED;
    for (unsigned i=0;i<30;++i) {
        int rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && !sent[4] && !sent[5]);
    }
    assert(!(owner->flags&TASK_FLAG_RESOURCES_RELEASED) && owner->running_cpu==1);
    owner->running_cpu=SCHED_CPU_NONE; owner->state=TASK_EXITED;
    owner->flags|=TASK_FLAG_RESOURCES_RELEASED;
    unsigned calls=0;
    while (!sent[4]) {
        int rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<500 && !sent[5]);
    }
    alternative->state=TASK_EXITED; alternative->flags|=TASK_FLAG_RESOURCES_RELEASED;
    drain_selector(); release_identities();
    printf("PASS late vfork holder skipped after original victim retired (%u batches)\n",calls);
}

/** @brief Catch a vfork holder inserted behind an in-progress signal cursor. */
static void vfork_behind_signal_cursor(void)
{
    struct task *owner=item(2,3,0,300), *parent=item(4,4,0,300);
    struct task *alternative=item(5,5,1,100);
    unsigned calls=0;
    while (oom.phase != OOM_SIGNAL || oom.member != 1) {
        int rc=memory_oom_advance(1);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<500);
    }
    assert(!sent[3] && !sent[4] && !sent[5]);
    struct task *child=item(0,6,0,300);
    child->vfork_parent=parent; child->state=TASK_BLOCKED;
    parent->vfork_child=child; parent->state=TASK_BLOCKED;
    while (!sent[5]) {
        int rc=memory_oom_advance(1);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<1200);
        assert(!sent[3] && !sent[4] && !sent[6]);
    }
    assert(owner->state==TASK_READY && sent[5]==1);
    alternative->state=TASK_EXITED; alternative->flags|=TASK_FLAG_RESOURCES_RELEASED;
    drain_selector(); release_identities();
    printf("PASS new vfork behind signal cursor triggers revalidation (%u batches)\n",calls);
}

/** @brief Revalidate when a scanned same-MM holder becomes OOM-protected. */
static void protection_behind_signal_cursor(void)
{
    struct task *protected=item(0,3,0,300), *owner=item(2,4,0,300);
    struct task *alternative=item(4,5,1,100);
    unsigned calls=0;
    while (oom.phase != OOM_SIGNAL || oom.member != 1) {
        int rc=memory_oom_advance(1);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<500);
    }
    assert(sent[3]==1 && !sent[4] && !sent[5]);
    sched_oom_policy_set(protected,-1000,-1000);
    for (unsigned i=0;i<30;++i) {
        int rc=memory_oom_advance(1);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && !sent[4] && !sent[5]);
    }
    protected->state=TASK_EXITED; protected->flags|=TASK_FLAG_RESOURCES_RELEASED;
    while (!sent[4]) {
        int rc=memory_oom_advance(1);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<1200 && !sent[5]);
    }
    assert(owner->state==TASK_READY && sent[4]==1);
    alternative->state=TASK_EXITED; alternative->flags|=TASK_FLAG_RESOURCES_RELEASED;
    owner->state=TASK_EXITED; owner->flags|=TASK_FLAG_RESOURCES_RELEASED;
    drain_selector(); release_identities();
    printf("PASS policy change behind signal cursor waits for published victim (%u batches)\n",calls);
}

/** @brief Restart a wait scan when a new same-MM owner appears behind it. */
static void sharer_behind_wait_cursor(void)
{
    struct task *owner=item(2,3,0,300), *alternative=item(5,5,1,100);
    owner->state=TASK_RUNNING; owner->running_cpu=1;
    wait_for_inflight();
    unsigned calls=0;
    while (oom.phase!=OOM_WAIT || oom.member<4) {
        int rc=memory_oom_advance(1);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<300);
    }
    struct task *late=item(0,6,0,300);
    while (!sent[6]) {
        int rc=memory_oom_advance(1);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<700 && !sent[5]);
    }
    assert(sent[3]==1 && sent[6]==1);
    late->state=owner->state=TASK_EXITED;
    late->flags|=TASK_FLAG_RESOURCES_RELEASED; owner->flags|=TASK_FLAG_RESOURCES_RELEASED;
    owner->running_cpu=SCHED_CPU_NONE;
    alternative->flags|=TASK_FLAG_RESOURCES_RELEASED;
    drain_selector(); release_identities();
    printf("PASS new sharer behind wait cursor is found and signalled (%u batches)\n",calls);
}

/** @brief Do not abandon a group-exited holder before its CPU quiesces. */
static void late_vfork_group_exit(void)
{
    struct task *owner=item(0,3,0,300), *sibling=item(1,4,0,300);
    struct task *parent=item(2,5,0,300), *alternative=item(3,6,1,100);
    owner->tgid=sibling->tgid=3;
    sibling->shared_process_pending=&owner->process_pending_signals;
    unsigned calls=0;
    while (oom.phase!=OOM_SIGNAL || oom.member!=1) {
        int rc=memory_oom_advance(1);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<500);
    }
    assert(sent[3]==1 && !sched_oom_victim_marked(sibling,oom.victim_epoch));
    struct task *child=item(4,7,0,300);
    parent->vfork_child=child; parent->state=TASK_BLOCKED;
    child->vfork_parent=parent; child->state=TASK_BLOCKED;
    while (!oom.abandon) {
        int rc=memory_oom_advance(1);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<700 && !sent[6]);
    }
    /* Model sched_exit_group after the first process-directed SIGKILL. The
     * signalled owner retired but its unmarked sibling is still on CPU 1. */
    owner->state=sibling->state=TASK_EXITED;
    owner->flags|=TASK_FLAG_RESOURCES_RELEASED;
    sibling->running_cpu=1;
    for (unsigned i=0;i<100;++i) {
        int rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && !sent[6]);
    }
    sibling->running_cpu=SCHED_CPU_NONE; sibling->flags|=TASK_FLAG_RESOURCES_RELEASED;
    while (!sent[6]) {
        int rc=memory_oom_advance(2);
        assert((rc==-EAGAIN || rc==-EINPROGRESS) && ++calls<1000);
    }
    alternative->state=TASK_EXITED; alternative->flags|=TASK_FLAG_RESOURCES_RELEASED;
    drain_selector(); release_identities();
    printf("PASS unmarked group-exit holder retires before alternate victim (%u batches)\n",calls);
}

/** @brief Select one deterministic OOM shared-MM regression.
 * @param argc Argument count. @param argv Case name.
 * @return Zero after all signal, retention and selection assertions.
 */
int main(int argc, char **argv)
{
    assert(argc==2);
    if (!strcmp(argv[1],"vfork")) vfork_alternative();
    else if (!strcmp(argv[1],"late")) late_sharer();
    else if (!strcmp(argv[1],"late-vfork")) late_vfork();
    else if (!strcmp(argv[1],"late-before-signal")) vfork_behind_signal_cursor();
    else if (!strcmp(argv[1],"policy-cursor")) protection_behind_signal_cursor();
    else if (!strcmp(argv[1],"wait-cursor")) sharer_behind_wait_cursor();
    else if (!strcmp(argv[1],"group-exit")) late_vfork_group_exit();
    else assert(!"unknown regression");
    return 0;
}
