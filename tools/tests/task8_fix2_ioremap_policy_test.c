/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Exercise real PCI leases, DEVICE backing and cache ownership.
 * Host physical pages, provider references, MMIO access and CPU/TLB are modeled.
 */
#define main task8_fix2_inherited_ioremap_main
#include "ioremap_test.c"
#undef main
#include "../../arch/x86_64/platform/pci.c"
#include <reliefnt/device_mapping.h>
#include <reliefnt/sched.h>
#include <linux/errno.h>

/** @brief Model local TLB invalidation without executing privileged CPU code.
 * @param address Invalidated fixture virtual page.
 */
void x86_64_invlpg(uint64_t address) { (void)address; }

#ifdef CONFIG_MODULES
static struct module fixture_module;
static unsigned module_refs;
/** @brief Retain the fixture's modeled provider code. @param owner Exact module.
 * @return True after one reference is acquired.
 */
bool try_module_get(struct module *owner)
{ assert(owner==&fixture_module);++module_refs;return true; }
/** @brief Release one modeled provider-code reference. @param owner Exact module. */
void module_put(struct module *owner)
{ assert(owner==&fixture_module && module_refs);--module_refs; }
#define FIXTURE_OWNER (&fixture_module)
#else
#define FIXTURE_OWNER NULL
#endif

/** @brief Return the inline VMA capacity of the fixture task.
 * @param task Borrowed task. @return Number of inline slots.
 */
uint32_t sched_task_vma_capacity(const struct task *task)
{ assert(task);return SCHED_TASK_VMA_MAX; }
/** @brief Borrow one fixture VMA under modeled execution ownership.
 * @param task Owned task. @param index Inline index. @return Stable slot.
 */
struct task_vma *sched_task_vma_at(struct task *task,uint32_t index)
{ assert(task && index<SCHED_TASK_VMA_MAX);return &sched_task_mm(task)->vmas[index]; }

struct fixture_device { uint64_t physical,policy; unsigned releases; };
/** @brief Check policy still held during the final provider callback.
 * @param context Retained fixture backing.
 */
static void provider_release(void *context)
{
    struct fixture_device *dev=context;
    uint64_t other=dev->policy ? 0 : RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD;
    assert(!paging_ioremap_device_page(dev->physical,other));
#ifdef CONFIG_MODULES
    assert(module_refs==1);
#endif
    ++dev->releases;
}
/** @brief Resolve one physical page within the modeled retained backing.
 * @param context Fixture backing. @param offset Aligned page offset.
 * @return Original physical page.
 */
static uint64_t provider_physical(void *context,uint64_t offset)
{ struct fixture_device *dev=context;return dev->physical+offset; }
static const struct device_mapping_operations device_ops={
    .release=provider_release,.physical=provider_physical};

/** @brief Require physical-cache compatibility across real PCI/DEVICE owners.
 * @return Zero after mappings, policy claims, references and tables retire.
 */
int main(void)
{
    nx_enabled=true;mmio_dynamic_tables_ready=true;
    kernel_pml4[511]=(uintptr_t)kernel_shared_pdpt|3;
    kernel_pml4[256]=(uintptr_t)direct_pdpt|3;
    const uint64_t physical=0xc000000000ULL;
    const uint64_t uc=RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD;
    const uint64_t cache_mask=uc|RELIEFNT_PAGE_PAT;
    struct fixture_device dev={.physical=physical,.policy=uc};
    struct device_mapping_info info={.length=8192,.max_prot=3,.can_split=true,.can_move=true};
    struct device_mapping *backing=NULL;
    struct task *task=calloc(1,sizeof(*task));assert(task);task->kind=TASK_KIND_USER;
    assert(address_space_create(sched_task_as(task)));
    unsigned baseline=live_pages();

    /* The actual PCI lease establishes UC for both pages. */
    void *lease=pci_map_mmio_lease(physical,8192);assert(lease);
    void *incompatible=paging_ioremap_device_page(physical+4096,0);
    assert(!incompatible);
    void *compatible=paging_ioremap_device_page(physical+4096,uc);assert(compatible);
    assert(!device_mapping_create(FIXTURE_OWNER,&device_ops,&dev,&info,&backing));
    uint64_t flags=UINT64_MAX;
    budget=0;
    assert(device_mapping_cache_page(backing,0,physical,&flags)==-ENOMEM);
    budget=-1;assert(flags==UINT64_MAX);
    assert(!device_mapping_cache_page(backing,0,physical,&flags) && flags==uc);
    unsigned held_heaps=heaps;
    budget=0;assert(device_mapping_cache_page(backing,4096,physical+4096,&flags)==-ENOMEM);
    budget=-1;assert(heaps==held_heaps);
    assert(!device_mapping_cache_page(backing,4096,physical+4096,&flags) && flags==uc);
    assert(heaps==held_heaps); /* Adjacent same-policy pages coalesce. */
    const uint64_t user=RELIEFNT_USER_BASE+0x400000;
    for(unsigned i=0;i<2;++i)
        assert(address_space_map_user_page(sched_task_as(task),user+i*4096,
            physical+i*4096,RELIEFNT_PAGE_DEVICE|RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC|uc));
    task->vmas[0]=(struct task_vma){.used=1,.start=user,.end=user+8192,
        .flags=TASK_VMA_FLAG_DEVICE,.prot=3,.max_prot=3,.device_mapping=backing};
    assert((address_space_user_page_flags(sched_task_as(task),user)&cache_mask)==uc);
    struct device_user_page stage={0};
    assert(!device_user_page_get(task,user,physical,
                                address_space_user_page_flags(sched_task_as(task),user),&stage));
    assert(!device_mapping_get(backing)); /* A second VMA keeps the backing. */
    assert(stage.temporary && stage.mapping==backing);
    uint64_t *stage_leaf=kernel_shared_leaf((uintptr_t)stage.alias,false);
    assert(stage_leaf && (*stage_leaf&cache_mask)==uc);
    pci_unmap_mmio(lease,8192); /* Last PCI lease closes first. */
    assert(!paging_ioremap_device_page(physical,0));
    assert(paging_iounmap(compatible,4096));
    assert(address_space_unmap_user_page(sched_task_as(task),user)==physical);
    assert(address_space_unmap_user_page(sched_task_as(task),user+4096)==physical+4096);
    task->vmas[0]=(struct task_vma){0};device_mapping_put(backing);
    assert(!dev.releases && !paging_ioremap_device_page(physical,0));
    device_user_page_put(&stage); /* Alias first; the second VMA still owns policy. */
    assert(!dev.releases);
    unsigned failed_aliases=0;
    for(int failure=0;;++failure) {
        unsigned before_heaps=heaps,before_pages=live_pages();
        budget=failure;void *retry=paging_ioremap_device_page(physical,uc);budget=-1;
        if(retry){assert(paging_iounmap(retry,4096));break;}
        assert(heaps==before_heaps && live_pages()==before_pages && failure<12);
        assert(!paging_ioremap_device_page(physical,0));++failed_aliases;
    }
    assert(failed_aliases>=3); /* Owner-node, PD and PT allocation failures. */
    device_mapping_put(backing);
    assert(dev.releases==1);
#ifdef CONFIG_MODULES
    assert(!module_refs);
#endif
    void *later=paging_ioremap_device_page(physical,0);assert(later);
    assert(paging_iounmap(later,4096));

    /* DEVICE first: WB remains authoritative; an overlapping UC PCI lease
     * cannot retag the user page, while a compatible alias may coexist. */
    dev=(struct fixture_device){.physical=physical+0x100000,.policy=0};backing=NULL;
    assert(!device_mapping_create(FIXTURE_OWNER,&device_ops,&dev,&info,&backing));
    assert(!device_mapping_cache_page(backing,0,dev.physical,&flags) && !flags);
    assert(!pci_map_mmio_lease(dev.physical,4096));
    void *wb=paging_ioremap_device_page(dev.physical,0);assert(wb);
    assert(!paging_ioremap_uc(dev.physical-4096,8192)); /* Partial overlap. */
    assert(paging_iounmap(wb,4096));device_mapping_put(backing);assert(dev.releases==1);
    lease=pci_map_mmio_lease(dev.physical,4096);assert(lease);pci_unmap_mmio(lease,4096);

    /* Direct-map policy cannot be changed under a DEVICE owner. Conversely,
     * last PCI close defers restoration until its UC user owner disappears. */
    direct_pdpt[6]=(uintptr_t)direct_pd|3;
    const uint64_t low=(6ULL<<30)+0x400000;
    direct_pd[2]=low|PAGE_SIZE_FLAG|3;
    struct paging_cache_claim low_claim={0};
    assert(paging_cache_claim_select(&low_claim,low,&flags) && !flags);
    assert(!pci_map_mmio_lease(low,8192));
    assert(paging_kernel_direct_cache_flags(low,&flags) && !flags);
    paging_cache_claim_release(&low_claim);
    lease=pci_map_mmio_lease(low,8192);assert(lease);
    assert(paging_cache_claim_select(&low_claim,low,&flags) && flags==uc);
    pci_unmap_mmio(lease,8192);
    assert(paging_kernel_direct_cache_flags(low,&flags) && flags==uc);
    /* The adjacent page has no retained owner: it must be restored before
     * A's independent DEVICE claim is released. */
    assert(paging_kernel_direct_cache_flags(low+4096,&flags) && !flags);
    assert(paging_mmio_writeback_nx(low+4096,4096));
    assert(paging_kernel_direct_cache_flags(low,&flags) && flags==uc);
    assert(paging_kernel_direct_map(low));
    assert((*mmio_existing_leaf(low)&(RELIEFNT_PAGE_PRESENT|RELIEFNT_PAGE_WRITABLE|uc))==
           (RELIEFNT_PAGE_PRESENT|RELIEFNT_PAGE_WRITABLE|uc));
    paging_cache_claim_release(&low_claim);
    assert(paging_kernel_direct_cache_flags(low,&flags) && !flags);
    assert(paging_kernel_direct_cache_flags(low+4096,&flags) && !flags);

    /* Two independently leased ranges, disjoint and duplicate DEVICE owners,
     * and a same-policy PCI reacquisition must restore each page exactly. */
    const uint64_t span=low+0x10000;
    uint64_t originals[64];
    for(unsigned i=0;i<64;++i)originals[i]=*mmio_existing_leaf(span+i*4096);
    unsigned prior_runs=mmio_run_count;
    void *wide=pci_map_mmio_lease(span,64*4096);assert(wide);
    void *middle=pci_map_mmio_lease(span+20*4096,16*4096);assert(middle);
    struct paging_cache_claim held[4]={{0}};
    const unsigned indices[4]={2,24,24,50};
    for(unsigned i=0;i<4;++i)
        assert(paging_cache_claim_select(&held[i],span+indices[i]*4096,&flags) && flags==uc);
    unsigned before_flush=flushes;
    pci_unmap_mmio(wide,64*4096);assert(flushes>before_flush);
    assert(paging_kernel_direct_cache_flags(span+4096,&flags) && !flags);
    assert(paging_mmio_writeback_nx(span+4096,4096));
    assert(paging_kernel_direct_cache_flags(span+24*4096,&flags) && flags==uc);
    pci_unmap_mmio(middle,16*4096);
    for(unsigned i=0;i<64;++i) {
        if(i==1 || i==2 || i==24 || i==50)continue;
        assert(*mmio_existing_leaf(span+i*4096)==originals[i]);
    }
    assert(mmio_run_count==prior_runs+1); /* Only the new permanent WB/NX page. */
    before_flush=flushes;
    paging_cache_claim_release(&held[0]);assert(flushes>before_flush);
    assert(*mmio_existing_leaf(span+2*4096)==originals[2]);
    paging_cache_claim_release(&held[1]);
    assert(paging_kernel_direct_cache_flags(span+24*4096,&flags) && flags==uc);
    void *again=pci_map_mmio_lease(span+24*4096,4096);assert(again);
    paging_cache_claim_release(&held[2]);
    assert(paging_kernel_direct_cache_flags(span+24*4096,&flags) && flags==uc);
    pci_unmap_mmio(again,4096);
    assert(*mmio_existing_leaf(span+24*4096)==originals[24]);
    paging_cache_claim_release(&held[3]);
    assert(*mmio_existing_leaf(span+50*4096)==originals[50]);

    /* More disjoint live owners than the old run-table capacity must not
     * force last-lease release to retain unrelated pages or consume run slots. */
    const uint64_t large=low+0x80000;
    const uint64_t next_pde=(low+0x200000)|PAGE_SIZE_FLAG|3;
    direct_pd[3]=next_pde;
    struct paging_cache_claim disjoint[260]={{0}};
    uint64_t large_original=*mmio_existing_leaf(large);
    prior_runs=mmio_run_count;
    wide=pci_map_mmio_lease(large,520*4096);assert(wide);
    for(unsigned i=0;i<260;++i)
        assert(paging_cache_claim_select(&disjoint[i],large+2*i*4096,&flags) && flags==uc);
    pci_unmap_mmio(wide,520*4096);
    assert(mmio_run_count==prior_runs);
    for(unsigned i=0;i<260;++i) {
        assert(paging_kernel_direct_cache_flags(large+(2*i+1)*4096,&flags) && !flags);
        assert(paging_kernel_direct_cache_flags(large+2*i*4096,&flags) && flags==uc);
    }
    for(unsigned i=260;i>0;--i)paging_cache_claim_release(&disjoint[i-1]);
    assert(*mmio_existing_leaf(large)==large_original);
    assert(mmio_run_count==prior_runs);
    /* Fixture-only retirement of the newly split, synthetic second PDE keeps
     * the inherited allocator accounting assertion unchanged. Production keeps
     * its shared split table for the lifetime of the kernel. */
    for(unsigned i=0;i<512;++i)
        assert(*mmio_existing_leaf(low+0x200000+(uint64_t)i*4096)==
               ((low+0x200000+(uint64_t)i*4096)|3));
    uint64_t synthetic_pt=direct_pd[3]&RELIEFNT_PHYS_ADDR_MASK;
    direct_pd[3]=next_pde;smp_flush_user_tlb();mm_free_page(synthetic_pt);

    address_space_destroy(sched_task_as(task));free(task);
    assert(!heaps && live_pages()<=baseline+1); /* Direct split remains owned. */
    puts("PASS real PCI/DEVICE cache ownership, reverse order, lifetime and rollback");
    return 0;
}
