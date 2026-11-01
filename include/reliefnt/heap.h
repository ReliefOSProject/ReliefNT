/*
 * ReliefOS kernel heap interface.
 * Allocates page-backed kernel memory with explicit ownership and release.
 */
#ifndef RELIEFNT_HEAP_H
#define RELIEFNT_HEAP_H

#include <reliefnt/types.h>

/** @brief Report heap initialization after physical memory becomes available.
 * @return None; pages are acquired lazily for each allocation class.
 */
void kernel_heap_init(void);
/** @brief Allocate zeroed kernel memory with at least 16-byte alignment.
 * @param size Nonzero usable payload bytes.
 * @return Owned payload, or NULL for zero, overflow, or page exhaustion.
 * IRQ/task safe and non-sleeping; small objects share pages, while larger
 * objects use contiguous physical pages. Release with kernel_free.
 */
void *kernel_malloc(size_t size);
/** @brief Bound fresh slab pages for independent equal-sized small objects.
 * @param size Object bytes, at most 2048. @param count Object count.
 * @return Upper bound assuming no existing free slots, or UINT64_MAX if the
 * request cannot use a slab or the count is unrepresentable. No lock or allocation.
 */
uint64_t kernel_heap_small_page_bound(size_t size, uint64_t count);
/** @brief Release a live allocation and its backing when no longer in use.
 * @param memory Original kernel_malloc pointer, or NULL; invalid pointers are ignored.
 * IRQ/task safe and non-sleeping; callers must exclude concurrent access to
 * the allocation and may not reuse a pointer after its lifetime ends.
 */
void kernel_free(void *memory);

#endif
