/* SPDX-License-Identifier: Apache-2.0 */
/* Exact 128 MiB character mmap and unrelated low-MMIO release operation counts.
 * The actual paging and mapping producers run against host CPU/TLB models.
 */
#define TASK8_FIX3_REUSED_BACKING 1
#define TASK8_FIX4_SCALE 1
#include "task8_fix2_device_mmap_test.c"
extern uint64_t device_mapping_test_visits;
static uint64_t fix4_direct_pdpt[512] __attribute__((aligned(4096)));
static uint64_t fix4_direct_pd[512] __attribute__((aligned(4096)));

/** @brief Count real production policy operations for a DRM-sized view.
 * @return Zero after the device mapping and unrelated MMIO lease retire.
 */
int main(void)
{
    const uint64_t pages_count=32768, low_pages=2048;
    const uint64_t low=6ULL<<30, length=pages_count*4096;
    kernel_pml4[511]=(uintptr_t)kernel_shared_pdpt|3;
    kernel_pml4[256]=(uintptr_t)fix4_direct_pdpt|3;
    fix4_direct_pdpt[6]=(uintptr_t)fix4_direct_pd|3;
    for(unsigned i=0;i<4;++i)fix4_direct_pd[i]=(low+(uint64_t)i*0x200000)|PAGE_SIZE_FLAG|3;
    mmio_dynamic_tables_ready=true;
    setup(1);clear_setup_vmas();
    current->stack_top=RELIEFNT_USER_TOP-4096;
    sched_task_limits(current)->as.rlim_cur=LINUX_RLIM_INFINITY;
    struct task_file file={.used=1,.flags=LINUX_O_RDWR};fix2_device_file=&file;
    assert(!character_device_register(&registration));
    assert(!character_file_open((uint64_t)221<<8,current,&file,&file.character_file));
    uint64_t user=0x40000000ULL;expected_cache=0;
    paging_cache_node_visits=0;device_mapping_test_visits=0;
    int64_t result=syscall_mm_mmap(user,length,3,LINUX_MAP_SHARED|LINUX_MAP_FIXED,3,0);
    fprintf(stderr,"mmap result=%lld expected=%llu provider_calls=%u tree_visits=%llu view_visits=%llu\n",
        (long long)result,(unsigned long long)user,provider_calls,
        (unsigned long long)paging_cache_node_visits,(unsigned long long)device_mapping_test_visits);
    assert(result==(int64_t)user);
    uint64_t map_nodes=paging_cache_node_visits,map_local=device_mapping_test_visits;
    assert(address_space_user_page_phys(sched_task_as(current),user)==device_base);
    assert(address_space_user_page_phys(sched_task_as(current),user+(pages_count-1)*4096)==device_base+(pages_count-1)*4096);
    assert(map_nodes<pages_count*100 && map_local<pages_count*10);
    assert(paging_acquire_mmio(low,low_pages*4096));
    paging_cache_node_visits=0;
    paging_release_mmio(low,low_pages*4096);
    uint64_t release_nodes=paging_cache_node_visits,flags=UINT64_MAX;
    assert(release_nodes<low_pages*100);
    assert(paging_kernel_direct_cache_flags(low,&flags) && !flags);
    assert(paging_kernel_direct_cache_flags(low+(low_pages-1)*4096,&flags) && !flags);
    assert(!paging_ioremap_device_page(device_base,RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD));
    device_mapping_test_visits=0;
    assert(!syscall_mm_munmap(user,length));
    uint64_t unmap_local=device_mapping_test_visits;
    assert(unmap_local<pages_count*10);
    assert(provider_releases==1 && !cache_claims);
    fprintf(stderr,"map_pages=%llu map_tree_visits=%llu map_view_visits=%llu low_pages=%llu low_release_tree_visits=%llu unmap_view_visits=%llu\n",
        (unsigned long long)pages_count,(unsigned long long)map_nodes,(unsigned long long)map_local,
        (unsigned long long)low_pages,(unsigned long long)release_nodes,(unsigned long long)unmap_local);
    character_file_close(file.character_file,&file);file.character_file=NULL;
    assert(!character_device_unregister(&registration));fix2_device_file=NULL;
    /* These four direct-map splits are synthetic fixture tables; real kernel
     * direct-map page tables remain kernel-owned for the system lifetime. */
    for(unsigned i=0;i<4;++i) {
        uint64_t entry=fix4_direct_pd[i];assert(!(entry&PAGE_SIZE_FLAG));
        mm_free_page(entry&RELIEFNT_PHYS_ADDR_MASK);fix4_direct_pd[i]=0;
    }
    kernel_pml4[256]=0;
    teardown();assert(!cache_claims);
    return 0;
}
