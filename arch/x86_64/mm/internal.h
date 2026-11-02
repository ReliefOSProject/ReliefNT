/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_X86_PAGING_INTERNAL_H
#define RELIEFNT_X86_PAGING_INTERNAL_H
#include <reliefnt/types.h>

/* Supervisor-only device mappings share the inherited high-half PDPT, outside
 * both physical direct mapping and the module window. No physical RAM ownership
 * is acquired: BAR owners stop the device and retire users before unmapping. */
#define RELIEFNT_IOREMAP_BASE 0xffffffb000000000ull
#define RELIEFNT_IOREMAP_END  0xffffffc000000000ull

/**
 * @brief Resolve an already owned, mapped page-table page without a coverage walk.
 * @param physical Page-aligned physical address taken from a trusted table entry.
 * @return Borrowed writable table alias; ownership remains with the table tree.
 * Never calls public direct-map validation, avoiding recursive page-table walks.
 */
uint64_t *paging_trusted_table_alias(uint64_t physical);

/** @brief Borrow the BSP kernel page-table root during early initialization.
 * @return Image-owned aligned root; BSP owns publication and lifetime.
 */
uint64_t *paging_kernel_root_table(void);
/** @brief Query whether the BSP enabled execute-disable support.
 * @return True only after successful NXE enablement.
 */
bool paging_nx_ready(void);
/** @brief Read one preexisting boot MMIO cache claim before root publication.
 * @param index Zero-based claim index. @param start Required inclusive output.
 * @param end Required exclusive output. @param cache_flags Exact PAT selector output.
 * @return True for a claim, false past the last. BSP boot context only.
 */
bool paging_boot_mmio_run(uint32_t index, uint64_t *start, uint64_t *end,
                          uint64_t *cache_flags);
/** @brief Publish new direct-map roots and retain the minimal low identity map.
 * @param branches 128 caller-owned, image-resident PDPT page arrays.
 * BSP boot context before user roots and APs; reloads CR3 after publication.
 */
void paging_publish_direct_map(uint64_t branches[128][512]);

/** @brief Install an exact high device page UC/RW/NX without a RAM alias.
 * @param address Aligned virtual page inside the reserved device window.
 * @param physical Aligned representable BAR page without a RAM/direct alias.
 * @return True or false; creates owned tables with rollback under the short map
 * lock. Caller synchronizes TLBs once before publishing the complete range.
 * Never transfers/reclaims physical device pages.
 */
bool paging_io_map_page(uint64_t address, uint64_t physical);
/** @brief Map one retained device page with the original user cache bits.
 * @param address Reserved supervisor address. @param physical Owned page.
 * @param cache_bits Only PWT, PCD and PAT from its admitted user leaf.
 * @return True on success. A physical direct-map alias must not exist.
 */
bool paging_io_map_device_page(uint64_t address,uint64_t physical,uint64_t cache_bits);
/** @brief Remove a quiescent high device extent and retire empty owned tables.
 * @param address Aligned exclusively owned device virtual base, users drained.
 * @param bytes Nonzero page-aligned full extent in the reserved device window.
 * @return True if removed, false if invalid/absent. Synchronizes all CPU TLBs
 * before freeing table pages; no device memory or RAM backing is freed.
 */
bool paging_io_unmap_range(uint64_t address, uint64_t bytes);
#endif
