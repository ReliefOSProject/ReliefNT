/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Failed mmap must not retain claims in a reused DEVICE backing.
 * Character mmap, page tables, VMA and PCI policy are production linked;
 * provider backing, physical memory, CPU and TLB are host models.
 */
#define TASK8_FIX3_REUSED_BACKING 1
#include "task8_fix2_device_mmap_test.c"

/** @brief Exercise failed and successful admissions on one reused backing.
 * @param nonadjacent Make the second physical page discontiguous.
 * @param lifetime_only Skip failure controls for the isolated lifetime RED.
 */
static void run_shared_case(bool nonadjacent,bool lifetime_only)
{
    fix3_page_stride=nonadjacent ? 0x20000 : 4096;
    provider_releases=provider_calls=0;
    kernel_pml4[511]=(uintptr_t)kernel_shared_pdpt|3;
    mmio_dynamic_tables_ready=true;
    setup(1);clear_setup_vmas();
    current->stack_top=RELIEFNT_USER_TOP-4096;
    sched_task_limits(current)->as.rlim_cur=LINUX_RLIM_INFINITY;
    struct task_file file={.used=1,.flags=LINUX_O_RDWR};fix2_device_file=&file;
    assert(!character_device_register(&registration));
    assert(!character_file_open((uint64_t)221<<8,current,&file,&file.character_file));
    struct device_mapping_info info={.length=8192,.max_prot=3,.can_split=true,.can_move=true};
    expected_cache=0;
    assert(!device_mapping_create(NULL,&backing_ops,&device_base,&info,&reused_backing));
    uint64_t user=base+0x1000000;
    fix2_device_user_start=user;fix2_device_user_end=user+4096;
    uint64_t mapflags=LINUX_MAP_SHARED|LINUX_MAP_FIXED_NOREPLACE;
    assert(syscall_mm_mmap(user,4096,3,mapflags,3,0)==(int64_t)user);
    uint64_t original=address_space_user_page_phys(sched_task_as(current),user);
    uint64_t original_flags=address_space_user_page_flags(sched_task_as(current),user);
    assert(original==device_base && (original_flags&RELIEFNT_PAGE_WRITABLE));
    assert(!regular_stage_begin(current,user,64,false));
    struct regular_io_stage *stage=regular_stage_find(current);
    assert(stage && stage->page[0].device);
    uint64_t target=(user&~((1ULL<<21)-1))+(1ULL<<21)-4096;
    assert(target!=user && !address_space_user_page_phys(sched_task_as(current),target));
    void *lease;
    if(!lifetime_only) {
    /* Page A shares the already allocated PT, then page B needs another PT.
     * The actual cache claim for B is selected before this allocation fails. */
    fail_page_after=0;
    int64_t ret=syscall_mm_mmap(target,8192,3,mapflags,3,0);
    fail_page_after=-1;
    assert(ret==-RELIEFOS_ENOMEM);
    assert(!address_space_user_page_phys(sched_task_as(current),target));
    assert(!address_space_user_page_phys(sched_task_as(current),target+4096));
    assert(address_space_user_page_phys(sched_task_as(current),user)==original);
    assert(address_space_user_page_flags(sched_task_as(current),user)==original_flags);
    assert(regular_stage_find(current)==stage && !provider_releases);
    /* B was never published: its WB policy cannot reject the real PCI UC lease. */
    lease=pci_map_mmio_lease(device_base+fix3_page_stride,4096);
    assert(lease);
    pci_unmap_mmio(lease,4096);
    /* Cover view allocation, first-page policy allocation and the next-page
     * policy allocation without losing the earlier VMA/stage. */
    for(int failure=0;failure<3;++failure) {
        fail_after=failure;
        ret=syscall_mm_mmap(target,8192,3,mapflags,3,0);
        fail_after=-1;
        assert(ret==-RELIEFOS_ENOMEM);
        assert(!address_space_user_page_phys(sched_task_as(current),target));
        assert(address_space_user_page_phys(sched_task_as(current),user)==original);
        assert(regular_stage_find(current)==stage && !provider_releases);
        lease=pci_map_mmio_lease(device_base+fix3_page_stride,4096);assert(lease);
        pci_unmap_mmio(lease,4096);
    }
    /* A later publication failure occurs after both leaves and both selected
     * policies existed; rollback must remove only this attempt's owners. */
    fail_fix3_vma_publication=true;
    ret=syscall_mm_mmap(target,8192,3,mapflags,3,0);
    fail_fix3_vma_publication=false;
    assert(ret==-RELIEFOS_ENOMEM);
    assert(!address_space_user_page_phys(sched_task_as(current),target));
    assert(!address_space_user_page_phys(sched_task_as(current),target+4096));
    assert(address_space_user_page_phys(sched_task_as(current),user)==original);
    assert(regular_stage_find(current)==stage && !provider_releases);
    lease=pci_map_mmio_lease(device_base+fix3_page_stride,4096);assert(lease);
    pci_unmap_mmio(lease,4096);
    }

    /* Successful A+B admission has page-specific ownership: dropping B
     * cannot leave it reserved merely because A and the provider remain. */
    assert(syscall_mm_mmap(target,8192,3,mapflags,3,0)==(int64_t)target);
    assert(!syscall_mm_munmap(target+4096,4096));
    assert(address_space_user_page_phys(sched_task_as(current),target)==device_base);
    lease=pci_map_mmio_lease(device_base+fix3_page_stride,4096);assert(lease);
    pci_unmap_mmio(lease,4096);
    assert(!syscall_mm_munmap(target,4096));
    task_regular_io_stage_cancel(current);

    /* A retained B stage plus a forked VMA each independently keep B's
     * policy. Both must retire before an incompatible UC lease can start. */
    assert(syscall_mm_mmap(target,8192,3,mapflags,3,0)==(int64_t)target);
    fix2_device_user_start=target+4096;fix2_device_user_end=target+8192;
    assert(!regular_stage_begin(current,target+4096,64,false));
    stage=regular_stage_find(current);assert(stage && stage->page[0].device);
    struct task *parent=current,*child=calloc(1,sizeof(*child));assert(child);
    child->kind=TASK_KIND_USER;child->stack_top=parent->stack_top;
    sched_task_limits(child)->as.rlim_cur=LINUX_RLIM_INFINITY;
    assert(address_space_clone_cow(sched_task_as(parent),sched_task_as(child)));
    for(uint32_t i=0;i<sched_task_vma_capacity(parent);++i) {
        const struct task_vma *src=sched_task_vma_at(parent,i);
        if(!src || !src->used)continue;
        struct task_vma *dst=sched_task_vma_at(child,i);assert(dst);
        *dst=*src;dst->device_mapping=NULL;
        assert(!sched_task_vma_retain_device(dst,src));
    }
    assert(!syscall_mm_munmap(target+4096,4096));
    assert(!pci_map_mmio_lease(device_base+fix3_page_stride,4096));
    current=child;
    assert(!syscall_mm_munmap(target+4096,4096));
    assert(!pci_map_mmio_lease(device_base+fix3_page_stride,4096));
    /* Move the surviving split in the child; the original parent A remains. */
    uint64_t moved=target+0x400000;
    assert(!syscall_mm_mprotect(target,4096,LINUX_PROT_READ));
    assert(syscall_mm_mremap(target,4096,4096,
        LINUX_MREMAP_MAYMOVE|LINUX_MREMAP_FIXED,moved)==(int64_t)moved);
    assert(address_space_user_page_phys(sched_task_as(child),moved)==device_base);
    assert(!syscall_mm_munmap(moved,4096));
    current=parent;
    assert(address_space_user_page_phys(sched_task_as(parent),target)==device_base);
    assert(!pci_map_mmio_lease(device_base+fix3_page_stride,4096));
    task_regular_io_stage_cancel(parent);
    lease=pci_map_mmio_lease(device_base+fix3_page_stride,4096);assert(lease);
    pci_unmap_mmio(lease,4096);
    address_space_destroy(sched_task_as(child));sched_task_vma_release(child);free(child);
    assert(!syscall_mm_munmap(target,4096));
    task_regular_io_stage_cancel(current);task_regular_io_stage_cancel(current);
    assert(!syscall_mm_munmap(user,4096));
    assert(!provider_releases);
    struct device_mapping *last=reused_backing;reused_backing=NULL;
    device_mapping_put(last);assert(provider_releases==1 && !policy_count());
    character_file_close(file.character_file,&file);file.character_file=NULL;
    assert(!character_device_unregister(&registration));fix2_device_file=NULL;
    teardown();assert(!cache_claims);
}

/** @brief Run contiguous and discontiguous provider-page ownership controls.
 * @param argc Argument count. @param argv Optional "lifetime" negative control.
 * @return Zero after all provider and policy ownership is retired.
 */
int main(int argc,char **argv)
{
    bool lifetime_only=argc>1 && !strcmp(argv[1],"lifetime");
    run_shared_case(false,lifetime_only);
    run_shared_case(true,lifetime_only);
    puts("PASS reused DEVICE backing: rollback, page lifetime, stage, fork and move");
    return 0;
}
