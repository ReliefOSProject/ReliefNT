/* Real swapon/swapoff frontdoors, drain, backend, MM records and PTEs.
 * VFS/device, physical RAM, CPU ownership and execution checkpoint are modeled. */
#define TASK9_SWAPOFF_REAL_MM 1
#define main old_swap_activation_main
#include "swap_activation_test.c"
#undef main
#include <linux/mman.h>
#include "../../arch/x86_64/mm/paging.c"

static struct { uint64_t physical; unsigned refs; } host_pages[64];
static unsigned live_host_pages, fail_page_after;
uint64_t mm_alloc_page(void)
{
    if (fail_page_after && !--fail_page_after) return 0;
    void *page=aligned_alloc(4096,4096); assert(page); memset(page,0,4096);
    unsigned i=0; while (i<64 && host_pages[i].physical) ++i; assert(i<64);
    host_pages[i].physical=(uintptr_t)page; host_pages[i].refs=1; ++live_host_pages;
    return (uintptr_t)page;
}
uint64_t mm_alloc_emergency_page(void) { return mm_alloc_page(); }
uint32_t mm_page_refcount(uint64_t phys)
{ for (unsigned i=0;i<64;++i) if(host_pages[i].physical==phys) return host_pages[i].refs; return 0; }
void mm_retain_page(uint64_t phys)
{ for(unsigned i=0;i<64;++i) if(host_pages[i].physical==phys){++host_pages[i].refs;return;} abort(); }
void mm_free_page(uint64_t phys)
{
    for(unsigned i=0;i<64;++i) if(host_pages[i].physical==phys) {
        assert(host_pages[i].refs); if(!--host_pages[i].refs) {
            free((void *)(uintptr_t)phys);host_pages[i].physical=0;--live_host_pages;
        } return;
    }
    abort();
}
bool mm_free_page_reclaimed(uint64_t phys)
{ bool freed=mm_page_refcount(phys)==1;mm_free_page(phys);return freed; }
void smp_flush_user_tlb(void) { }
void x86_64_invlpg(uint64_t address) { (void)address; }
int page_cache_owns(uint64_t phys) { (void)phys;return 0; }
int page_cache_retain(uint64_t phys) { (void)phys;return -1; }
void page_cache_release(uint64_t phys) { (void)phys;abort(); }
bool futex_swap_pinned(uint64_t phys) { (void)phys;return false; }

#ifndef SWAPOFF_FRONTDOOR_FIXTURE_ENTRY
#define SWAPOFF_FRONTDOOR_FIXTURE_ENTRY main
#endif
int SWAPOFF_FRONTDOOR_FIXTURE_ENTRY(void)
{
    files[0].node=(struct storage_node){.type=RELIEFOS_FS_TYPE_FILE,
        .flags=STORAGE_NODE_FLAG_EXT2,.first_cluster=1};
    files[0].physical=64; header(64);
    assert(!syscall_mm_swapon((uintptr_t)"one",0));
    printf("live backing claimed=%u device_refs=%u\n",(unsigned)files[0].claimed,refs);fflush(stdout);
    assert(files[0].claimed && claims && refs);
    unsigned held_refs=refs;
    task.state=TASK_RUNNING; nx_enabled=true;
    const uint64_t base=RELIEFNT_USER_BASE+0x60000;
    task.vmas[0]=(struct task_vma){.used=1,.start=base,.end=base+8192,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    assert(address_space_create(sched_task_as(&task)));
    for(unsigned j=0;j<2;++j) {
        uint64_t phys=mm_alloc_page();assert(phys);
        for(unsigned i=0;i<4096;++i)((uint8_t *)mm_phys_to_virt(phys))[i]=(uint8_t)(i*17u+j*59u);
        assert(address_space_map_user_page(sched_task_as(&task),base+j*4096,phys,RELIEFNT_PAGE_WRITABLE));
        assert(swap_try_evict(sched_task_as(&task),base+j*4096)==1);
    }
    fail_page_after=2;
    int rc=syscall_mm_swapoff((uintptr_t)"one");
    printf("live syscall swapoff partial rc=%d\n",rc);fflush(stdout);
    assert(rc==-ENOMEM && files[0].claimed && claims && refs==held_refs);
    assert(paging_user_entry_kind(sched_task_as(&task),base)==PAGING_USER_ENTRY_RESIDENT);
    assert(paging_user_entry_kind(sched_task_as(&task),base+4096)==PAGING_USER_ENTRY_SWAP);
    struct swap_stats stats;swap_get_stats(&stats);assert(stats.used==1 && stats.total==3);
    uint64_t probe=swap_slot_alloc();assert(probe);swap_slot_put(probe);
    fail_page_after=0;
    assert(!syscall_mm_swapoff((uintptr_t)"one"));
    swap_get_stats(&stats);assert(!stats.total && !stats.used);
    for(unsigned j=0;j<2;++j) {
        uint64_t phys=address_space_user_page_phys(sched_task_as(&task),base+j*4096);assert(phys);
        for(unsigned i=0;i<4096;++i)
            assert(((uint8_t *)mm_phys_to_virt(phys))[i]==(uint8_t)(i*17u+j*59u));
    }
    address_space_destroy(sched_task_as(&task));assert(!live_host_pages);empty();
    puts("PASS real live referenced swapoff syscall, partial rollback and 8192 bytes");
    return 0;
}
