/* Sparse, permanently bootstrap-owned physical-page metadata. */
#ifndef RELIEFNT_PHYSICAL_METADATA_INTERNAL_H
#define RELIEFNT_PHYSICAL_METADATA_INTERNAL_H

#include <reliefnt/memory_topology.h>

#define MM_METADATA_PAGES_PER_SECTION 952u
#define MM_METADATA_WORDS 15u
#define MM_PHYS_PAGE_SIZE 4096ULL
#define MM_DMA32_PFN (0x100000000ULL / MM_PHYS_PAGE_SIZE)

/** @brief One bounded run of trusted mapped RAM and its ownership state. */
struct mm_page_section {
    struct mm_page_section *next; /**< Next section in ascending PFN order. */
    struct mm_page_section *prev; /**< Previous section, or NULL. */
    uint64_t first_pfn;           /**< First physical page frame number. */
    uint32_t pages;               /**< Number of represented pages. */
    uint32_t free_count;          /**< Free bits in this section. */
    uint64_t usable[MM_METADATA_WORDS]; /**< Pages allowed into the pool. */
    uint64_t free[MM_METADATA_WORDS];   /**< Currently free usable pages. */
    uint32_t refs[MM_METADATA_PAGES_PER_SECTION]; /**< Owned reference counts. */
    struct mm_page_section *left;  /**< Lower PFN search subtree. */
    struct mm_page_section *right; /**< Higher PFN search subtree. */
};

/** @brief Prepared, permanently owned metadata list and initial accounting. */
struct mm_metadata {
    struct mm_page_section *first; /**< First sparse section, or NULL. */
    struct mm_page_section *last;  /**< Last sparse section, or NULL. */
    struct mm_page_section *root;  /**< Balanced lookup tree, or NULL. */
    uint64_t free_pages;           /**< Initially usable pages. */
    uint64_t mapped_ram_pages;     /**< Trusted mapped RAM, including reserved. */
};

/**
 * @brief Reserve all metadata from bootstrap, then seed only final usable pages.
 * @param topology Current published bootstrap view; its ranges may move during reservation.
 * @param out Unpublished output, cleared on failure.
 * @return 0 or negative errno. BSP only; no PMM, heap, lock, or reclaim calls.
 * Successful metadata stays permanently bootstrap-owned; failed claims remain reserved.
 */
int mm_metadata_prepare(const struct mm_topology *topology, struct mm_metadata *out);

/**
 * @brief Locate the section representing a physical page frame.
 * @param metadata Prepared immutable section list.
 * @param pfn 64-bit page frame number.
 * @return Borrowed section or NULL for a hole/unmapped RAM; no ownership transfer.
 */
struct mm_page_section *mm_metadata_find(const struct mm_metadata *metadata, uint64_t pfn);
#endif
