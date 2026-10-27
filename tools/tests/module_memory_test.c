#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/module_memory.h>
#include "../../arch/x86_64/mm/paging.c"
static struct { uint64_t phys; void *data; unsigned refs; } pages[1024];
static int fail_after=-1;
static unsigned tlb_flushes;
static size_t heaps;
void *kernel_malloc(size_t n) { if(fail_after==0) return NULL; if(fail_after>0)--fail_after; void *p=calloc(1,n); if(p)++heaps; return p; }
void kernel_free(void *p) { if(p){ assert(heaps); --heaps; free(p); } }
uint64_t mm_alloc_page(void)
{
    if(fail_after==0) return 0;
    if(fail_after>0)--fail_after;
    for(unsigned i=0;i<1024;++i) if(!pages[i].refs) {
        pages[i].data=aligned_alloc(4096,4096); assert(pages[i].data);
        memset(pages[i].data,0,4096); pages[i].refs=1;
        return pages[i].phys=0x10000000ULL+i*4096;
    }
    return 0;
}
void *mm_phys_to_virt(uint64_t p)
{
    uintptr_t start=(uintptr_t)kernel_low_pt;
    if(p>=start && p-start<sizeof(kernel_low_pt)) return (void *)(uintptr_t)p;
    start=(uintptr_t)kernel_pdpt;
    if(p>=start && p-start<sizeof(kernel_pdpt)) return (void *)(uintptr_t)p;
    for(unsigned i=0;i<1024;++i)
        if(pages[i].refs && p>=pages[i].phys && p-pages[i].phys<4096)
            return (char *)pages[i].data+(p-pages[i].phys);
    return NULL;
}
void mm_free_page(uint64_t p) { for(unsigned i=0;i<1024;++i) if(pages[i].phys==p && pages[i].refs){if(!--pages[i].refs)free(pages[i].data);return;} abort(); }
void mm_retain_page(uint64_t p) { for(unsigned i=0;i<1024;++i) if(pages[i].phys==p && pages[i].refs){++pages[i].refs;return;} abort(); }
uint64_t mm_virt_to_phys(const void *p) { (void)p; return 0; }
int page_cache_retain(uint64_t p) { (void)p; return -1; }
int page_cache_owns(uint64_t p) { (void)p; return 0; }
void page_cache_release(uint64_t p) { (void)p; abort(); }
void x86_64_load_cr3(uint64_t p) { (void)p; }
void x86_64_invlpg(uint64_t p) { (void)p; }
void smp_flush_user_tlb(void) { ++tlb_flushes; }
void kernel_spin_lock(struct kernel_spinlock *p) { assert(!p->state);p->state=1; }
void kernel_spin_unlock(struct kernel_spinlock *p) { assert(p->state);p->state=0; }
static unsigned live_pages(void) { unsigned n=0; for(unsigned i=0;i<1024;++i)n+=pages[i].refs!=0; return n; }
int main(void)
{
    nx_enabled=true; mmio_dynamic_tables_ready=true;
    kernel_pml4[511]=(uintptr_t)kernel_pdpt|3;
    for(unsigned i=0;i<512;++i){
        kernel_pd[0][i]=(uintptr_t)kernel_low_pt[i]|3;
        for(unsigned j=0;j<512;++j)kernel_low_pt[i][j]=((uint64_t)i<<21)+j*4096+3;
    }
    struct address_space parent={0},child={0};
    assert(address_space_create(&parent));
    unsigned baseline=live_pages();
    struct module_memory *m=NULL;
    assert(!module_memory_alloc(8193,8192,&m));
    assert(m && m->size==12288 && !(m->address&8191));
    assert(m->address>=RELIEFNT_MODULE_BASE && m->address+m->size<=RELIEFNT_MODULE_END);
    uint64_t *pdpt=mm_phys_to_virt(parent.pml4[511]&RELIEFNT_PHYS_ADDR_MASK);
    assert(pdpt==kernel_pdpt);
    assert(pdpt[(m->address>>30)&511]&RELIEFNT_PAGE_PRESENT);
    assert(!(pdpt[(m->address>>30)&511]&RELIEFNT_PAGE_USER));
    assert(address_space_clone_cow(&parent,&child));
    assert(child.pml4[511]==parent.pml4[511]);
    uint64_t physical=0,flags=0;
    assert(paging_module_page(m->address,&physical,&flags));
    assert(physical==m->pages[0]);
    assert((flags&(RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC))==(RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    assert(!(flags&RELIEFNT_PAGE_USER));
    unsigned flushed=tlb_flushes;
    assert(!module_memory_protect(m,MODULE_MEMORY_EXEC));
    assert(tlb_flushes>flushed);
    assert(paging_module_page(m->address,&physical,&flags));
    assert(!(flags&(RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC|RELIEFNT_PAGE_USER)));
    uint64_t alias=*mmio_existing_leaf(physical);
    assert(alias&RELIEFNT_PAGE_NOEXEC); assert(!(alias&RELIEFNT_PAGE_WRITABLE));
    assert(module_memory_protect(m,MODULE_MEMORY_WRITE|MODULE_MEMORY_EXEC)==-22);
    assert(!module_memory_protect(m,0));
    assert(paging_module_page(m->address,&physical,&flags));
    assert(flags&RELIEFNT_PAGE_NOEXEC); assert(!(flags&RELIEFNT_PAGE_WRITABLE));
    assert(!module_memory_protect(m,MODULE_MEMORY_WRITE));
    assert(*mmio_existing_leaf(physical)&RELIEFNT_PAGE_WRITABLE);
    assert(!paging_module_protect_page(m->address,RELIEFNT_PAGE_USER|RELIEFNT_PAGE_NOEXEC));
    assert(!paging_module_protect_page(m->address,RELIEFNT_PAGE_WRITABLE));
    uintptr_t old=m->address;
    module_memory_free(m);
    assert(!paging_module_page(old,&physical,&flags));
    address_space_destroy(&child);
    assert(live_pages()==baseline && !heaps);
    for(int fail=0;;++fail) {
        fail_after=fail; m=NULL;
        int rc=module_memory_alloc(16384,4096,&m);
        fail_after=-1;
        if(!rc){assert(m->address==old);module_memory_free(m);break;}
        assert(rc==-12); assert(!m && !heaps && live_pages()==baseline);
        assert(!paging_module_page(old,&physical,&flags)); assert(fail<100);
    }
    assert(module_memory_alloc(SIZE_MAX,4096,&m)==-22);
    assert(module_memory_alloc(4096,3,&m)==-22);
    address_space_destroy(&parent);
    assert(!live_pages() && !heaps);
    puts("PASS module memory: existing CR3/fork, supervisor W^X, alias, TLB, reuse, allocation rollback");
}
