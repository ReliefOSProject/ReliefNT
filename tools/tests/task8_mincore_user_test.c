/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: mincore user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../mm/user/memory_internal.h"
#include <reliefnt/swap.h>
static struct task current;
static struct task_vma vma;
static unsigned char output[65536] __attribute__((aligned(4096)));
static bool contended=true;
static unsigned page_queries,first_prepares,second_prepares,allocations;
struct task *sched_current_task(void) { return &current; }
uint64_t align_up_page(uint64_t value){return (value+4095u)&~4095ULL;}
struct task_vma *task_vma_containing(struct task *task,uint64_t start,uint64_t end)
{ assert(task==&current);return start>=vma.start && end<=vma.end?&vma:NULL; }
uint64_t address_space_user_page_phys(const struct address_space *as,uint64_t address)
{ assert(as==sched_task_as(&current) && address>=vma.start && address<vma.end);++page_queries;return ((address-vma.start)/4096 & 1)?0:0x4000; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    assert(task==&current && writing && length<=32768);
    if(address==(uintptr_t)output)++first_prepares;
    if(address==(uintptr_t)output+32768){++second_prepares;if(contended)return SWAP_STORAGE_CONTENDED;}
    return 0;
}
bool user_range_writable(uint64_t address,uint64_t length)
{ return address>=(uintptr_t)output && length<=32768 && address+length<=(uintptr_t)output+sizeof(output); }
enum paging_user_entry_kind paging_user_entry_kind(const struct address_space *as,uint64_t address)
{ (void)as;(void)address;assert(0);return PAGING_USER_ENTRY_EMPTY; }
int syscall_handle_user_page_fault_detailed(uint64_t address,uint64_t error,bool user_fault)
{ (void)address;(void)error;(void)user_fault;assert(0);return 0; }
void *kernel_malloc(size_t n){void *p=malloc(n);if(p)++allocations;return p;}
void kernel_free(void *p){if(p){assert(allocations);--allocations;free(p);}}
void storage_drain_task_io(uint32_t pid){assert(pid==current.pid);}
void kernel_wait_queue_remove(struct kernel_wait_queue *queue,struct task *task){(void)queue;(void)task;assert(0);}
void vfs_walk_cancel(struct vfs_walk **walk){assert(!*walk);}
void vfs_path_put(struct vfs_path *path){assert(!path->mount);}
void storage_inode_allow_write(struct storage_inode_ref *inode){(void)inode;assert(0);}
int storage_inode_put(struct storage_inode_ref *inode){(void)inode;assert(0);return 0;}
bool sysv_shm_page_resident(struct task_vma *v,uint64_t page){(void)v;(void)page;assert(0);return false;}
bool mm_shared_resident(struct address_space *as,uint64_t page){(void)as;(void)page;assert(0);return false;}
bool storage_tmpfs_page_resident(const struct storage_node *node,uint64_t offset){(void)node;(void)offset;assert(0);return false;}
int page_cache_lookup(const struct storage_node *node,uint64_t offset,uint64_t *phys)
{(void)node;(void)offset;(void)phys;assert(0);return -1;}
void page_cache_release(uint64_t physical){(void)physical;assert(0);}
int main(void)
{
    current.kind=TASK_KIND_USER;current.pid=7;vma=(struct task_vma){.used=1,.start=0x1000000,
        .end=0x1000000+sizeof(output)*4096ULL,.flags=TASK_VMA_FLAG_ANON};
    struct trap_frame frame={.rax=LINUX_SYS_MINCORE,.rdi=vma.start,.rsi=sizeof(output)*4096ULL,.rdx=(uintptr_t)output};
    task_lookup_scope_begin(&current,&frame);
    assert(syscall_mm_mincore(frame.rdi,frame.rsi,frame.rdx)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(page_queries==32768 && first_prepares==1 && second_prepares==1);
    output[0]=0x5a;contended=false;task_lookup_scope_begin(&current,&frame);
    assert(!syscall_mm_mincore(frame.rdi,frame.rsi,frame.rdx));
    assert(output[0]==0x5a && output[1]==0 && output[32768]==1 && page_queries==65536 && first_prepares==1 && second_prepares==2);
    task_lookup_scope_cancel(&current);assert(!allocations);
    puts("PASS mincore: bounded output progress, no target page-in or repeated prefix");
    return 0;
}
