/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_X86_PAGING_INTERNAL_H
#define RELIEFNT_X86_PAGING_INTERNAL_H
#include <reliefnt/types.h>

/* Supervisor-only device mappings share the inherited high-half PDPT, outside
 * both physical direct mapping and the module window. No physical RAM ownership
 * is acquired: BAR owners stop the device and retire users before unmapping. */
#define RELIEFNT_IOREMAP_BASE 0xffffffb000000000ull
#define RELIEFNT_IOREMAP_END  0xffffffc000000000ull

/** @brief Install an exact high device page UC/RW/NX without a RAM alias.
 * @param address Aligned virtual page inside the reserved device window.
 * @param physical Aligned representable BAR page beyond the physical direct map.
 * @return True or false; creates owned tables with rollback under the short map
 * lock. Caller synchronizes TLBs once before publishing the complete range.
 * Never transfers/reclaims physical device pages.
 */
bool paging_io_map_page(uint64_t address, uint64_t physical);
/** @brief Remove a quiescent high device extent and retire empty owned tables.
 * @param address Aligned exclusively owned device virtual base, users drained.
 * @param bytes Nonzero page-aligned full extent in the reserved device window.
 * @return True if removed, false if invalid/absent. Synchronizes all CPU TLBs
 * before freeing table pages; no device memory or RAM backing is freed.
 */
bool paging_io_unmap_range(uint64_t address, uint64_t bytes);
#endif
