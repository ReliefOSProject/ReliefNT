/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: process vm swap.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_process_vm_main
#include "process_vm_test.c"
#undef main
#include <reliefnt/swap.h>

struct task_identity {struct task *task;};
static struct task_identity identity;
static unsigned identity_refs;
static uint64_t swapped_page,local_swapped_page,import_blocked,generation=17;
static bool contended=true,fail_ensure;
static unsigned ensure_calls;
int sched_task_identity_get(struct task *task,struct task_identity **out)
{ assert(task==&target);identity.task=task;++identity_refs;*out=&identity;return 0; }
struct task *sched_task_identity_task(const struct task_identity *id)
{ assert(id==&identity);return id->task; }
void sched_task_identity_put(struct task_identity *id)
{ assert(id==&identity && identity_refs);--identity_refs; }
uint64_t mm_context_generation_existing(const struct address_space *as)
{ return as==sched_task_as(&target)?generation:0; }
int mm_context_ensure_generation(struct address_space *as,uint64_t *out)
{
    assert(as==sched_task_as(&target));++ensure_calls;
    if(fail_ensure)return -LINUX_ENOMEM;
    generation=91;*out=generation;return 0;
}
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    assert(length<=32768);
    if(task==&caller && address==import_blocked && contended)return SWAP_STORAGE_CONTENDED;
    bool target_swap=task==&target && (address&~4095ULL)==swapped_page;
    bool local_swap=task==&caller && (address&~4095ULL)==local_swapped_page;
    if(target_swap || local_swap) {
        if(contended)return SWAP_STORAGE_CONTENDED;
        uint64_t physical=mm_alloc_page();memcpy((void *)(uintptr_t)physical,"swap-page",9);
        assert(paging_user_swap_in(sched_task_as(task),address,physical,writing));
        if(target_swap)swapped_page=0;else local_swapped_page=0;
    }
    return 0;
}
void storage_drain_task_io(uint32_t pid){assert(pid==caller.pid);}
void kernel_wait_queue_remove(struct kernel_wait_queue *queue,struct task *task){(void)queue;(void)task;assert(0);}
void vfs_walk_cancel(struct vfs_walk **walk){assert(!*walk);}
void vfs_path_put(struct vfs_path *path){assert(!path->mount);}
void storage_inode_allow_write(struct storage_inode_ref *inode){(void)inode;assert(0);}
int storage_inode_put(struct storage_inode_ref *inode){(void)inode;assert(0);return 0;}

int main(void)
{
    nx_enabled=true;caller.pid=caller.tgid=10;target.pid=target.tgid=20;
    caller.kind=target.kind=TASK_KIND_USER;
    caller.uid=caller.euid=caller.suid=target.uid=target.euid=target.suid=1000;
    caller.gid=caller.egid=caller.sgid=target.gid=target.egid=target.sgid=100;
    assert(address_space_create(&caller.as) && address_space_create(&target.as));
    const uint64_t va=RELIEFNT_USER_BASE+0x20000;
    uint64_t local_page=add_page(&caller,va,true);
    target.address_space.vmas[0]=(struct task_vma){.used=1,.start=va,.end=va+4096,
        .flags=TASK_VMA_FLAG_ANON,.prot=TASK_VMA_PROT_READ|TASK_VMA_PROT_WRITE};
    assert(address_space_map_user_swap(&target.as,va,RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    swapped_page=va;
    struct iovec local={(void *)va,9},remote={(void *)va,9};
    struct trap_frame frame={.rax=LINUX_SYS_PROCESS_VM_READV,.rdi=20,.rsi=(uintptr_t)&local,.rdx=1,
        .r10=(uintptr_t)&remote,.r8=1};
    task_lookup_scope_begin(&caller,&frame);
    assert(transfer(20,&local,1,&remote,1,0,false)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(identity_refs==1 && paging_user_entry_kind(&target.as,va)==PAGING_USER_ENTRY_SWAP);
    local.iov_len=1;remote.iov_len=1;contended=false;task_lookup_scope_begin(&caller,&frame);
    assert(transfer(20,&local,1,&remote,1,0,false)==9 && !memcmp((void *)(uintptr_t)local_page,"swap-page",9));
    task_lookup_scope_cancel(&caller);assert(!identity_refs && !allocations);
    uint64_t resident=address_space_user_page_phys(&target.as,va);

    /* A copied length word is not reloaded after contention on its base. */
    local=(struct iovec){(void *)va,9};remote=(struct iovec){(void *)va,9};
    import_blocked=(uintptr_t)&local;contended=true;
    task_lookup_scope_begin(&caller,&frame);
    assert(transfer(20,&local,1,&remote,1,0,false)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!identity_refs);local.iov_len=1;import_blocked=0;contended=false;
    task_lookup_scope_begin(&caller,&frame);
    assert(transfer(20,&local,1,&remote,1,0,false)==9);
    task_lookup_scope_cancel(&caller);assert(!identity_refs && !allocations);

    target.address_space.vmas[0].end=va+8192;
    assert(address_space_map_user_swap(&target.as,va+4096,RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    swapped_page=va+4096;local.iov_len=9;remote=(struct iovec){(void *)(uintptr_t)swapped_page,9};
    generation=0;contended=true;task_lookup_scope_begin(&caller,&frame);
    assert(transfer(20,&local,1,&remote,1,0,false)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(identity_refs==1 && ensure_calls==1 && generation==91);
    uint64_t same_root=target.as.cr3;generation=92;contended=false;task_lookup_scope_begin(&caller,&frame);
    assert(target.as.cr3==same_root && transfer(20,&local,1,&remote,1,0,false)==-LINUX_EFAULT);
    assert(swapped_page==va+4096);task_lookup_scope_cancel(&caller);assert(!identity_refs && !allocations);

    generation=17;contended=true;task_lookup_scope_begin(&caller,&frame);
    assert(transfer(20,&local,1,&remote,1,0,false)==KERNEL_SYSCALL_SWAP_CONTENDED && identity_refs==1);
    identity.task=NULL;contended=false;task_lookup_scope_begin(&caller,&frame);
    assert(sched_find(20)==&target && transfer(20,&local,1,&remote,1,0,false)==-LINUX_ESRCH);
    task_lookup_scope_cancel(&caller);assert(!identity_refs && !allocations);

    generation=0;fail_ensure=true;contended=true;task_lookup_scope_begin(&caller,&frame);
    assert(transfer(20,&local,1,&remote,1,0,false)==-LINUX_ENOMEM && !identity_refs);
    assert(ensure_calls==2);task_lookup_scope_cancel(&caller);assert(!allocations);
    fail_ensure=false;generation=17;

    /* The first page in one remote GUP batch is unpinned before a wait on the next. */
    struct iovec remote_batch={(void *)(va+4090),15};
    local=(struct iovec){(void *)va,15};frame.r10=(uintptr_t)&remote_batch;frame.r8=1;
    unsigned batch_refs=0;
    for(unsigned i=0;i<2048;++i)if(pages[i].address==resident)batch_refs=pages[i].refs;
    contended=true;task_lookup_scope_begin(&caller,&frame);
    assert(transfer(20,&local,1,&remote_batch,1,0,false)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(identity_refs==1);
    for(unsigned i=0;i<2048;++i)if(pages[i].address==resident)assert(pages[i].refs==batch_refs);
    task_lookup_scope_cancel(&caller);assert(!identity_refs && !allocations);

    struct iovec remote_pair[]={{(void *)va,4},{(void *)(uintptr_t)swapped_page,4}};
    local=(struct iovec){(void *)va,8};frame.r10=(uintptr_t)remote_pair;frame.r8=2;
    unsigned before_refs=0;
    for(unsigned i=0;i<2048;++i)if(pages[i].address==resident)before_refs=pages[i].refs;
    task_lookup_scope_begin(&caller,&frame);
    assert(transfer(20,&local,1,remote_pair,2,0,false)==4 && !identity_refs);
    for(unsigned i=0;i<2048;++i)if(pages[i].address==resident)assert(pages[i].refs==before_refs);
    task_lookup_scope_cancel(&caller);assert(!allocations);

    assert(address_space_map_user_swap(&caller.as,va+4096,RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    local_swapped_page=va+4096;local=(struct iovec){(void *)(uintptr_t)local_swapped_page,9};
    remote=(struct iovec){(void *)va,9};frame.r10=(uintptr_t)&remote;frame.r8=1;contended=true;
    task_lookup_scope_begin(&caller,&frame);
    assert(transfer(20,&local,1,&remote,1,0,false)==KERNEL_SYSCALL_SWAP_CONTENDED && identity_refs==1);
    contended=false;task_lookup_scope_begin(&caller,&frame);
    assert(transfer(20,&local,1,&remote,1,0,false)==9);
    uint64_t restored=address_space_user_page_phys(&caller.as,va+4096);
    assert(!memcmp((void *)(uintptr_t)restored,"swap-page",9));
    task_lookup_scope_cancel(&caller);assert(!identity_refs && !allocations);
    address_space_destroy(&caller.as);address_space_destroy(&target.as);
    puts("PASS process_vm: snapshots, swap in both MMs, stable identity, ABA, ENOMEM and partial progress");
    return 0;
}
