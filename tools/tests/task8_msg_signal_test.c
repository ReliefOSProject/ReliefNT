/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: msg signal.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define TASK8_REAL_LOOKUP_SCOPE 1
#define TASK8_REAL_SYSV_SIGNAL 1
#define main inherited_signal_main
#include "signal_address_space_test.c"
#undef main
#include <linux/msg.h>

/** @brief Invoke a message syscall with the real retry scope and task state. */
static int64_t scoped_msg(struct task *task,uint64_t nr,uint64_t a0,uint64_t a1,
                          uint64_t a2,uint64_t a3,uint64_t a4)
{
    struct trap_frame f={.rax=nr,.rdi=a0,.rsi=a1,.rdx=a2,.r10=a3,.r8=a4};
    signal_running=task;task_lookup_scope_begin(task,&f);
    return syscall_sysv_msg(nr,a0,a1,a2,a3,a4);
}

int main(void)
{
    unsigned char *user=mmap((void *)STACK_ADDRESS,sizeof(target_pages),PROT_READ|PROT_WRITE,
                             MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(user==(void *)STACK_ADDRESS);
    target=(struct task){.kind=TASK_KIND_USER,.pid=1,.tgid=1,.state=TASK_READY,
        .euid=1000,.egid=100,.fsgid=100};
    signal_sender=(struct task){.kind=TASK_KIND_USER,.pid=2,.tgid=2,.state=TASK_READY,
        .euid=1000,.egid=100,.fsgid=100};
    target.limits.sigpending.rlim_cur=8;
    target.signal_actions[2]=(struct kernel_signal_action){.handler=0x410000};
    struct {int64_t type;char data[8];} *input=(void *)user;
    input->type=7;memcpy(input->data,"message",8);
    uint64_t dest=STACK_ADDRESS+512;
    signal_running=&signal_sender;
    int q=(int)syscall_sysv_msg(__NR_msgget,0,0600,0,0,0);assert(q>=0);
    assert(scoped_msg(&target,__NR_msgrcv,q,dest,8,0,0)==KERNEL_SYSCALL_BLOCKED);
    target.restart_syscall=__NR_msgrcv+1;
    assert(!scoped_msg(&signal_sender,__NR_msgsnd,q,STACK_ADDRESS,8,0,0));
    task_lookup_scope_cancel(&signal_sender);
    assert(target.sysv_msg.completed && target.sysv_msg.message);
    signal_prep_error=SWAP_STORAGE_CONTENDED;
    assert(scoped_msg(&target,__NR_msgrcv,q,dest,8,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    struct trap_frame frame={.rip=0x430000,.rsp=STACK_ADDRESS+7000,.cs=0x23};
    assert(!kernel_signal_queue_task(&target,2));
    assert(!kernel_signal_enqueue(&target,false,19,NULL));
    assert(!kernel_signal_deliver_pending_outer(&target,&frame));
    assert(target.state==TASK_STOPPED && (target.pending_signals&(1ULL<<1)));
    assert(target.sysv_msg.completed && target.sysv_msg.message && frame.rip==0x430000);
    assert(!kernel_signal_queue_task(&target,18) && target.state==TASK_READY);
    signal_running=&signal_sender;
    assert(!syscall_sysv_msg(__NR_msgctl,q,LINUX_IPC_RMID,0,0,0));
    signal_prep_error=0;
    assert(scoped_msg(&target,__NR_msgrcv,q,dest,8,0,0)==8);
    assert(!target.sysv_msg.queue && (target.pending_signals&(1ULL<<1)));
    int64_t type;memcpy(&type,target_pages+512,8);
    assert(type==7 && !memcmp(target_pages+520,"message",8));
    task_lookup_scope_cancel(&target);kernel_signal_flush(&target,false,UINT64_MAX);
    signal_running=&signal_sender;q=(int)syscall_sysv_msg(__NR_msgget,0,0600,0,0,0);assert(q>=0);
    assert(scoped_msg(&target,__NR_msgrcv,q,dest,8,0,0)==KERNEL_SYSCALL_BLOCKED);
    target.restart_syscall=__NR_msgrcv+1;
    assert(!scoped_msg(&signal_sender,__NR_msgsnd,q,STACK_ADDRESS,8,0,0));
    task_lookup_scope_cancel(&signal_sender);
    signal_prep_error=SWAP_STORAGE_CONTENDED;
    assert(scoped_msg(&target,__NR_msgrcv,q,dest,8,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!kernel_signal_enqueue(&target,false,11,NULL));
    assert(!kernel_signal_deliver_pending_outer(&target,&frame));
    assert(target.state==TASK_EXITED && exits==1 && !target.sysv_msg.queue);
    task_lookup_scope_cancel(&target);
    signal_running=&signal_sender;signal_prep_error=0;
    assert(!syscall_sysv_msg(__NR_msgctl,q,LINUX_IPC_RMID,0,0,0));
    assert(!signal_allocations);
    munmap(user,sizeof(target_pages));
    puts("PASS real SysV+signal: caught/stop/continue, RMID, fatal and owned cleanup");
    return 0;
}
