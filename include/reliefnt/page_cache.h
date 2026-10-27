/*
 * ReliefOS read-only page cache interface.
 * Caches file-backed code/data pages by storage identity and page offset.
 */
#ifndef RELIEFNT_PAGE_CACHE_H
#define RELIEFNT_PAGE_CACHE_H

#include <reliefnt/storage.h>
#include <reliefnt/types.h>

typedef int (*page_cache_loader)(uint64_t phys, void *context);

/* Pinned-cache metadata allocation failed; private callers may use their own
 * backing page. Shared mappings must retain a single cached page identity. */
#define PAGE_CACHE_FULL (-2)

void page_cache_init(void);
int page_cache_lookup(const struct storage_node *node, uint64_t offset,
                      uint64_t *phys);
int page_cache_load(const struct storage_node *node, uint64_t offset,
                    page_cache_loader loader, void *context, uint64_t *phys);
void page_cache_release(uint64_t phys);
int page_cache_retain(uint64_t phys);
int page_cache_owns(uint64_t phys);
/**
 * @brief Drop unreferenced resident pages matching one native backing identity.
 * @param node Borrowed stable type/volume/inode identity, nullable for a no-op.
 * @par Context
 * Acquires the IRQ-safe cache lock and releases only unpinned resident pages. Pinned and overflow entries survive until their normal final release; no file/provider callback or user access.
 */
void page_cache_invalidate_node(const struct storage_node *node);
void page_cache_stats(uint32_t *entries, uint32_t *hits);

#endif
