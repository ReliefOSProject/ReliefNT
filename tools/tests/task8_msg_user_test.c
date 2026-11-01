/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: msg user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_sysv_msg_main
#include "sysv_msg_test.c"
#undef main
#include <reliefnt/swap.h>
static uint64_t blocked;
static bool contended;
static unsigned preparations;
int swap_prepare_user_range(struct task *task,uint64_t p,uint64_t n,bool writing)
{assert(task==current && n<=32768);(void)writing;++preparations;
 return contended && p==blocked ? SWAP_STORAGE_CONTENDED : 0;}
void storage_drain_task_io(uint32_t pid){assert(pid==current->pid);}
void kernel_wait_queue_remove(struct kernel_wait_queue *q,struct task *task){(void)q;(void)task;assert(0);}
void vfs_walk_cancel(struct vfs_walk **walk){assert(!*walk);}
void vfs_path_put(struct vfs_path *path){assert(!path->mount);}
void storage_inode_allow_write(struct storage_inode_ref *inode){(void)inode;assert(0);}
int storage_inode_put(struct storage_inode_ref *inode){(void)inode;assert(0);return 0;}
void task_process_vm_scope_release(void *state){(void)state;assert(0);}
static int64_t send_scoped(int q,void *msg,uint64_t size,uint32_t flags)
{struct trap_frame frame={.rax=LINUX_SYS_MSGSND,.rdi=q,.rsi=(uintptr_t)msg,.rdx=size,.r10=flags};
 task_lookup_scope_begin(current,&frame);return send_msg(q,msg,size,flags);}
static int64_t recv_scoped(int q,void *msg,uint64_t size,uint32_t flags)
{struct trap_frame frame={.rax=LINUX_SYS_MSGRCV,.rdi=q,.rsi=(uintptr_t)msg,.rdx=size,.r8=flags};
 task_lookup_scope_begin(current,&frame);return receive_msg(q,msg,size,0,flags);}
int main(void)
{
    for(unsigned i=0;i<2;++i){tasks[i].pid=101+i;tasks[i].tgid=101+i;
        tasks[i].euid=1000;tasks[i].egid=tasks[i].fsgid=100;
        tasks[i].kind=TASK_KIND_USER;tasks[i].state=TASK_READY;}
    current=&tasks[0];int q=msgget(0,0600);assert(q>=0);
    struct {int64_t type;char data[16];} input={7,"original"},output={0};
    blocked=(uintptr_t)input.data;contended=true;
    assert(send_scoped(q,&input,8,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    input.type=9;contended=false;assert(!send_scoped(q,&input,8,0));
    task_lookup_scope_cancel(current);
    blocked=(uintptr_t)&output;contended=true;
    assert(recv_scoped(q,&output,8,LINUX_IPC_NOWAIT)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(msg_find(q,true)->stat.msg_qnum==1);
    contended=false;assert(recv_scoped(q,&output,8,LINUX_IPC_NOWAIT)==8);
    assert(output.type==7 && !memcmp(output.data,"original",8));task_lookup_scope_cancel(current);
    current=&tasks[1];memset(&output,0,sizeof(output));
    assert(recv_scoped(q,&output,8,0)==KERNEL_SYSCALL_BLOCKED);
    current=&tasks[0];input.type=5;memcpy(input.data,"pipeline",8);
    assert(!send_scoped(q,&input,8,0));task_lookup_scope_cancel(current);
    current=&tasks[1];assert(current->sysv_msg.completed && current->sysv_msg.message);
    blocked=(uintptr_t)&output;contended=true;
    assert(recv_scoped(q,&output,8,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(current->sysv_msg.completed && current->sysv_msg.message);
    current=&tasks[0];assert(!control(q,LINUX_IPC_RMID,NULL));
    assert(tasks[1].sysv_msg.completed && tasks[1].sysv_msg.message);
    current=&tasks[1];
    contended=false;assert(recv_scoped(q,&output,8,0)==8);
    assert(output.type==5 && !memcmp(output.data,"pipeline",8));task_lookup_scope_cancel(current);
    current=&tasks[0];q=msgget(0,0600);assert(q>=0);
    current=&tasks[1];assert(recv_scoped(q,&output,8,0)==KERNEL_SYSCALL_BLOCKED);
    current=&tasks[0];assert(!send_scoped(q,&input,8,0));task_lookup_scope_cancel(current);
    current=&tasks[1];contended=true;
    assert(recv_scoped(q,&output,8,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    task_sysv_msg_cancel(current);task_sysv_msg_cancel(current);task_lookup_scope_cancel(current);
    current=&tasks[0];assert(!control(q,LINUX_IPC_RMID,NULL));
    /* Boundary stress: an owned, unlinked waiter finds a candidate, waits for
     * prerequisite page-in, and the queue is removed before its retry. */
    q=msgget(0,0600);assert(q>=0);
    current=&tasks[1];contended=false;
    assert(recv_scoped(q,&output,8,0)==KERNEL_SYSCALL_BLOCKED);
    struct sysv_msg_queue *queue=current->sysv_msg.queue;
    msg_unlink_wait(&current->sysv_msg);
    struct sysv_message *candidate=kernel_malloc(sizeof(*candidate)+8);assert(candidate);
    *candidate=(struct sysv_message){.type=5,.size=8};memcpy(candidate->data,"boundary",8);
    queue->first=queue->last=candidate;queue->stat.msg_qnum=1;queue->stat.msg_cbytes=8;
    ++msg_total_headers;msg_total_bytes+=8;
    contended=true;assert(recv_scoped(q,&output,8,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    current=&tasks[0];assert(!control(q,LINUX_IPC_RMID,NULL));
    current=&tasks[1];contended=false;
    assert(recv_scoped(q,&output,8,0)==-LINUX_EIDRM);
    task_lookup_scope_cancel(current);assert(!allocations && preparations);
    puts("PASS SysV msg: retained input, queued and pipeline waits, RMID and exact cancellation");
    return 0;
}
