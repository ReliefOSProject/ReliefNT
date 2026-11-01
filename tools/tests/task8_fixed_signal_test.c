/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Real signal delivery cancels fixed-input scopes at descriptor cleanup.
 * User pages and scheduler state are host models; signal, scope, setters and
 * task_release_syscall_file are production code.
 */
#define TASK8_REAL_LOOKUP_SCOPE 1
#define TASK8_REAL_DESCRIPTOR_CLEANUP 1
#define TASK8_PROCESS_SIGNAL_TEST 1
#define main inherited_fixed_signal_main
#include "signal_address_space_test.c"
#undef main
#include "../../kernel/syscall/process/process.c"
#include "../../kernel/syscall/event/timerfd.c"

void task_regular_io_stage_cancel(struct task *task) { assert(task==&target); }
void task_socket_stage_cancel(struct task *task) { assert(task==&target); }
void graphics_service_cancel_task_continuation(struct task *task) { assert(task==&target); }
void task_sysv_msg_cancel(struct task *task) { assert(task==&target); }
void task_sysv_sem_cancel(struct task *task) { assert(task==&target); }
void task_pty_release_entry(struct task_pty_fd *entry) { assert(entry && !entry->used); }
bool task_sysv_sem_ready(const struct task *task) { assert(task==&target);return false; }
void syscall_record_lock_cancel(struct task *task) { assert(task==&target); }

int main(int argc,char **argv)
{
    assert(argc==2);
    unsigned char *user=mmap((void *)STACK_ADDRESS,sizeof(target_pages),PROT_READ|PROT_WRITE,
                             MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(user==(void *)STACK_ADDRESS);
    target=(struct task){.kind=TASK_KIND_USER,.pid=1,.state=TASK_READY};
    target.limits.sigpending.rlim_cur=8;target.limits.core=(struct linux_rlimit64){2,10};
    target.signal_actions[2]=(struct kernel_signal_action){.handler=0x410000,
        .restorer=0x420000,.flags=LINUX_SA_SIGINFO};
    struct task_timerfd timer={.expiry_tick=150};
    struct task_file timerfile={.used=1,.references=2,.flags=TASK_FILE_FLAG_TIMERFD,.aux=(uintptr_t)&timer};
    uint64_t input=(uintptr_t)user+64,old=(uintptr_t)user+128;
    struct trap_frame call={0};
    if(!strcmp(argv[1],"mask")) {
        uint64_t value=4;memcpy((void *)(uintptr_t)input,&value,sizeof(value));
        call=(struct trap_frame){.rax=LINUX_SYS_RT_SIGPROCMASK,.rdi=0,.rsi=input,.rdx=old,.r10=8};
    } else if(!strcmp(argv[1],"limit")) {
        struct linux_rlimit64 value={5,10};memcpy((void *)(uintptr_t)input,&value,sizeof(value));
        call=(struct trap_frame){.rax=LINUX_SYS_PRLIMIT64,.rdi=0,.rsi=LINUX_RLIMIT_CORE,.rdx=input,.r10=old};
    } else {
        assert(!strcmp(argv[1],"timer"));
        struct linux_itimerspec value={.it_value={.tv_sec=2}};memcpy((void *)(uintptr_t)input,&value,sizeof(value));
        call=(struct trap_frame){.rax=LINUX_SYS_TIMERFD_SETTIME,.rdi=3,.rdx=input,.r10=old};
        target.syscall_file=&timerfile;target.syscall_fd=3;target.syscall_file_number=call.rax;
    }
    task_lookup_scope_begin(&target,&call);signal_inet_blocked=old;
    int64_t rc=call.rax==LINUX_SYS_RT_SIGPROCMASK ? syscall_linux_signal(call.rax,0,input,old,8,0) :
        call.rax==LINUX_SYS_PRLIMIT64 ? process_resource_limit(call.rax,0,LINUX_RLIMIT_CORE,input,old) :
        syscall_timerfd_settime(3,0,input,old);
    assert(rc==KERNEL_SYSCALL_SWAP_CONTENDED && target.lookup_scope.armed && signal_allocations);
    assert(!target.blocked_signals && target.limits.core.rlim_cur==2 && timer.expiry_tick==150);
    target.restart_syscall=call.rax+1;signal_inet_blocked=0;
    assert(!kernel_signal_queue_task(&target,2));
    struct trap_frame frame={.rsp=STACK_ADDRESS+4896,.rip=0x430000,.cs=0x23};
    assert(kernel_signal_deliver_pending_outer(&target,&frame)==1);
    assert(!target.lookup_scope.armed && !target.syscall_file && !signal_allocations);
    assert(!target.blocked_signals || target.blocked_signals==(1ULL<<1));
    assert(target.limits.core.rlim_cur==2 && timer.expiry_tick==150 && timerfile.references==1+(call.rax!=LINUX_SYS_TIMERFD_SETTIME));
    task_release_syscall_file(&target);assert(!signal_allocations);
    munmap(user,sizeof(target_pages));
    puts("PASS actual caught signal and descriptor cleanup retire fixed input once without committing");return 0;
}
