/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/framebuffer.h>
#include "paging_cpu_fixture.h"
#include "../../arch/x86_64/mm/paging.c"
#include "../../arch/x86_64/mm/internal.h"

static struct {uint64_t physical;void *cpu;unsigned used;} pages[128];
static int budget = -1;
static unsigned flushes, heaps;
static uint64_t direct_pdpt[512] __attribute__((aligned(4096)));
static uint64_t direct_pd[512] __attribute__((aligned(4096)));
static struct framebuffer display;
void *kernel_malloc(size_t bytes)
{
    if (!budget) return NULL;
    if (budget > 0) --budget;
    void *result = calloc(1,bytes); if (result) ++heaps; return result;
}
void kernel_free(void *memory) {assert(heaps);--heaps;free(memory);}
uint64_t mm_alloc_page(void)
{
    if (!budget) return 0;
    if (budget > 0) --budget;
    for(unsigned i=0;i<128;++i) if(!pages[i].used) {
        pages[i].physical=0x10000000ull+i*4096;
        pages[i].cpu=aligned_alloc(4096,4096);assert(pages[i].cpu);
        memset(pages[i].cpu,0,4096);pages[i].used=1;return pages[i].physical;
    }
    abort();
}
/** @brief Resolve fixture-owned physical pages to borrowed host aliases.
 * @param physical Physical fixture address. @return Host alias or NULL.
 */
void *mm_phys_to_virt(uint64_t physical)
{
    uintptr_t base=(uintptr_t)kernel_shared_pdpt;
    if(physical>=base && physical-base<sizeof(kernel_shared_pdpt))return(void *)(uintptr_t)physical;
    for(unsigned i=0;i<128;++i)if(pages[i].used && physical>=pages[i].physical && physical-pages[i].physical<4096)
        return(char *)pages[i].cpu+physical-pages[i].physical;
    return NULL;
}
/** @brief Resolve trusted fixture page tables without re-entering coverage.
 * @param physical Hosted static-table pointer or fixture-owned page address.
 * @return Borrowed table pointer, or NULL when unrecognized.
 */
uint64_t *paging_trusted_table_alias(uint64_t physical)
{
    uintptr_t first = (uintptr_t)direct_pdpt, second = (uintptr_t)direct_pd;
    if ((physical >= first && physical - first < sizeof(direct_pdpt)) ||
        (physical >= second && physical - second < sizeof(direct_pd)))
        return (void *)(uintptr_t)physical;
    return mm_phys_to_virt(physical);
}
/** @brief Classify the fixture's one-page known RAM range.
 * @param physical Physical byte start. @param bytes Checked byte count.
 * @return True only when the complete requested extent is within fixture RAM.
 */
bool mm_phys_is_ram(uint64_t physical, uint64_t bytes)
{
    const uint64_t ram = 0x500000000ULL;
    const uint64_t stolen = 6ULL << 30;
    return (physical >= ram && physical - ram < 4096 && bytes <= 4096 - (physical - ram)) ||
        (physical >= stolen && physical - stolen < 0x200000 &&
         bytes <= 0x200000 - (physical - stolen));
}
/** @brief Return the fixture's explicitly reserved display aperture.
 * @return Borrowed stable display descriptor.
 */
const struct framebuffer *framebuffer_get(void) { return &display; }
void mm_free_page(uint64_t physical)
{
    for(unsigned i=0;i<128;++i)if(pages[i].used && pages[i].physical==physical) {
        assert(pages[i].used==1);pages[i].used=0;free(pages[i].cpu);return;
    }
    abort();
}
void smp_flush_user_tlb(void) {++flushes;}
/* This fixture creates empty user branches; reaching user backing is an error. */
void mm_retain_page(uint64_t physical) {(void)physical;abort();}
int page_cache_retain(uint64_t physical) {(void)physical;abort();}
int page_cache_owns(uint64_t physical) {(void)physical;abort();}
void page_cache_release(uint64_t physical) {(void)physical;abort();}
void kernel_spin_lock(struct kernel_spinlock *lock) {assert(!lock->state);lock->state=1;}
void kernel_spin_unlock(struct kernel_spinlock *lock) {assert(lock->state);lock->state=0;}
static unsigned live_pages(void) {unsigned count=0;for(unsigned i=0;i<128;++i)count+=pages[i].used;return count;}

/** @brief Exercise sparse device mappings, sharing, aliases and rollback.
 * @return Zero after all fixture assertions succeed.
 */
int main(void)
{
    nx_enabled=true;mmio_dynamic_tables_ready=true;
    kernel_pml4[511]=(uintptr_t)kernel_shared_pdpt|3;
    kernel_pml4[256]=(uintptr_t)direct_pdpt|3;
    direct_pdpt[6]=(uintptr_t)direct_pd|3;
    direct_pd[0]=(6ULL<<30)|PAGE_SIZE_FLAG|3;
    direct_pd[1]=((6ULL<<30)+0x200000)|PAGE_SIZE_FLAG|RELIEFNT_PAGE_PWT|(1ULL<<12)|3;
    direct_pdpt[7]=(7ULL<<30)|PAGE_SIZE_FLAG|RELIEFNT_PAGE_PCD|(1ULL<<12)|3;
    uint64_t direct_cache=0;
    assert(paging_kernel_direct_cache_flags(6ULL<<30,&direct_cache) && !direct_cache);
    assert(paging_kernel_direct_cache_flags((6ULL<<30)+0x200000,&direct_cache) &&
           direct_cache==(RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PAT));
    assert(paging_kernel_direct_cache_flags(7ULL<<30,&direct_cache) &&
           direct_cache==(RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_PAT));
    assert(!paging_acquire_mmio(6ULL << 30, 4096));
    display.reservation_start = 6ULL << 30;
    display.reservation_bytes = 4096;
    assert(paging_mmio_uncached(6ULL << 30, 4096));
    assert(paging_kernel_direct_cache_flags(6ULL<<30,&direct_cache));
    assert(direct_cache==(RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD));
    assert(paging_acquire_mmio(6ULL << 30, 4096));
    paging_release_mmio(6ULL << 30, 4096);
    unsigned pinned_tables = live_pages();
    struct address_space parent={0},child={0};
    assert(address_space_create(&parent));unsigned baseline=live_pages();
    assert(parent.pml4[256] == kernel_pml4[256]);
    uint64_t physical=0xc000000000ull; /* Actual OVMF-assigned NVMe BAR location. */
    assert(!paging_ioremap_uc(physical+1,4096));
    assert(!paging_ioremap_uc(physical,0));
    assert(!paging_ioremap_uc(physical,4095));
    assert(!paging_ioremap_uc(0,4096));
    assert(!paging_ioremap_uc(0x500000000ULL-4096,3*4096)); /* Partial RAM overlap. */
    assert(!paging_ioremap_uc((6ULL<<30)-4096,8192)); /* Partial direct alias overlap. */
    assert(!paging_ioremap_uc(UINT64_MAX-4095,8192));
    assert(!paging_ioremap_uc(RELIEFNT_PHYS_ADDR_MASK+4096,4096));
    void *first=paging_ioremap_uc(physical,16384);assert(first);
    uint64_t address=(uintptr_t)first;
    assert(address>=RELIEFNT_IOREMAP_BASE && address+16384<=RELIEFNT_IOREMAP_END);
    assert(parent.pml4[511]==kernel_pml4[511]);
    assert(address_space_clone_cow(&parent,&child));assert(child.pml4[511]==parent.pml4[511]);
    assert(child.pml4[256] == parent.pml4[256]);
    for(unsigned i=0;i<4;++i) {
        uint64_t *leaf=kernel_shared_leaf(address+i*4096,false);assert(leaf);
        assert((*leaf&RELIEFNT_PHYS_ADDR_MASK)==physical+i*4096);
        assert((*leaf&(RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_NOEXEC|3))==
                      (RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_NOEXEC|3));
        assert(!(*leaf&RELIEFNT_PAGE_USER));
    }
    void *alias=paging_ioremap_uc(physical,4096);assert(alias && alias!=first);
    unsigned before=flushes;
    assert(!paging_iounmap(first,4096));assert(!paging_iounmap((char *)first+4096,12288));
    assert(paging_iounmap(first,16384));assert(flushes>before);
    assert(!paging_iounmap(first,16384));
    assert(*kernel_shared_leaf((uintptr_t)alias,false)&RELIEFNT_PAGE_PRESENT);
    assert(paging_iounmap(alias,4096));address_space_destroy(&child);
    assert(!heaps && live_pages()==baseline);
    before=flushes;
    void *large=paging_ioremap_uc(physical,4*1024*1024);assert(large);
    assert(flushes==before+1); /* One transaction, independent of BAR page count. */
    before=flushes;assert(paging_iounmap(large,4*1024*1024));
    assert(flushes==before+1 && !heaps && live_pages()==baseline);
    for(int failure=0;;++failure) {
        budget=failure;void *mapping=paging_ioremap_uc(physical,4*1024*1024);budget=-1;
        if(mapping) {assert(mapping==first);assert(paging_iounmap(mapping,4*1024*1024));break;}
        assert(!heaps && live_pages()==baseline && failure<20);
    }
    /* A retained DEVICE VMA may refer to a high page outside the direct map.
     * Match the admitted user cache bits exactly, including PAT and WB RAM. */
    const uint64_t policies[]={0,RELIEFNT_PAGE_PWT,RELIEFNT_PAGE_PCD,
        RELIEFNT_PAGE_PAT,RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD};
    for(unsigned i=0;i<sizeof(policies)/sizeof(policies[0]);++i) {
        void *device=paging_ioremap_device_page(0x500000000ULL,policies[i]);assert(device);
        uint64_t *leaf=kernel_shared_leaf((uintptr_t)device,false);assert(leaf);
        assert((*leaf&RELIEFNT_PHYS_ADDR_MASK)==0x500000000ULL);
        assert((*leaf&(RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_PAT))==policies[i]);
        assert((*leaf&(RELIEFNT_PAGE_PRESENT|RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC))==
               (RELIEFNT_PAGE_PRESENT|RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
        assert(!(*leaf&RELIEFNT_PAGE_USER));assert(paging_iounmap(device,4096));
    }
    assert(!paging_ioremap_device_page(physical,RELIEFNT_PAGE_USER));
    assert(!paging_ioremap_device_page(6ULL<<30,0));
    for(int failure=0;;++failure) {
        budget=failure;void *device=paging_ioremap_device_page(physical,RELIEFNT_PAGE_PAT);budget=-1;
        if(device){assert(paging_iounmap(device,4096));break;}
        assert(!heaps && live_pages()==baseline && failure<20);
    }
    address_space_destroy(&parent);assert(!heaps && live_pages()==pinned_tables);
    puts("PASS high PCI MMIO UC/NX, inherited/forked CR3, aliases, exact unmap, OOM/TLB/page retirement");
    return 0;
}
