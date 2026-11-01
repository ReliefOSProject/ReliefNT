/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Real swap activation and deactivation preserve a partial pathname.
 * Storage and VFS callbacks use the existing activation fixture boundaries.
 */
#define TASK8_SWAP_REAL_PATH
#define main task8_inherited_swap_activation_main
#include "swap_activation_test.c"
#undef main
#include <reliefnt/syscall_internal.h>

static uint64_t blocked,unmapped;
static bool contended;
int swap_prepare_user_range(struct task *owner,uint64_t address,uint64_t length,bool writing)
{
    assert(owner==&task && !writing && length<=4096);
    if(address==unmapped)return -EFAULT;
    return address==blocked && contended ? SWAP_STORAGE_CONTENDED : 0;
}
bool user_range_ok(uint64_t address,uint64_t length)
{ return address>=4096 && length<=UINT64_MAX-address && address!=unmapped; }
void storage_drain_task_io(uint32_t pid){assert(pid==task.pid && !active_token);}
void task_process_vm_scope_release(void *state){(void)state;abort();}
void kernel_wait_queue_remove(struct kernel_wait_queue *q,struct task *t){(void)q;(void)t;abort();}
void vfs_walk_cancel(struct vfs_walk **w){assert(!*w);}
void storage_inode_allow_write(struct storage_inode_ref *i){(void)i;abort();}
int main(void)
{
    static char pages[8192] __attribute__((aligned(4096)));
    char *name=pages+4094;memcpy(name,"one",4);
    files[0].node=(struct storage_node){.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_EXT2,.first_cluster=1};
    files[0].physical=64;header(64);
    struct trap_frame f={.rax=LINUX_SYS_SWAPON,.rdi=(uintptr_t)name};
    blocked=(uintptr_t)pages+4096;contended=true;task_lookup_scope_begin(&task,&f);
    assert(syscall_mm_swapon((uintptr_t)name,0)==KERNEL_SYSCALL_SWAP_CONTENDED && !active_token);
    memcpy(name,"xx",2);unmapped=(uintptr_t)name;contended=false;task_lookup_scope_begin(&task,&f);
    assert(!syscall_mm_swapon((uintptr_t)name,0));task_lookup_scope_cancel(&task);
    memcpy(name,"one",4);unmapped=0;contended=true;f.rax=LINUX_SYS_SWAPOFF;
    task_lookup_scope_begin(&task,&f);
    assert(syscall_mm_swapoff((uintptr_t)name)==KERNEL_SYSCALL_SWAP_CONTENDED && !active_token);
    memcpy(name,"xx",2);unmapped=(uintptr_t)name;contended=false;task_lookup_scope_begin(&task,&f);
    assert(!syscall_mm_swapoff((uintptr_t)name));task_lookup_scope_cancel(&task);empty();
    puts("PASS swap on/off retains original cross-page path before storage ownership");return 0;
}
