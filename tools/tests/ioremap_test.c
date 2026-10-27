/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../arch/x86_64/mm/paging.c"
#include "../../arch/x86_64/mm/internal.h"

static struct {uint64_t physical;void *cpu;unsigned used;} pages[128];
static int budget = -1;
static unsigned flushes, heaps;
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
void *mm_phys_to_virt(uint64_t physical)
{
    uintptr_t base=(uintptr_t)kernel_pdpt;
    if(physical>=base && physical-base<sizeof(kernel_pdpt))return(void *)(uintptr_t)physical;
    for(unsigned i=0;i<128;++i)if(pages[i].used && physical>=pages[i].physical && physical-pages[i].physical<4096)
        return(char *)pages[i].cpu+physical-pages[i].physical;
    return NULL;
}
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

int main(void)
{
    nx_enabled=true;mmio_dynamic_tables_ready=true;
    kernel_pml4[511]=(uintptr_t)kernel_pdpt|3;
    struct address_space parent={0},child={0};
    assert(address_space_create(&parent));unsigned baseline=live_pages();
    uint64_t physical=0xc000000000ull; /* Actual OVMF-assigned NVMe BAR location. */
    assert(!paging_ioremap_uc(physical+1,4096));
    assert(!paging_ioremap_uc(physical,0));
    assert(!paging_ioremap_uc(physical,4095));
    assert(!paging_ioremap_uc(0,4096));
    assert(!paging_ioremap_uc(RELIEFNT_KERNEL_DIRECT_MAP_SIZE-4096,8192));
    assert(!paging_ioremap_uc(UINT64_MAX-4095,8192));
    assert(!paging_ioremap_uc(RELIEFNT_PHYS_ADDR_MASK+4096,4096));
    void *first=paging_ioremap_uc(physical,16384);assert(first);
    uint64_t address=(uintptr_t)first;
    assert(address>=RELIEFNT_IOREMAP_BASE && address+16384<=RELIEFNT_IOREMAP_END);
    assert(parent.pml4[511]==kernel_pml4[511]);
    assert(address_space_clone_cow(&parent,&child));assert(child.pml4[511]==parent.pml4[511]);
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
    address_space_destroy(&parent);assert(!heaps && !live_pages());
    puts("PASS high PCI MMIO UC/NX, inherited/forked CR3, aliases, exact unmap, OOM/TLB/page retirement");
}
