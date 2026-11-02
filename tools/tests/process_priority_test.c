/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../../kernel/syscall/process/process.c"

static struct task caller, target;
static unsigned updates;

struct task *sched_current_task(void) { return &caller; }
struct task *sched_find(uint32_t pid)
{
    return pid == caller.pid ? &caller : pid == target.pid ? &target : NULL;
}
uint32_t sched_current_pid(void) { return caller.pid; }
int sched_task_priority(uint32_t pid, int priority, int set)
{
    struct task *task = sched_find(pid);
    assert(task && task->kind == TASK_KIND_USER && task->state != TASK_EXITED);
    if (set) { ++updates; task->priority = priority; }
    return task->priority;
}

int main(void)
{
    caller = (struct task){.pid=41, .kind=TASK_KIND_USER, .state=TASK_READY,
        .uid=1000, .euid=1000, .priority=-10};
    target = (struct task){.pid=42, .kind=TASK_KIND_USER, .state=TASK_READY,
        .uid=1000, .euid=1000, .priority=-10};
    /* The renice child may lower the priority of its same-owner parent. */
    assert(syscall_process_control(LINUX_SYS_SETPRIORITY,0,42,0,0)==0);
    assert(target.priority==0 && updates==1);
    assert(syscall_process_control(LINUX_SYS_GETPRIORITY,0,42,0,0)==20);
    assert(syscall_process_control(LINUX_SYS_SETPRIORITY,0,42,-1,0)==-LINUX_EACCES);
    assert(target.priority==0 && updates==1);
    assert(syscall_process_control(LINUX_SYS_SETPRIORITY,0,0,0,0)==0);
    assert(caller.priority==0);
    assert(syscall_process_control(LINUX_SYS_SETPRIORITY,0,0,-1,0)==-LINUX_EACCES);
    target.uid=target.euid=2000;
    assert(syscall_process_control(LINUX_SYS_SETPRIORITY,0,42,10,0)==-LINUX_EPERM);
    assert(syscall_process_control(LINUX_SYS_GETPRIORITY,0,42,0,0)==20);
    /* Modern Linux accepts a match with either target real/effective uid. */
    target.euid=1000;
    assert(syscall_process_control(LINUX_SYS_SETPRIORITY,0,42,1,0)==0);
    target.euid=2000;target.uid=1000;
    assert(syscall_process_control(LINUX_SYS_SETPRIORITY,0,42,2,0)==0);
    caller.uid=1000;caller.euid=3000;
    assert(syscall_process_control(LINUX_SYS_SETPRIORITY,0,42,3,0)==-LINUX_EPERM);
    caller.uid=caller.euid=0;
    assert(syscall_process_control(LINUX_SYS_SETPRIORITY,0,42,3,0)==-LINUX_EPERM);
    _Static_assert(CAP_SYS_NICE == 23, "Linux capability number");
    caller.cap_effective=1ULL<<CAP_SYS_NICE;
    assert(syscall_process_control(LINUX_SYS_SETPRIORITY,0,42,-99,0)==0);
    assert(target.priority==-20);
    assert(syscall_process_control(LINUX_SYS_GETPRIORITY,0,42,0,0)==40);
    assert(syscall_process_control(LINUX_SYS_SETPRIORITY,0,42,99,0)==0);
    assert(target.priority==19);
    target.priority=-1;
    assert(syscall_process_control(LINUX_SYS_GETPRIORITY,0,42,0,0)==21);
    assert(syscall_process_control(LINUX_SYS_GETPRIORITY,0,99,0,0)==-LINUX_ESRCH);
    target.state=TASK_EXITED;
    assert(syscall_process_control(LINUX_SYS_SETPRIORITY,0,42,0,0)==-LINUX_ESRCH);
    puts("PASS PRIO_PROCESS: same-owner lowering, capability/raise checks, clamp, Linux encoding and ESRCH");
    return 0;
}
