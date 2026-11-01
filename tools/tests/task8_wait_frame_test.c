/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Actual wait dispatch through the real frame/scheduler retry gate. */
#define TASK8_WAIT_FRAME_TEST 1
#include "task8_wait_swap_test.c"

static unsigned release_calls,net_calls;
void net_tcp_maintenance(void) { assert(execution_lock_depth);++net_calls; }
uint32_t graphics_service_process_retirements(uint32_t n) { assert(execution_lock_depth && n==2);return 0; }
void driver_manager_process_audio_faults(void) { assert(execution_lock_depth); }
void usb_process_work(void) { assert(execution_lock_depth); }
void input_process_pending(void) { assert(execution_lock_depth); }
void task_socket_stage_enter(struct task *task,const struct trap_frame *frame) { assert(task && frame); }
void task_transfer_enter(struct task *task,const struct trap_frame *frame) { assert(task && frame); }
void task_vector_enter(struct task *task,const struct trap_frame *frame) { assert(task && frame); }
void task_lookup_scope_begin(struct task *task,const struct trap_frame *frame) { assert(task && frame); }
void storage_set_io_async_context(bool enabled) { assert(execution_lock_depth);(void)enabled; }
void storage_release_task_io(uint32_t pid) { assert(execution_lock_depth && pid==50);++release_calls; }
uint64_t time_ticks(void) { return 100; }
void arch_set_user_fs(uint64_t fs) { (void)fs;assert(execution_lock_depth); }
int64_t syscall_dispatch_regs(struct task *task,uint64_t nr,uint64_t a0,uint64_t a1,uint64_t a2,
                              uint64_t a3,uint64_t a4,uint64_t a5)
{ assert(task && execution_lock_depth);return syscall_legacy_process(nr,a0,a1,a2,a3,a4,a5); }
struct task *syscall_dispatch_frame(struct trap_frame *frame);

int main(void)
{
    struct task *parent=wait_fixture_task(50),*child=wait_fixture_task(51);
    struct task *table[]={parent,child};install_tasks(table,2);current_pid[0]=50;current_task[0]=parent;
    parent->state=TASK_RUNNING;parent->running_cpu=0;parent->as.cr3=0x1000;
    parent->stack_low=0x700000;parent->stack_top=0x710000;
    child->parent_pid=50;child->state=TASK_EXITED;child->exit_code=7;
    child->flags|=TASK_FLAG_RESOURCES_RELEASED;int status=-1;
    struct trap_frame frame={.rax=LINUX_SYS_WAIT4,.rdi=51,.rsi=(uintptr_t)&status,
                             .rip=0x400002,.rsp=0x708000,.cs=RELIEFNT_USER_CS,
                             .ss=RELIEFNT_USER_DS,.rflags=1ULL<<9};
    swapped=(uintptr_t)&status;contended=true;
    assert(!syscall_dispatch_frame(&frame));
    assert(!execution_lock_depth && parent->state==TASK_BLOCKED && parent->wake_tick==101);
    assert(frame.rax==LINUX_SYS_WAIT4 && frame.rip==0x400000 && child->pid==51 && status==-1);
    contended=false;sched_wake_interruptible(parent);assert(parent->state==TASK_READY);
    parent->state=TASK_RUNNING;sched_resched_pending[0]=0;frame.rip+=2;
    assert(syscall_dispatch_frame(&frame)==parent);
    assert(!execution_lock_depth && frame.rax==51 && frame.rip==0x400002);
    assert(!child->pid && status==(7<<8) && release_calls==1 && net_calls==2);
    free(parent);free(child);
    puts("PASS wait frame parks on prerequisite, wakes and consumes original event once");return 0;
}
