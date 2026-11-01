/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Real prlimit, PID allocation, reap and identity ownership across wait.
 * Scheduling, swap and physical mappings are modeled by the host boundary.
 */
#define main inherited_prlimit_exit_logging_main
#include "task_exit_logging_test.c"
#undef main
#include "../../kernel/syscall/process/process.c"
#include <reliefnt/syscall_internal.h>

static uint64_t blocked_output;
static bool contended;
bool kernel_execution_is_outermost(void) { return execution_lock_depth==1; }
bool user_range_ok(uint64_t address,uint64_t size)
{ return address>=4096 && size<=UINT64_MAX-address; }
bool user_range_writable(uint64_t address,uint64_t size)
{ return user_range_ok(address,size); }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t size,bool writing)
{
    assert(task==sched_current_task() && size<=16 && !scheduler_lock_depth && execution_lock_depth==1);
    return writing && address==blocked_output && contended ? SWAP_STORAGE_CONTENDED : 0;
}
void task_process_vm_scope_release(void *state) { (void)state;abort(); }
void vfs_walk_cancel(struct vfs_walk **walk) { assert(!*walk); }
int futex_task_exit_step(struct task *task) { (void)task;abort(); }
void kernel_signal_detach_process(struct task *task) { (void)task;abort(); }
int syscall_unshare_task_files(struct task *task) { (void)task;abort(); }

/** @brief Allocate a task with the private scheduler tail required by reuse. */
static struct task *fixture_object(uint32_t pid)
{
    struct task *old=fixture_task(pid,"identity-fixture");
    struct sched_task_object *object=calloc(1,sizeof(*object));assert(object);
    object->task=*old;free(old);return &object->task;
}

int main(void)
{
    struct task *caller=fixture_object(801),*target=fixture_object(1),*blocker=fixture_object(UINT32_MAX);
    struct task *table[]={caller,target,blocker};install_tasks(table,3);current_pid[0]=801;
    caller->state=TASK_RUNNING;target->state=TASK_READY;target->parent_pid=801;
    target->limits.core=(struct linux_rlimit64){2,10};
    struct linux_rlimit64 input={5,10},old={0};
    struct trap_frame frame={.rax=LINUX_SYS_PRLIMIT64,.rdi=1,.rsi=LINUX_RLIMIT_CORE,
                             .rdx=(uintptr_t)&input,.r10=(uintptr_t)&old};
    blocked_output=(uintptr_t)&old;contended=true;execution_lock_depth=1;interrupts_enabled=false;
    task_lookup_scope_begin(caller,&frame);
    assert(process_resource_limit(frame.rax,frame.rdi,frame.rsi,frame.rdx,frame.r10)==KERNEL_SYSCALL_SWAP_CONTENDED);
    struct task_identity *identity=(struct task_identity *)caller->lookup_scope.paths[1].name;
    assert(identity && identity->task==target && identity->references==2);
    target->state=TASK_EXITED;target->flags=TASK_FLAG_WAITABLE_CHILD|TASK_FLAG_RESOURCES_RELEASED;
    target->running_cpu=SCHED_CPU_NONE;int status=-1;
    assert(sched_wait_reap(801,1,4,&status,NULL)==1);
    assert(!target->pid && !identity->task && identity->references==1);
    next_pid=UINT32_MAX;assert(task_allocate_pid()==1);
    assert(alloc_task_slot()==target);
    target->pid=target->tgid=1;target->kind=TASK_KIND_USER;target->state=TASK_READY;
    target->limits.core=(struct linux_rlimit64){3,10};
    contended=false;task_lookup_scope_begin(caller,&frame);
    assert(process_resource_limit(frame.rax,frame.rdi,frame.rsi,frame.rdx,frame.r10)==-LINUX_ESRCH);
    assert(target->limits.core.rlim_cur==3 && !old.rlim_cur);
    task_lookup_scope_cancel(caller);task_lookup_scope_cancel(caller);
    assert(!caller->lookup_scope.armed);
    execution_lock_depth=0;interrupts_enabled=true;
    free(caller);free(target);free(blocker);
    puts("PASS actual prlimit identity survives real reap, 32-bit PID wrap/reuse and scope cancellation");
    return 0;
}
