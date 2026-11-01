/* SPDX-License-Identifier: Apache-2.0 */
#define main unused_swap_vma_fixture_main
#include "brk_collision_test.c"
#undef main
#include "../../block/core/storage_internal.h"

/** @brief Enter fixture execution ownership. @param flags Saved interrupt state. */
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags=0; }
/** @brief Leave fixture execution ownership. @param flags Saved interrupt state. */
void kernel_execution_unlock_irqrestore(uint64_t flags) { assert(!flags); }

/** @brief Reject a swap fault: the VMA-only page model has no swap leaves.
 * @param task Unexpected task. @param address Unexpected address.
 * @param error Unexpected fault flags. @return Never; such a call is a bug.
 */
int swap_resolve_fault(struct task *task, uint64_t address, uint64_t error)
{ (void)task; (void)address; (void)error; abort(); }

/** @brief Exclude all new shared file mappings during an active swap claim.
 * @return Zero when denial, ownership and control mappings are correct.
 */
int main(void)
{
    struct task owner={.kind=TASK_KIND_USER};owner.stack_top=RELIEFNT_USER_TOP-4096;
    owner.limits.as.rlim_cur=owner.limits.as.rlim_max=LINUX_RLIM_INFINITY;current_task=&owner;
    struct storage_inode_ref inode={.node={.type=RELIEFOS_FS_TYPE_FILE}};
    assert(!storage_inode_write_get(&inode));
    framebuffer_file=(struct task_file){.used=1,.references=1,.flags=LINUX_O_RDWR,
        .inode=&inode,
        .node={.type=RELIEFOS_FS_TYPE_FILE,.first_cluster=50,.size=16384}};
    bind_mmap_provider(&framebuffer_file);
    const uint64_t base=0x52000000,page=4096,fixed=LINUX_MAP_FIXED_NOREPLACE;
    unsigned pins=mmap_provider_pins,allocations=page_alloc_attempts;
    assert(!storage_inode_swap_claim(&inode));
    assert(syscall_mm_mmap(base,page,LINUX_PROT_WRITE,LINUX_MAP_SHARED|fixed,7,0)==-ETXTBSY);
    assert(syscall_mm_mmap(base,page,LINUX_PROT_READ,LINUX_MAP_SHARED|fixed,7,0)==-ETXTBSY);
    assert(syscall_mm_mmap(base,page,LINUX_PROT_NONE,LINUX_MAP_SHARED|fixed,7,0)==-ETXTBSY);
    assert(!mmap_result_vma(&owner,base) && framebuffer_file.references==1 &&
           mmap_provider_pins==pins && page_alloc_attempts==allocations);
    file_writable=0;
    assert(syscall_mm_mmap(base,page,LINUX_PROT_READ,LINUX_MAP_SHARED|fixed,7,0)==-ETXTBSY);
    assert(syscall_mm_mmap(base,page,LINUX_PROT_NONE,LINUX_MAP_SHARED|fixed,7,0)==-ETXTBSY);
    assert(!mmap_result_vma(&owner,base) && framebuffer_file.references==1 &&
           mmap_provider_pins==pins && page_alloc_attempts==allocations);
    file_writable=1;
    assert(syscall_mm_mmap(base,page,LINUX_PROT_READ,LINUX_MAP_PRIVATE|fixed,7,0)==(int64_t)base);
    assert(!syscall_mm_munmap(base,page));
    storage_inode_swap_release(&inode);
    assert(syscall_mm_mmap(base,page,LINUX_PROT_READ,LINUX_MAP_SHARED|fixed,7,0)==(int64_t)base);
    struct task_vma *vma=mmap_result_vma(&owner,base);assert(vma);
    vma->inode=framebuffer_file.inode;
    /* Direct claim models the independent protection guard for an extant VMA. */
    assert(!storage_inode_swap_claim(&inode));
    assert(syscall_mm_mprotect(base,page,LINUX_PROT_WRITE)==-ETXTBSY);
    storage_inode_swap_release(&inode);
    assert(!syscall_mm_mprotect(base,page,LINUX_PROT_WRITE));
    assert(!syscall_mm_munmap(base,page));task_file_put(&framebuffer_file);
    storage_inode_write_put(&inode);assert(!mmap_provider_pins && !inode.write_count);
    puts("swap shared mmap admission, ownership and mprotect exclusion: ok");return 0;
}
