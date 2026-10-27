/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/paging.h>
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <reliefnt/module_memory.h>
#include <reliefnt/smp.h>

_Static_assert((RELIEFNT_IOREMAP_BASE >> 39) == (RELIEFNT_KERNEL_DIRECT_MAP_BASE >> 39),
               "device maps must inherit the kernel PDPT");
_Static_assert(((RELIEFNT_IOREMAP_END - 1) >> 39) == (RELIEFNT_IOREMAP_BASE >> 39),
               "device maps cannot cross the inherited branch");
_Static_assert(RELIEFNT_IOREMAP_BASE >= RELIEFNT_MODULE_END &&
               RELIEFNT_IOREMAP_BASE >= RELIEFNT_KERNEL_DIRECT_MAP_BASE + RELIEFNT_KERNEL_DIRECT_MAP_SIZE,
               "device maps must not overlap RAM or module windows");

struct io_mapping {
    struct io_mapping *next;
    uint64_t address, bytes;
};
static struct io_mapping *mappings;
static struct kernel_spinlock mapping_lock = KERNEL_SPINLOCK_INIT;

/** @brief Claim a distinct exact UC/NX mapping of a high physical PCI BAR extent.
 * @param physical Page-aligned validated device base beyond the RAM direct map.
 * @param bytes Nonzero page-aligned length; full physical extent must fit a PTE.
 * @return Owned supervisor pointer or NULL. Task context after allocator setup;
 * short mapping exclusion spans table allocation, never hardware/provider I/O.
 * Failed construction reclaims only its own tables/node; device pages stay owned.
 */
void *paging_ioremap_uc(uint64_t physical, uint64_t bytes)
{
    if ((physical & 4095u) || !bytes || (bytes & 4095u) ||
        physical < RELIEFNT_KERNEL_DIRECT_MAP_SIZE ||
        (physical & ~RELIEFNT_PHYS_ADDR_MASK) ||
        bytes - 1u > (RELIEFNT_PHYS_ADDR_MASK | 4095u) - physical ||
        bytes > RELIEFNT_IOREMAP_END - RELIEFNT_IOREMAP_BASE) return NULL;
    struct io_mapping *mapping = kernel_malloc(sizeof(*mapping));
    if (!mapping) return NULL;
    kernel_spin_lock(&mapping_lock);
    struct io_mapping **link = &mappings;
    uint64_t address = RELIEFNT_IOREMAP_BASE;
    while (*link) {
        if ((*link)->address - address >= bytes) break;
        address = (*link)->address + (*link)->bytes;
        link = &(*link)->next;
    }
    uint64_t mapped = 0;
    if (bytes <= RELIEFNT_IOREMAP_END - address) {
        while (mapped < bytes && paging_io_map_page(address + mapped, physical + mapped))
            mapped += 4096;
    }
    if (mapped != bytes) {
        if (mapped) (void)paging_io_unmap_range(address, mapped);
        kernel_spin_unlock(&mapping_lock);
        kernel_free(mapping);
        return NULL;
    }
    *mapping = (struct io_mapping){.next = *link, .address = address, .bytes = bytes};
    smp_flush_user_tlb(); /* Publish the entire BAR in one shared-map transaction. */
    *link = mapping;
    kernel_spin_unlock(&mapping_lock);
    return (void *)(uintptr_t)address;
}

/** @brief Retire one exact high device mapping after hardware/users quiesce.
 * @param pointer Owned base returned by paging_ioremap_uc, never a CPU RAM alias.
 * @param bytes Original complete mapped length, page aligned.
 * @return True if retired, false for wrong base/length/stale pointer. Task context;
 * all CPU translations retire before table/node release. Device memory is not freed.
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
    kernel_spin_unlock(&mapping_lock);
    kernel_free(mapping);
    return true;
}
