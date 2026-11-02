/* SPDX-License-Identifier: Apache-2.0 */
#define RELIEFNT_PAGING_FIXTURE_EXTERNAL_RAM
#define main inherited_paging_fixture_main
#include "paging_protection_test.c"
#undef main
static bool pat_ready=true, ordinary_ram;
static unsigned flushes;
static uint64_t irq=0x200;
uint64_t mm_free_memory_kib(void){return 1024*1024;}
bool arch_pat_writecombine_ready(void){return pat_ready;}
void arch_pat_flush_boot(void){assert(!irq);++flushes;}
uint64_t kernel_irq_save(void){uint64_t f=irq;irq=0;return f;}
void kernel_irq_restore(uint64_t f){irq=f;}
bool mm_phys_is_ram(uint64_t physical,uint64_t bytes)
{(void)physical;(void)bytes;return ordinary_ram;}
int main(int argc,char **argv)
{
    assert(argc==2);
    const uint64_t base=0x80000, target=base+4096, bytes=8192;
    nx_enabled=true;
    kernel_pml4[256]=(uintptr_t)kernel_pdpt|3;
    kernel_pdpt[0]=(uintptr_t)kernel_pd[0]|3;
    kernel_pd[0][0]=3|PAGE_SIZE_FLAG;
    assert(paging_mmio_uncached(base,4*4096));
    uint64_t original=*mmio_existing_leaf(target);
    struct paging_cache_claim alias={0};
    bool success=!strcmp(argv[1],"normal");
    if(!strcmp(argv[1],"lease")) assert(paging_acquire_mmio(target,4096));
    else if(!strcmp(argv[1],"alias")) {
        uint64_t flags=0;
        assert(paging_cache_claim_select(&alias,target,&flags));
        assert(flags==(RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD));
    }
    else if(!strcmp(argv[1],"ram")) ordinary_ram=true;
    else if(!strcmp(argv[1],"unsupported")) pat_ready=false;
    else if(!strcmp(argv[1],"users")) {
        struct address_space as={0};assert(address_space_create(&as));address_space_destroy(&as);
    } else if(!strcmp(argv[1],"malformed")) {
        assert(!paging_mmio_writecombine_boot(target+1,bytes));
    }
    bool result=paging_mmio_writecombine_boot(target,bytes);
    assert(result==success && irq==0x200);
    if(success) {
        assert(flushes==2 && paging_mmio_writecombined(target,bytes));
        assert(!paging_mmio_writecombined(base,4096));
        assert(!paging_mmio_writecombined(target,bytes+4096));
        for(uint64_t p=target;p<target+bytes;p+=4096) {
            uint64_t flags=0;assert(paging_kernel_direct_cache_flags(p,&flags));
            assert(flags==RELIEFNT_PAGE_PAT);
            assert((*mmio_existing_leaf(p)&(RELIEFNT_PAGE_PAT|RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_PWT))==RELIEFNT_PAGE_PAT);
            assert(kernel_low_pt[0][p>>12]==*mmio_existing_leaf(p));
        }
        assert((*mmio_existing_leaf(base)&(RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD))==(RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD));
        assert((*mmio_existing_leaf(base+3*4096)&(RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD))==(RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD));
        assert(!paging_acquire_mmio(target,4096));
        assert(!paging_cache_claim_exact(&alias,target,4096,0));
        assert(!paging_cache_claim_exact(&alias,target,4096,RELIEFNT_PAGE_PAT));
        uint64_t selected=0;
        assert(paging_cache_claim_select(&alias,target,&selected) && selected==RELIEFNT_PAGE_PAT);
        paging_cache_claim_release(&alias);
        assert(paging_mmio_writecombined(target,bytes));
    } else assert(*mmio_existing_leaf(target)==original && !flushes);
    assert(!paging_mmio_writecombine_boot(target,bytes));
    printf("PASS boot WC cache policy, alias and admission: %s\n",argv[1]);
    return 0;
}
