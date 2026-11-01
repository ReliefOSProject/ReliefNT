/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: process user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/syscall/process/process.c"
#include <reliefnt/swap.h>
#include <reliefnt/syscall_internal.h>
static struct task current;
static struct task *external_task;
static uint64_t swapped;
static uint64_t unmapped;
static bool contended;
static unsigned prepares, action_changes;
struct task *sched_current_task(void) { return &current; }
struct task *sched_find(uint32_t pid)
{ return pid==current.pid ? &current : external_task && external_task->pid==pid ? external_task : NULL; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    (void)writing; assert(task==&current && length<=128);++prepares;
    if(address<4096 || address==unmapped)return -LINUX_EFAULT;
    if(address==swapped){if(contended)return SWAP_STORAGE_CONTENDED;swapped=0;}
    return 0;
}
bool user_range_ok(uint64_t address,uint64_t length)
{ return address>=4096 && length<=UINT64_MAX-address && address!=swapped && address!=unmapped; }
bool user_range_writable(uint64_t address,uint64_t length) { return user_range_ok(address,length); }
void sched_block_current(void) { assert(0); }
int kernel_signal_set_action(struct task *task,int number,uint64_t handler,uint64_t mask,
                             uint32_t flags,uint64_t restorer,struct kernel_signal_action *old)
{
    assert(task==&current && !old);++action_changes;
    task->signal_actions[number]=(struct kernel_signal_action){.handler=handler,.mask=mask,
        .flags=flags,.restorer=restorer};return 0;
}
int main(void)
{
    current.kind=TASK_KIND_USER;current.pid=42;
    struct reliefos_linux_sigaction action={.handler=0x10000,.restorer=0x20000},old={0};
    swapped=(uintptr_t)&action;contended=true;
    assert(syscall_linux_signal(LINUX_SYS_RT_SIGACTION,2,(uintptr_t)&action,(uintptr_t)&old,8,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!action_changes && !current.signal_actions[2].handler);
    contended=false;swapped=(uintptr_t)&old;contended=true;
    assert(syscall_linux_signal(LINUX_SYS_RT_SIGACTION,2,(uintptr_t)&action,(uintptr_t)&old,8,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!action_changes);
    contended=false;assert(!syscall_linux_signal(LINUX_SYS_RT_SIGACTION,2,(uintptr_t)&action,(uintptr_t)&old,8,0));
    assert(action_changes==1 && current.signal_actions[2].handler==action.handler);
    uint64_t mask=4,previous=0;swapped=(uintptr_t)&previous;contended=true;
    assert(syscall_linux_signal(LINUX_SYS_RT_SIGPROCMASK,0,(uintptr_t)&mask,(uintptr_t)&previous,8,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!current.blocked_signals && !previous);
    contended=false;assert(!syscall_linux_signal(LINUX_SYS_RT_SIGPROCMASK,0,(uintptr_t)&mask,(uintptr_t)&previous,8,0));
    assert(current.blocked_signals==4 && !previous);
    struct linux_sigaltstack stack={.sp=0x400000,.size=4096},oldstack={0};
    swapped=(uintptr_t)&stack;contended=true;
    assert(syscall_linux_signal(LINUX_SYS_SIGALTSTACK,(uintptr_t)&stack,(uintptr_t)&oldstack,0,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!current.signal_stack_size);
    contended=false;assert(!syscall_linux_signal(LINUX_SYS_SIGALTSTACK,(uintptr_t)&stack,(uintptr_t)&oldstack,0,0,0));
    assert(current.signal_stack_size==4096);
    struct linux_rlimit64 limit={5,10},limit_old={0};
    current.limits.core=(struct linux_rlimit64){2,10};
    swapped=(uintptr_t)&limit_old;contended=true;
    assert(process_resource_limit(LINUX_SYS_PRLIMIT64,0,LINUX_RLIMIT_CORE,(uintptr_t)&limit,(uintptr_t)&limit_old)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(current.limits.core.rlim_cur==2);
    contended=false;assert(!process_resource_limit(LINUX_SYS_PRLIMIT64,0,LINUX_RLIMIT_CORE,(uintptr_t)&limit,(uintptr_t)&limit_old));
    assert(limit_old.rlim_cur==2 && current.limits.core.rlim_cur==5);
    limit.rlim_cur=6;
    assert(process_resource_limit(LINUX_SYS_PRLIMIT64,0,LINUX_RLIMIT_CORE,(uintptr_t)&limit,1)==-LINUX_EFAULT);
    assert(current.limits.core.rlim_cur==6 && prepares);
    puts("PASS signal and prlimit: bounded swap preparation, no repeated mutation");
    return 0;
}
