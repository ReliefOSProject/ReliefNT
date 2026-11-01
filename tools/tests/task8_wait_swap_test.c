/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Exercise real wait event ownership through swap preparation.
 * The scheduler and syscall dispatcher are production code. Swap, IRQ state,
 * and deferred futex cleanup are controlled host models, not a guest runtime.
 */
#define main inherited_wait_exit_logging_main
#include "task_exit_logging_test.c"
#undef main
#include <reliefnt/swap.h>
#include <reliefnt/syscall_internal.h>

static int cleanup_result=1;
static uintptr_t swapped,invalid;
static bool contended;
static unsigned prepares;
bool kernel_execution_is_outermost(void) { return execution_lock_depth==1; }
int futex_task_exit_step(struct task *task)
{ assert(task && !scheduler_lock_depth);return cleanup_result; }
int syscall_unshare_task_files(struct task *task) { assert(task);return 0; }
void kernel_signal_detach_process(struct task *task) { assert(task); }
bool user_range_writable(uint64_t address,uint64_t size)
{ return address>=4096 && size<=UINT64_MAX-address && address!=swapped && address!=invalid; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t size,bool write)
{
    assert(task && write && !scheduler_lock_depth && execution_lock_depth==1 && size<=128);
    ++prepares;
    if(address==invalid)return -LINUX_EFAULT;
    if(address==swapped) {
        if(contended)return SWAP_STORAGE_CONTENDED;
        swapped=0;
    }
    return 0;
}

int64_t syscall_legacy_process(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static struct task *wait_fixture_task(uint32_t pid)
{
    struct task *old=fixture_task(pid,"wait-fixture");
    struct sched_task_object *object=calloc(1,sizeof(*object));assert(object);
    object->task=*old;free(old);return &object->task;
}

#ifdef TASK8_WAIT_FRAME_TEST
int inherited_task8_wait_swap_main(void)
#else
int main(void)
#endif
{
    struct task *parent=wait_fixture_task(50),*child=wait_fixture_task(51),*peer=wait_fixture_task(52);
    peer->tgid=50;
    struct task *table[]={parent,child,peer};install_tasks(table,3);current_pid[0]=50;
    parent->state=TASK_RUNNING;child->parent_pid=50;child->state=TASK_EXITED;
    child->exit_code=7;child->flags|=TASK_FLAG_RESOURCES_RELEASED;
    execution_lock_depth=1;interrupts_enabled=false;
    int status=-1;swapped=(uintptr_t)&status;contended=true;
    assert(syscall_legacy_process(LINUX_SYS_WAIT4,51,(uintptr_t)&status,0,0,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(child->pid==51 && status==-1);
    /* Cancellation owns no event: a later wait still sees this child. */
    contended=false;
    assert(syscall_legacy_process(LINUX_SYS_WAIT4,51,(uintptr_t)&status,0,0,0,0)==51);
    assert(child->pid==0 && status==(7<<8));

    child->pid=51;child->parent_pid=50;child->state=TASK_EXITED;
    child->flags=TASK_FLAG_WAITABLE_CHILD|TASK_FLAG_RESOURCES_RELEASED;
    struct linux_siginfo info={0};swapped=(uintptr_t)&info;contended=true;
    assert(syscall_legacy_process(LINUX_SYS_WAITID,1,51,(uintptr_t)&info,4|0x01000000u,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(child->pid==51);contended=false;
    assert(!syscall_legacy_process(LINUX_SYS_WAITID,1,51,(uintptr_t)&info,4|0x01000000u,0,0));
    assert(child->pid==51 && info.fields.child.pid==51);
    assert(!syscall_legacy_process(LINUX_SYS_WAITID,1,51,(uintptr_t)&info,4,0,0));
    assert(child->pid==0 && info.fields.child.status==7);

    child->pid=51;child->parent_pid=50;child->state=TASK_READY;
    child->flags=TASK_FLAG_WAITABLE_CHILD;status=123;swapped=(uintptr_t)&status;contended=true;
    unsigned old=prepares;
    assert(!syscall_legacy_process(LINUX_SYS_WAIT4,51,(uintptr_t)&status,1,0,0,0));
    assert(prepares==old && status==123);
    memset(&info,0x5a,sizeof(info));swapped=(uintptr_t)&info;
    assert(syscall_legacy_process(LINUX_SYS_WAITID,1,51,(uintptr_t)&info,4|1,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    contended=false;
    assert(!syscall_legacy_process(LINUX_SYS_WAITID,1,51,(uintptr_t)&info,4|1,0,0));
    assert(!info.signo && !info.fields.child.pid);

    /* A sibling may consume the unreserved event while the first waiter is
     * parked. Its retry must observe ECHILD, never a fabricated old result. */
    child->pid=51;child->parent_pid=50;child->state=TASK_EXITED;
    child->flags=TASK_FLAG_WAITABLE_CHILD|TASK_FLAG_RESOURCES_RELEASED;
    status=-1;swapped=(uintptr_t)&status;contended=true;
    assert(syscall_legacy_process(LINUX_SYS_WAIT4,51,(uintptr_t)&status,0,0,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(child->pid==51 && status==-1);
    int peer_status=-1;current_pid[0]=52;swapped=0;contended=false;
    assert(syscall_legacy_process(LINUX_SYS_WAIT4,51,(uintptr_t)&peer_status,0,0,0,0)==51);
    assert(peer_status==(7<<8) && !child->pid);
    current_pid[0]=50;
    assert(syscall_legacy_process(LINUX_SYS_WAIT4,51,(uintptr_t)&status,0,0,0,0)==-RELIEFOS_ECHILD);
    assert(status==-1);

    /* STOP and CONT notifications also remain queued on prerequisite wait;
     * WNOWAIT observes the event without clearing the scheduler's flag. */
    child->pid=51;child->parent_pid=50;child->state=TASK_STOPPED;
    child->flags=TASK_FLAG_WAITABLE_CHILD;child->child_event=TASK_CHILD_EVENT_STOPPED;
    child->stop_signal=19;memset(&info,0,sizeof(info));
    swapped=(uintptr_t)&info;contended=true;
    assert(syscall_legacy_process(LINUX_SYS_WAITID,1,51,(uintptr_t)&info,2|0x01000000u,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(child->child_event==TASK_CHILD_EVENT_STOPPED);
    contended=false;
    assert(!syscall_legacy_process(LINUX_SYS_WAITID,1,51,(uintptr_t)&info,2|0x01000000u,0,0));
    assert(info.code==5 && info.fields.child.status==19 && child->child_event==TASK_CHILD_EVENT_STOPPED);
    assert(!syscall_legacy_process(LINUX_SYS_WAITID,1,51,(uintptr_t)&info,2,0,0));
    assert(child->child_event==TASK_CHILD_EVENT_NONE);
    child->state=TASK_READY;child->child_event=TASK_CHILD_EVENT_CONTINUED;
    swapped=(uintptr_t)&info;contended=true;
    assert(syscall_legacy_process(LINUX_SYS_WAITID,1,51,(uintptr_t)&info,8,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(child->child_event==TASK_CHILD_EVENT_CONTINUED);contended=false;
    assert(!syscall_legacy_process(LINUX_SYS_WAITID,1,51,(uintptr_t)&info,8,0,0));
    assert(info.code==6 && info.fields.child.status==18 && child->child_event==TASK_CHILD_EVENT_NONE);

    child->state=TASK_READY;

    /* Deferred robust cleanup prevents observation and reaping before output. */
    child->state=TASK_EXITED;child->flags=TASK_FLAG_WAITABLE_CHILD;
    cleanup_result=SWAP_STORAGE_CONTENDED;swapped=(uintptr_t)&status;contended=true;old=prepares;
    assert(syscall_legacy_process(LINUX_SYS_WAIT4,51,(uintptr_t)&status,0,0,0,0)==-RELIEFOS_EAGAIN);
    assert(child->pid==51 && prepares==old && !(child->flags&TASK_FLAG_REAP_IN_PROGRESS));
    swapped=0;memset(&info,0x5a,sizeof(info));
    assert(!syscall_legacy_process(LINUX_SYS_WAITID,1,51,(uintptr_t)&info,4|1,0,0));
    assert(!info.signo && !info.fields.child.pid && child->pid==51);
    cleanup_result=1;contended=false;
    assert(syscall_legacy_process(LINUX_SYS_WAIT4,51,(uintptr_t)&status,0,0,0,0)==51);
    assert(!child->pid && status==(7<<8));

    /* A permanently invalid output keeps the inherited late-EFAULT order. */
    child->pid=51;child->parent_pid=50;child->state=TASK_EXITED;
    child->flags=TASK_FLAG_WAITABLE_CHILD|TASK_FLAG_RESOURCES_RELEASED;
    invalid=(uintptr_t)&status;swapped=0;
    assert(syscall_legacy_process(LINUX_SYS_WAIT4,51,(uintptr_t)&status,0,0,0,0)==-RELIEFOS_EFAULT);
    assert(!child->pid);
    memset(&info,0x5a,sizeof(info));invalid=(uintptr_t)&info;
    assert(syscall_legacy_process(LINUX_SYS_WAITID,1,51,(uintptr_t)&info,4,0,0)==-RELIEFOS_EFAULT);
    invalid=0;
    assert(syscall_legacy_process(LINUX_SYS_WAITID,1,51,(uintptr_t)&info,4,0,0)==-RELIEFOS_ECHILD);
    assert(!info.signo && !info.fields.child.pid);
    execution_lock_depth=0;interrupts_enabled=true;free(parent);free(child);free(peer);
    puts("PASS wait4/waitid readiness, swap retry, cleanup gate and late errors");return 0;
}
