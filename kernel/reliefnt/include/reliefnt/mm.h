/*
 * ReliefOS physical-memory interface: declares page allocation and reclamation.
 * Provides memory statistics and reference management for kernel mappings.
 */
#ifndef RELIEFNT_MM_H
#define RELIEFNT_MM_H

#include <reliefos/boot_handoff.h>
#include <reliefnt/multiboot2.h>
#include <reliefnt/types.h>

/**
 * @brief Initialize the physical-memory allocator from the boot map and handoff data.
 */
void mm_init(const struct boot_info *boot, const struct reliefos_boot_handoff *handoff);
/**
 * @brief Return total physical RAM in KiB.
 */
uint64_t mm_total_memory_kib(void);
/**
 * @brief Return currently free physical RAM in KiB.
 */
uint64_t mm_free_memory_kib(void);
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
