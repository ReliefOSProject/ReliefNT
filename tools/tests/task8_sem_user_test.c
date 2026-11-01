/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: sem user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_sysv_sem_main
#include "sysv_sem_test.c"
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
static int64_t scoped(uint64_t nr,uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3)
{struct trap_frame frame={.rax=nr,.rdi=a0,.rsi=a1,.rdx=a2,.r10=a3};
 task_lookup_scope_begin(current,&frame);return syscall_sysv_sem(nr,a0,a1,a2,a3);}
int main(void)
{
    current=&tasks[0];*current=(struct task){.pid=101,.tgid=101,.euid=1000,
        .egid=100,.fsgid=100,.kind=TASK_KIND_USER,.state=TASK_READY};
    int id=get(0,17000,0600);assert(id>=0);
    struct linux_sembuf operation={0,1,0};struct linux_timespec timeout={1,0};
    blocked=(uintptr_t)&operation;contended=true;
    assert(scoped(__NR_semtimedop,id,(uintptr_t)&operation,1,(uintptr_t)&timeout)==
           KERNEL_SYSCALL_SWAP_CONTENDED);
    timeout.tv_sec=-1;contended=false;
    assert(!scoped(__NR_semtimedop,id,(uintptr_t)&operation,1,(uintptr_t)&timeout));
    task_lookup_scope_cancel(current);assert(ctl(id,0,LINUX_GETVAL,0)==1);
    uint16_t *values=malloc(17000*sizeof(*values)),*output=malloc(17000*sizeof(*output));
    assert(values && output);for(unsigned i=0;i<17000;++i)values[i]=(uint16_t)(i%100);
    blocked=(uintptr_t)values+32768;contended=true;
    assert(scoped(__NR_semctl,id,0,LINUX_SETALL,(uintptr_t)values)==KERNEL_SYSCALL_SWAP_CONTENDED);
    values[0]=77;contended=false;
    assert(!scoped(__NR_semctl,id,0,LINUX_SETALL,(uintptr_t)values));task_lookup_scope_cancel(current);
    memset(output,0x5a,17000*sizeof(*output));blocked=(uintptr_t)output+32768;contended=true;
    assert(scoped(__NR_semctl,id,0,LINUX_GETALL,(uintptr_t)output)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(output[0]==0x5a5a && output[16999]==0x5a5a);
    contended=false;assert(!scoped(__NR_semctl,id,0,LINUX_GETALL,(uintptr_t)output));
    assert(output[0]==0 && output[16999]==99);task_lookup_scope_cancel(current);
    /* The complete numeric aperture is checked before any sliced page-in. */
    uint64_t across=RELIEFNT_USER_TOP-32768;unsigned before=preparations;
    blocked=across;contended=true;fault_at=across;
    assert(scoped(__NR_semctl,id,0,LINUX_GETALL,across)==-LINUX_EFAULT);
    assert(preparations==before);task_lookup_scope_cancel(current);fault_at=0;contended=false;
    assert(!ctl(id,0,LINUX_IPC_RMID,0));free(values);free(output);assert(!allocations && preparations);
    puts("PASS SysV sem: retained timeout and SETALL input; bounded GETALL prerequisite");
    return 0;
}
