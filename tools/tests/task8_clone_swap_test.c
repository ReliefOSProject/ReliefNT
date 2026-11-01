/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Clone argument and TID preparation with an owned retry snapshot.
 * Production clone3 and lookup scope run with modeled swap and child creation.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/swap.h>

static struct task caller;
static uintptr_t swapped,unmapped;
static bool contended;
static unsigned allocations,frees,created;
struct task *sched_current_task(void) { return &caller; }
void *kernel_malloc(size_t n) { void *p=malloc(n);if(p)++allocations;return p; }
void kernel_free(void *p) { if(p){++frees;free(p);} }
bool user_range_ok(uint64_t address,uint64_t size)
{ return address>=4096 && size<=UINT64_MAX-address && address!=swapped && address!=unmapped; }
bool user_range_writable(uint64_t address,uint64_t size) { return user_range_ok(address,size); }
bool kernel_execution_is_outermost(void) { return true; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t size,bool writing)
{
    assert(task==&caller && size<=4096);(void)writing;
    if(address==unmapped || address<4096)return -LINUX_EFAULT;
    if(address==swapped){if(contended)return SWAP_STORAGE_CONTENDED;swapped=0;}
    return 0;
}
/* The actual scheduler receives these prepared arguments in integration tests;
 * here child publication is deliberately modeled to detect premature entry. */
int64_t sched_clone_current(const struct trap_frame *frame,uint64_t flags,uint64_t stack,
                            uint64_t parent_tid,uint64_t child_tid,uint64_t tls)
{
    (void)frame;(void)stack;(void)tls;
    if((flags&CLONE_PARENT_SETTID) && !user_range_writable(parent_tid,4))return -LINUX_EFAULT;
    if((flags&CLONE_CHILD_SETTID) && !user_range_writable(child_tid,4))return -LINUX_EFAULT;
    ++created;
    if(flags&CLONE_PARENT_SETTID)*(uint32_t *)(uintptr_t)parent_tid=71;
    if(flags&CLONE_CHILD_SETTID)*(uint32_t *)(uintptr_t)child_tid=71;
    return 71;
}
int sched_clone_validate_flags(uint64_t flags,uint64_t tls)
{ (void)flags;(void)tls;return 0; }
uint64_t address_space_user_page_phys(const struct address_space *as,uint64_t va)
{ (void)as;return va&~4095ULL; }
static unsigned aliases;
void paging_writable_alias_add(struct paging_writable_alias *alias,uint64_t physical)
{ assert(alias && physical);alias->physical=physical;++aliases; }
void paging_writable_alias_remove(struct paging_writable_alias *alias)
{ assert(alias && aliases);--aliases; }

int main(int argc,char **argv)
{
    assert(argc==2);caller.kind=TASK_KIND_USER;caller.pid=42;
    uint32_t parent=0,child=0;
    struct clone_args args={.flags=CLONE_VM|CLONE_PARENT_SETTID|CLONE_CHILD_SETTID,
                            .exit_signal=17,.parent_tid=(uintptr_t)&parent,.child_tid=(uintptr_t)&child};
    struct trap_frame frame={.rax=LINUX_SYS_CLONE3,.rdi=(uintptr_t)&args,.rsi=sizeof(args)};
    task_lookup_scope_begin(&caller,&frame);
    if(!strcmp(argv[1],"args")) {
        swapped=(uintptr_t)&args;
        assert(syscall_clone3(&frame,frame.rdi,frame.rsi)==71);
    } else {
        assert(!strcmp(argv[1],"parent") || !strcmp(argv[1],"child") || !strcmp(argv[1],"cancel"));
        swapped=!strcmp(argv[1],"child") ? (uintptr_t)&child : (uintptr_t)&parent;
        contended=true;
        assert(syscall_clone3(&frame,frame.rdi,frame.rsi)==KERNEL_SYSCALL_SWAP_CONTENDED);
        assert(!created && !parent && !child && !aliases);
        if(!strcmp(argv[1],"cancel")) {
            task_lookup_scope_cancel(&caller);task_lookup_scope_cancel(&caller);
            assert(allocations==frees);puts("PASS clone canceled before child publication");return 0;
        }
        args.flags=0;args.parent_tid=0;args.child_tid=0;unmapped=(uintptr_t)&args;
        contended=false;task_lookup_scope_begin(&caller,&frame);
        assert(syscall_clone3(&frame,frame.rdi,frame.rsi)==71);
    }
    assert(created==1 && parent==71 && child==71 && !aliases);
    task_lookup_scope_cancel(&caller);assert(allocations==frees);
    puts("PASS clone3 imported args and TID preparation");return 0;
}
