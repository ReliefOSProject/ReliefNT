/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/paging.h>
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <reliefnt/module_memory.h>
#include <reliefnt/smp.h>
#include <reliefnt/mm.h>

_Static_assert((RELIEFNT_IOREMAP_BASE >> 39) == (RELIEFNT_MODULE_BASE >> 39),
               "device and module maps must inherit their shared PDPT");
_Static_assert(((RELIEFNT_IOREMAP_END - 1) >> 39) == (RELIEFNT_IOREMAP_BASE >> 39),
               "device maps cannot cross the inherited branch");
_Static_assert(RELIEFNT_IOREMAP_BASE >= RELIEFNT_MODULE_END &&
               RELIEFNT_IOREMAP_BASE >= RELIEFNT_KERNEL_DIRECT_MAP_BASE + RELIEFNT_KERNEL_DIRECT_MAP_SIZE,
               "device maps must not overlap RAM or module windows");

struct io_mapping {
    struct io_mapping *next;
    uint64_t address, bytes;
    struct paging_cache_claim cache;
};
static struct io_mapping *mappings;
static struct kernel_spinlock mapping_lock = KERNEL_SPINLOCK_INIT;

/** @brief Claim a distinct supervisor mapping with the requested cache policy.
 * @param physical Page-aligned physical base outside the direct map.
 * @param bytes Nonzero page-aligned length; full physical extent must fit a PTE.
 * @param cache_bits Admitted PWT/PCD/PAT bits for every mapped leaf.
 * @return Owned supervisor pointer or NULL on invalid, incompatible or failed
 * construction. Task context after allocator setup; mapping_lock precedes the
 * MMIO policy lock, and neither spans hardware/provider I/O. The physical
 * policy is retained until unmap; rollback releases its tables and claim only.
 */
static void *paging_ioremap_owned(uint64_t physical,uint64_t bytes,uint64_t cache_bits)
{
    if ((physical & 4095u) || !bytes || (bytes & 4095u) ||
        !physical || !paging_phys_addressable(physical, bytes) ||
        bytes > RELIEFNT_IOREMAP_END - RELIEFNT_IOREMAP_BASE) return NULL;
    uint64_t first, last;
    if (paging_kernel_direct_map_next(physical, physical + bytes, &first, &last)) return NULL;
    struct io_mapping *mapping = kernel_malloc(sizeof(*mapping));
    if (!mapping) return NULL;
    *mapping = (struct io_mapping){0};
    kernel_spin_lock(&mapping_lock);
    if (!paging_cache_claim_exact(&mapping->cache,physical,bytes,cache_bits)) {
        kernel_spin_unlock(&mapping_lock);
        kernel_free(mapping);
        return NULL;
    }
    struct io_mapping **link = &mappings;
    uint64_t address = RELIEFNT_IOREMAP_BASE;
    while (*link) {
        if ((*link)->address - address >= bytes) break;
        address = (*link)->address + (*link)->bytes;
        link = &(*link)->next;
    }
    uint64_t mapped = 0;
    if (bytes <= RELIEFNT_IOREMAP_END - address) {
        while (mapped < bytes && paging_io_map_device_page(address + mapped, physical + mapped,cache_bits))
            mapped += 4096;
    }
    if (mapped != bytes) {
        if (mapped) (void)paging_io_unmap_range(address, mapped);
        paging_cache_claim_release(&mapping->cache);
        kernel_spin_unlock(&mapping_lock);
        kernel_free(mapping);
        return NULL;
    }
    mapping->next = *link;
    mapping->address = address;
    mapping->bytes = bytes;
    smp_flush_user_tlb(); /* Publish the entire BAR in one shared-map transaction. */
    *link = mapping;
    kernel_spin_unlock(&mapping_lock);
    return (void *)(uintptr_t)address;
}

/** @brief Map an unowned PCI BAR uncached, rejecting known RAM pages.
 * @param physical Page-aligned base. @param bytes Complete page-aligned extent.
 * @return Owned supervisor mapping or NULL on conflict/failure; the caller
 * retains device backing and the mapping owns its physical cache policy.
 */
void *paging_ioremap_uc(uint64_t physical,uint64_t bytes)
{
    if ((physical & 4095u) || !bytes || (bytes & 4095u) ||
        !physical || !paging_phys_addressable(physical,bytes))return NULL;
    /* BARs may not manufacture an uncoordinated alias of any known RAM. */
    for(uint64_t offset=0;offset<bytes;offset+=4096)
        if(mm_phys_is_ram(physical+offset,4096))return NULL;
    return paging_ioremap_owned(physical,bytes,RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD);
}

/** @brief Map one retained device page using the existing user cache policy.
 * @param physical Aligned page outside the direct map. @param cache_bits PTE policy.
 * @return Owned supervisor alias or NULL on conflict/failure; caller retires it
 * before backing release, after which its cache-policy claim is consumed.
 */
void *paging_ioremap_device_page(uint64_t physical,uint64_t cache_bits)
{
    const uint64_t mask=RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_PAT;
    if(cache_bits&~mask)return NULL;
    return paging_ioremap_owned(physical,4096,cache_bits);
}

/** @brief Retire one exact high device mapping after hardware/users quiesce.
 * @param pointer Owned base returned by a high PCI or DEVICE mapping helper.
 * @param bytes Original complete mapped length, page aligned.
 * @return True if retired, false for wrong base/length/stale pointer. Task context;
 * all CPU translations retire before table/node and policy release. Physical
 * device or RAM backing is never freed by this operation.
 */
bool paging_iounmap(void *pointer, uint64_t bytes)
{
    kernel_spin_lock(&mapping_lock);
    struct io_mapping **link = &mappings;
    while (*link && (*link)->address != (uintptr_t)pointer) link = &(*link)->next;
    struct io_mapping *mapping = *link;
    if (!mapping || mapping->bytes != bytes) {
        kernel_spin_unlock(&mapping_lock);
        return false;
    }
    if (!paging_io_unmap_range(mapping->address, bytes)) {
        kernel_spin_unlock(&mapping_lock);
        return false;
    }
    *link = mapping->next;
    paging_cache_claim_release(&mapping->cache);
    kernel_spin_unlock(&mapping_lock);
    kernel_free(mapping);
    return true;
}
