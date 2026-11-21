/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: lookup resume.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/syscall/fs/lookup_state.c"
static struct task current;
static _Alignas(4096) unsigned char input[65536];
static uint64_t blocked;
static bool contended, backing_busy;
static unsigned first_prepares,second_prepares,allocations,frees;
void *kernel_malloc(size_t n) { void *p=malloc(n);if(p)++allocations;return p; }
void kernel_free(void *p) { if(p){++frees;free(p);} }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    assert(task==&current && !writing && length<=32768);
    if(address==(uintptr_t)input)++first_prepares;
    if(address==blocked){++second_prepares;if(contended)return SWAP_STORAGE_CONTENDED;}
    return 0;
}
struct task *sched_current_task(void) { return &current; }
bool address_space_user_page_readable(const struct address_space *as, uint64_t page)
{
    assert(as == sched_task_as(&current));
    return !backing_busy || page != blocked;
}
int syscall_handle_user_page_fault_detailed(uint64_t address, uint64_t error, bool user_fault)
{
    assert(address == blocked && error == 4 && !user_fault && backing_busy);
    return -EAGAIN;
}
int syscall_handle_user_page_fault(uint64_t address, uint64_t error)
{ return syscall_handle_user_page_fault_detailed(address, error, false) > 0; }
void sched_task_identity_put(struct task_identity *identity) { (void)identity; assert(0); }
void storage_drain_task_io(uint32_t pid) { assert(pid==current.pid); }
void task_process_vm_scope_release(void *state) { (void)state;assert(0); }
void kernel_wait_queue_remove(struct kernel_wait_queue *queue,struct task *task)
{ (void)queue;(void)task;assert(0); }
void vfs_walk_cancel(struct vfs_walk **walk) { assert(!*walk); }
void vfs_path_put(struct vfs_path *path) { assert(!path->mount); }
void storage_inode_allow_write(struct storage_inode_ref *inode) { (void)inode;assert(0); }
int storage_inode_put(struct storage_inode_ref *inode) { (void)inode;assert(0);return 0; }
int copy_user_path(char *dst,uint32_t cap,uint64_t ptr) { (void)dst;(void)cap;(void)ptr;assert(0);return -1; }
int main(void)
{
    current.pid=42;current.kind=TASK_KIND_USER;
    for(size_t i=0;i<sizeof(input);++i)input[i]=(unsigned char)(i*13u);
    struct trap_frame frame={.rax=LINUX_SYS_SENDMSG,.rdi=3,.rsi=(uintptr_t)input};
    task_lookup_scope_begin(&current,&frame);
    struct task_user_text out={0};blocked=(uintptr_t)input+32768;contended=true;
    assert(task_lookup_user_bytes(&current,(uintptr_t)input,sizeof(input),&out)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!out.text && first_prepares==1 && second_prepares==1);
    memset(input,0xa5,32768);contended=false;task_lookup_scope_begin(&current,&frame);
    assert(!task_lookup_user_bytes(&current,(uintptr_t)input,sizeof(input),&out));
    assert(first_prepares==1 && second_prepares==2);
    for(size_t i=0;i<sizeof(input);++i)assert((unsigned char)out.text[i]==(unsigned char)(i*13u));
    task_lookup_scope_cancel(&current);assert(!current.lookup_scope.armed && allocations==frees);
    puts("PASS scoped byte import: bounded resume, original prefix, one-time cancellation");

    /* X11 writev can include a string in a cold file-backed library page.
     * The disk owner's temporary contention must not fault the socket write. */
    frame.rax = LINUX_SYS_WRITEV;
    memcpy(input, "sent", 4);
    memcpy(input + 32768, "X11", 4);
    task_lookup_scope_begin(&current, &frame);
    backing_busy = true;
    assert(task_lookup_user_bytes(&current, (uintptr_t)input, sizeof(input), &out) ==
           KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!out.text);
    memcpy(input, "lost", 4);
    backing_busy = false;
    task_lookup_scope_begin(&current, &frame);
    assert(!task_lookup_user_bytes(&current, (uintptr_t)input, sizeof(input), &out));
    assert(!memcmp(out.text, "sent", 4) && !memcmp(out.text + 32768, "X11", 4));
    task_lookup_scope_cancel(&current);
    assert(allocations == frees);
    puts("PASS scoped byte import: contested lazy page waits without dropping the copied prefix");
}
