/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/module_memory.h>
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <reliefnt/mm.h>
#include <reliefnt/paging.h>
#include <linux/errno.h>

#define MODULE_PAGE_SIZE 4096u
static struct module_memory *regions;
static struct kernel_spinlock regions_lock=KERNEL_SPINLOCK_INIT;

/** @brief Find and reserve an aligned virtual gap without running callbacks under lock.
 * @param memory Metadata allocated by caller; receives an owned list reservation.
 * @param alignment Power-of-two alignment at least one page.
 * @return True when reserved, false for exhausted virtual space.
 */
static bool reserve_region(struct module_memory *memory,size_t alignment)
{
    uintptr_t address=RELIEFNT_MODULE_BASE;
    struct module_memory **at=&regions;
    kernel_spin_lock(&regions_lock);
    for(;;) {
        if(address>UINTPTR_MAX-(alignment-1)) break;
        address=(address+alignment-1)&~(uintptr_t)(alignment-1);
        if(address>RELIEFNT_MODULE_END || memory->size>RELIEFNT_MODULE_END-address) break;
        if(!*at || (address<=(*at)->address && memory->size<=(*at)->address-address)) {
            memory->address=address; memory->next=*at; *at=memory;
            kernel_spin_unlock(&regions_lock); return true;
        }
        address=(*at)->address+(*at)->size; at=&(*at)->next;
    }
    kernel_spin_unlock(&regions_lock); return false;
}
/** @brief Unmap an exclusively owned/quiescent allocation before recycling its virtual reservation.
 * @param memory Owned allocation or NULL; execution and external users must already be drained.
 * Task context; page/TLB operations occur outside the short reservation-list lock.
 */
void module_memory_free(struct module_memory *memory)
{
    if(!memory) return;
    for(size_t i=0;i<memory->page_count;++i) {
        paging_module_unmap_page(memory->address+i*MODULE_PAGE_SIZE,memory->alias_flags[i]);
        mm_free_page(memory->pages[i]);
    }
    if(memory->address) {
        kernel_spin_lock(&regions_lock);
        struct module_memory **at=&regions;
        while(*at && *at!=memory) at=&(*at)->next;
        if(*at) *at=memory->next;
        kernel_spin_unlock(&regions_lock);
    }
    kernel_free(memory->pages); kernel_free(memory->alias_flags); kernel_free(memory);
}
/** @brief Allocate private zeroed backing pages in the shared supervisor module window.
 * @param size Nonzero byte count rounded to page size with overflow checks.
 * @param alignment Power-of-two virtual alignment, zero means unspecified.
 * @param out Required owned result; reset to NULL before any failure.
 * @return Zero or -EINVAL/-ENOMEM. Task context; physical pages may be discontiguous.
 */
int module_memory_alloc(size_t size,size_t alignment,struct module_memory **out)
{
    if(!out) return -EINVAL;
    *out=NULL;
    if(!size || size>SIZE_MAX-(MODULE_PAGE_SIZE-1) ||
       (alignment && (alignment&(alignment-1)))) return -EINVAL;
    size=(size+MODULE_PAGE_SIZE-1)&~(size_t)(MODULE_PAGE_SIZE-1);
    if(size>RELIEFNT_MODULE_END-RELIEFNT_MODULE_BASE ||
       alignment>RELIEFNT_MODULE_END-RELIEFNT_MODULE_BASE) return -ENOMEM;
    if(alignment<MODULE_PAGE_SIZE) alignment=MODULE_PAGE_SIZE;
    size_t count=size/MODULE_PAGE_SIZE;
    if(count>SIZE_MAX/sizeof(uint64_t)) return -ENOMEM;
    struct module_memory *memory=kernel_malloc(sizeof(*memory));
    if(!memory) return -ENOMEM;
    *memory=(struct module_memory){.size=size};
    memory->pages=kernel_malloc(count*sizeof(*memory->pages));
    memory->alias_flags=kernel_malloc(count*sizeof(*memory->alias_flags));
    if(!memory->pages || !memory->alias_flags || !reserve_region(memory,alignment)) goto nomemory;
    for(size_t i=0;i<count;++i) {
        uint64_t page=mm_alloc_page();
        if(!page) goto nomemory;
        if(!paging_module_map_page(memory->address+i*MODULE_PAGE_SIZE,page,&memory->alias_flags[i])) {
            mm_free_page(page); goto nomemory;
        }
        memory->pages[i]=page; ++memory->page_count;
    }
    *out=memory; return 0;
nomemory:
    module_memory_free(memory); return -ENOMEM;
}
/** @brief Seal a private region and its physical aliases with per-CPU synchronization.
 * @param memory Caller-owned region; no concurrent mutation or execution permitted.
 * @param permissions Zero for RO/NX, WRITE for RW/NX, EXEC for RX.
 * @return Zero or -EINVAL/-EFAULT. W+X and unknown flags are rejected before changes.
 */
int module_memory_protect(struct module_memory *memory,unsigned int permissions)
{
    if(!memory || permissions&~(MODULE_MEMORY_WRITE|MODULE_MEMORY_EXEC) ||
       permissions==(MODULE_MEMORY_WRITE|MODULE_MEMORY_EXEC)) return -EINVAL;
    uint64_t flags=permissions&MODULE_MEMORY_EXEC ? 0 : RELIEFNT_PAGE_NOEXEC;
    if(permissions&MODULE_MEMORY_WRITE) flags|=RELIEFNT_PAGE_WRITABLE;
    for(size_t i=0;i<memory->page_count;++i)
        if(!paging_module_page(memory->address+i*MODULE_PAGE_SIZE,NULL,NULL)) return -EFAULT;
    for(size_t i=0;i<memory->page_count;++i)
        if(!paging_module_protect_page(memory->address+i*MODULE_PAGE_SIZE,flags)) return -EFAULT;
    return 0;
}
