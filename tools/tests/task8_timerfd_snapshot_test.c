/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Preserve the admitted timer value across output page-in contention.
 * Timer and lookup code are real; user mapping availability is controlled.
 */
#define main task8_inherited_timerfd_user_main
#include "task8_timerfd_user_test.c"
#undef main
#include <stdlib.h>
static unsigned frees;
void *kernel_malloc(size_t size){void *p=malloc(size);if(p)++allocations;return p;}
void kernel_free(void *p){if(p){++frees;free(p);}}
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
    current.pid=42;current.kind=TASK_KIND_USER;
    timer=(struct task_timerfd){.expiry_tick=150};file.flags=TASK_FILE_FLAG_TIMERFD;file.aux=(uintptr_t)&timer;
    struct linux_itimerspec input={.it_value={.tv_sec=2}},old={0};
    struct trap_frame f={.rax=LINUX_SYS_TIMERFD_SETTIME,.rdi=3,.rdx=(uintptr_t)&input,.r10=(uintptr_t)&old};
    swapped=(uintptr_t)&old;contended=true;task_lookup_scope_begin(&current,&f);
    assert(syscall_timerfd_settime(3,0,f.rdx,f.r10)==KERNEL_SYSCALL_SWAP_CONTENDED);
    input.it_value.tv_sec=7;unmapped=(uintptr_t)&input;contended=false;task_lookup_scope_begin(&current,&f);
    assert(!syscall_timerfd_settime(3,0,f.rdx,f.r10));
    assert(timer.expiry_tick==300);
    task_lookup_scope_cancel(&current);assert(allocations==frees);
    /* The old timer overlaps the admitted input but extends into a second
     * page. Output contention must retain the original input and no update. */
    static uint8_t pages[8192] __attribute__((aligned(4096)));
    uint64_t in=(uintptr_t)pages+4096-sizeof(input);
    uint64_t out=(uintptr_t)pages+4096-sizeof(input)/2;
    input=(struct linux_itimerspec){.it_value={.tv_sec=3}};
    memcpy((void *)(uintptr_t)in,&input,sizeof(input));timer.expiry_tick=150;
    f.rdx=in;f.r10=out;unmapped=0;swapped=out;contended=true;task_lookup_scope_begin(&current,&f);
    assert(syscall_timerfd_settime(3,0,in,out)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(timer.expiry_tick==150);
    memset((void *)(uintptr_t)in,0x7f,sizeof(input));unmapped=in;contended=false;
    task_lookup_scope_begin(&current,&f);
    assert(!syscall_timerfd_settime(3,0,in,out));
    assert(timer.expiry_tick==400);
    memcpy(&old,(void *)(uintptr_t)out,sizeof(old));assert(!old.it_value.tv_sec && old.it_value.tv_nsec==500000000);
    task_lookup_scope_cancel(&current);assert(allocations==frees);
    /* Cancellation before output readiness releases the accepted input and
     * does not update the timer, even if cancellation is repeated. */
    input=(struct linux_itimerspec){.it_value={.tv_sec=4}};
    memcpy((void *)(uintptr_t)in,&input,sizeof(input));unmapped=0;swapped=out;contended=true;
    task_lookup_scope_begin(&current,&f);
    assert(syscall_timerfd_settime(3,0,in,out)==KERNEL_SYSCALL_SWAP_CONTENDED);
    task_lookup_scope_cancel(&current);task_lookup_scope_cancel(&current);
    assert(timer.expiry_tick==400 && allocations==frees);
    puts("PASS timer input survives retry and scope cancellation");return 0;
}
