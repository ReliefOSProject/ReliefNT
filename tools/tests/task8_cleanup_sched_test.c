/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: cleanup sched.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
/* Compose the real scheduler with a controlled, bounded robust-cleanup step. */
#define main inherited_exit_logging_main
#include "task_exit_logging_test.c"
#undef main
#include <reliefnt/swap.h>

static int cleanup_result;
static unsigned cleanup_calls;
bool kernel_execution_is_outermost(void) { return execution_lock_depth == 1; }
int futex_task_exit_step(struct task *task)
{ assert(task && !scheduler_lock_depth && kernel_execution_is_outermost()); ++cleanup_calls; return cleanup_result; }
int syscall_unshare_task_files(struct task *task) { assert(task);return 0; }
void kernel_signal_detach_process(struct task *task) { assert(task); }

/** @brief Give inherited fixture tasks the scheduler's actual private tail. */
static struct task *fixture_sized_task(uint32_t pid,const char *name)
{
    struct task *old=fixture_task(pid,name);
    struct sched_task_object *object=calloc(1,sizeof(*object));assert(object);
    object->task=*old;free(old);return &object->task;
}

int main(void)
{
    struct task *waiter=fixture_sized_task(50,"waiter"), *child=fixture_sized_task(51,"child");
    child->state=TASK_EXITED; child->parent_pid=50; child->exit_code=7;
    struct task *table[]={waiter,child}; install_tasks(table,2);
    execution_lock_depth=1; interrupts_enabled=false;
    cleanup_result=-4100;
    int status=-1; struct linux_siginfo info={0};
    assert(sched_wait_reap(50,51,4|0x01000000u,&status,&info)==-RELIEFOS_EAGAIN);
    assert(child->pid==51 && !(child->flags&TASK_FLAG_RESOURCES_RELEASED));
    assert(!(child->flags&TASK_FLAG_REAP_IN_PROGRESS) && status==-1);
    assert(sched_wait_reap(50,51,4,&status,&info)==-RELIEFOS_EAGAIN);
    assert(child->pid==51 && !(child->flags&TASK_FLAG_REAP_IN_PROGRESS));
    cleanup_result=1;
    sched_cleanup_poll(2); /* Models an idle outer gate, including PID zero. */
    assert(child->flags&TASK_FLAG_RESOURCES_RELEASED);
    assert(sched_wait_reap(50,51,4|0x01000000u,&status,&info)==51);
    assert(child->pid==51 && status==(7<<8));
    assert(sched_wait_reap(50,51,4,&status,&info)==51 && !child->pid);
    assert(cleanup_calls==3);
    struct task *solo[]={waiter};install_tasks(solo,1);
    cleanup_result=-EINVAL;
    assert(sched_prepare_exec_current(waiter)==-EINVAL);
    cleanup_result=SWAP_STORAGE_CONTENDED;
    assert(sched_prepare_exec_current(waiter)==-EAGAIN);
    execution_lock_depth=0; interrupts_enabled=true;
    free(waiter);free(child);

    struct sched_task_object *object=calloc(1,sizeof(*object));assert(object);
    object->task.kind=TASK_KIND_USER;object->task.state=TASK_EXITED;
    object->task.running_cpu=SCHED_CPU_NONE;object->task.flags=TASK_FLAG_RESOURCES_RELEASED;
    object->futex_exit.phase=9;object->futex_exit.entry=123;
    object->socket_stage=(struct socket_send_stage *)(uintptr_t)1;
    struct task *slots[]={&object->task};install_tasks(slots,1);
    assert(alloc_task_slot()==&object->task);
    assert(!object->futex_exit.phase && !object->futex_exit.entry && !object->socket_stage);
    free(object);
    puts("PASS deferred exit: contention retains PID/MM, WNOWAIT/reap gate, idle progress, slot cursor reset");
}
