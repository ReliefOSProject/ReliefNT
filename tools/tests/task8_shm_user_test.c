/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: shm user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/syscall/ipc/sysv_shm.c"
#include <reliefnt/swap.h>
#include <reliefnt/syscall_internal.h>
static struct task current;
static bool contended;
static unsigned preparations;
bool user_range_ok(uint64_t p,uint64_t n){return p>=4096 && n<=UINT64_MAX-p;}
int user_copy_to_task(struct task *task,uint64_t p,const void *src,uint64_t n)
{assert(task==&current);if(!user_range_ok(p,n))return -LINUX_EFAULT;memcpy((void *)(uintptr_t)p,src,n);return 0;}
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t size,bool writing)
{assert(task==&current && address>=4096 && size<=32768);(void)writing;++preparations;
 return contended ? SWAP_STORAGE_CONTENDED : 0;}
int time_clock_get(int32_t id,struct linux_timespec *out)
{assert(id==LINUX_CLOCK_REALTIME);*out=(struct linux_timespec){.tv_sec=42};return 0;}
int main(void)
{
    current.kind=TASK_KIND_USER;current.euid=100;
    struct sysv_shm_segment segment={.id=1,.slot=1,.stat.shm_perm={.uid=100,.cuid=100,.mode=0600}};
    sysv_shm_ids[1]=&segment;sysv_shm_used=1;
    struct shminfo64 info={0};contended=true;
    assert(sysv_shm_control(&current,1,IPC_INFO,(uintptr_t)&info)==KERNEL_SYSCALL_SWAP_CONTENDED);
    contended=false;assert(sysv_shm_control(&current,1,IPC_INFO,(uintptr_t)&info)==1);
    assert(info.shmmni==SYSV_SHM_SLOTS);
    struct shmid64_ds input={0};input.shm_perm.uid=101;input.shm_perm.gid=20;input.shm_perm.mode=0640;
    contended=true;assert(sysv_shm_control(&current,1,IPC_SET,(uintptr_t)&input)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(segment.stat.shm_perm.uid==100);
    contended=false;assert(!sysv_shm_control(&current,1,IPC_SET,(uintptr_t)&input));
    assert(segment.stat.shm_perm.uid==101 && segment.stat.shm_ctime==42);
    unsigned before=preparations;
    assert(sysv_shm_control(&current,1,-1,1)==-LINUX_EINVAL && preparations==before);
    puts("PASS shmctl: bounded swapped input/output, no early mutation, ignored invalid command");
    return 0;
}
