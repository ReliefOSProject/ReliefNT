/*
 * ReliefOS physical-memory interface: declares page allocation and reclamation.
 * Provides memory statistics and reference management for kernel mappings.
 */
#ifndef RELIEFNT_MM_H
#define RELIEFNT_MM_H

#include <reliefnt/multiboot2.h>
#include <reliefnt/types.h>

/**
 * @brief Initialize the physical-memory allocator from the boot map.
 */
void mm_init(const struct boot_info *boot);
/**
 * @brief Return total physical RAM in KiB.
 */
uint64_t mm_total_memory_kib(void);
/**
 * @brief Return currently free physical RAM in KiB.
 */
uint64_t mm_free_memory_kib(void);
/**
 * @brief Translate a supported physical address through the supervisor direct map.
 * @param phys Physical byte address in the mapped direct-map range.
 * @return Kernel pointer including the original page offset, or NULL when the
 *         physical byte is outside the addressable direct-map extent.
 */
void *mm_phys_to_virt(uint64_t phys);
/**
 * @brief Translate a supported supervisor kernel pointer to a physical address.
 * @param address Pointer within the shared direct map or linked kernel hole.
 * @return Physical byte address; zero for user pointers, other low addresses,
 *         and addresses outside the direct-map window.
 * The identity-mapped interval is exactly [KERNEL_HOLE_START, KERNEL_HOLE_END).
 */
uint64_t mm_virt_to_phys(const void *address);
/**
 * @brief Allocate one physical page; returns its physical address (0 on failure).
 */
uint64_t mm_alloc_page(void);
/**
 * @brief Allocate contiguous physical pages using the allocator hint.
 * @param page_count Requested 4 KiB pages.
 * @return Owned zeroed physical base or 0; IRQ-safe allocator lock, caller frees.
 */
uint64_t mm_alloc_pages(uint32_t page_count);
/** @brief Claim an owned zeroed DMA run entirely below an inclusive mask.
 * @param page_count Number of 4 KiB pages, nonzero and overflow checked.
 * @param mask Maximum final byte bus address.
 * @return Physical address, or 0; IRQ-safe scan/claim, no sleeping; caller frees.
 */
uint64_t mm_alloc_pages_below(uint32_t page_count, uint64_t mask);
/**
 * @brief Return one physical page (phys) to the allocator.
 */
void mm_free_page(uint64_t phys);
/**
 * @brief Return page_count contiguous physical pages starting at phys.
 */
void mm_free_pages(uint64_t phys, uint32_t page_count);
/**
 * @brief Pin a physical page so it is never returned to the free pool.
 */
void mm_retain_page(uint64_t phys);

#endif
