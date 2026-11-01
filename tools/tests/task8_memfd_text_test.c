/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief The actual memfd name front door preserves a partial string on retry.
 * The allocator, fd and page availability are host fixture boundaries.
 */
#define TASK8_REAL_PATH
#define main task8_inherited_timerfd_user_main
#include "task8_timerfd_user_test.c"
#undef main
#include <stdlib.h>
static unsigned heap_allocs,heap_frees;
void *kernel_malloc(size_t size){void *p=malloc(size);if(p)++heap_allocs;return p;}
void kernel_free(void *p){if(p){++heap_frees;free(p);}}
void storage_drain_task_io(uint32_t pid){assert(pid==current.pid);}
void task_process_vm_scope_release(void *state){(void)state;abort();}
void kernel_wait_queue_remove(struct kernel_wait_queue *queue,struct task *task)
{ (void)queue;(void)task;abort(); }
void vfs_walk_cancel(struct vfs_walk **walk){assert(!*walk);}
void vfs_path_put(struct vfs_path *path){assert(!path->mount);}
void storage_inode_allow_write(struct storage_inode_ref *inode){(void)inode;abort();}
int storage_inode_put(struct storage_inode_ref *inode){(void)inode;abort();}
int main(void)
{
    static char pages[8192] __attribute__((aligned(4096)));
    char *name=pages+4092;memcpy(name,"original",9);
    current.pid=42;current.kind=TASK_KIND_USER;
    struct trap_frame f={.rax=LINUX_SYS_MEMFD_CREATE,.rdi=(uintptr_t)name,.rsi=0};
    swapped=(uintptr_t)pages+4096;contended=true;task_lookup_scope_begin(&current,&f);
    assert(syscall_memfd_create((uintptr_t)name,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    memcpy(name,"xxxx",4);unmapped=(uintptr_t)name;contended=false;task_lookup_scope_begin(&current,&f);
    assert(syscall_memfd_create((uintptr_t)name,0)==3);
    assert(!strcmp(saved_name,"original"));task_lookup_scope_cancel(&current);
    assert(heap_allocs==heap_frees);
    puts("PASS memfd retains original cross-page name across contention");return 0;
}
