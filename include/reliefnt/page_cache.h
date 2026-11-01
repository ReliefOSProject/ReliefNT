/*
 * ReliefOS file page cache interface.
 * Caches file-backed code/data pages by storage identity and page offset.
 */
#ifndef RELIEFNT_PAGE_CACHE_H
#define RELIEFNT_PAGE_CACHE_H

#include <reliefnt/storage.h>
#include <reliefnt/types.h>

/** Fill an owned frame with the complete reproducible file page; negative on failure. */
typedef int (*page_cache_loader)(uint64_t phys, void *context);

/* Pinned-cache metadata allocation failed; private callers may use their own
 * backing page. Shared mappings must retain a single cached page identity. */
#define PAGE_CACHE_FULL (-2)

/** @brief Initialize the boot-time cache before any users. */
void page_cache_init(void);
/** @brief Retain a matching resident page.
 * @param node Borrowed identity. @param offset Aligned file byte offset.
 * @param phys Required frame output. @return Zero on hit, negative on miss or invalid input.
 */
int page_cache_lookup(const struct storage_node *node, uint64_t offset,
                      uint64_t *phys);
/** @brief Load or retain a file page with transactional replacement.
 * @param node Borrowed identity. @param offset Aligned file byte offset.
 * @param loader Complete reproducible page loader; NULL permits cache hits only.
 * @param context Borrowed loader state.
 * @param phys Required frame output. @return Zero, PAGE_CACHE_FULL, -ENOMEM
 * for a frame/loader allocation failure, or another negative failure.
 * A separately committed clean reclaim after PMM exhaustion may survive a later failure.
 */
int page_cache_load(const struct storage_node *node, uint64_t offset,
                    page_cache_loader loader, void *context, uint64_t *phys);
/** @brief Load a fault page while deferring clean reclaim to the outer writer.
 * @param node Borrowed identity. @param offset Aligned file offset.
 * @param loader Complete reproducible page loader; NULL permits hits only.
 * @param context Borrowed loader state. @param phys Required frame output.
 * @return Zero or negative errno, including -ENOMEM when frame or metadata
 * allocation fails. No clean replacement or reclaim scan runs under the fault
 * and loader locks; the caller reclaims separately at its audited boundary.
 */
int page_cache_load_fault(const struct storage_node *node, uint64_t offset,
                          page_cache_loader loader, void *context, uint64_t *phys);
/** @brief Release a retained cache reference. @param phys Cache frame or zero.
 * Reaching UINT32_MAX references pins an entry permanently, as in PMM.
 */
void page_cache_release(uint64_t phys);
/** @brief Retain a resident cache frame. @param phys Aligned frame.
 * @return Zero on hit, negative otherwise; saturation pins it permanently.
 */
int page_cache_retain(uint64_t phys);
/** @brief Query cache ownership without acquiring a pin.
 * @param phys Candidate frame. @return Nonzero when resident.
 */
int page_cache_owns(uint64_t phys);
/** @brief Query a live inode's resident page identity without acquiring a pin.
 * @param node Borrowed stable identity. @return Nonzero while visible in cache.
 * Storage execution ownership serializes clustered metadata eviction and publication.
 */
int page_cache_has_inode(const struct storage_node *node);
/**
 * @brief Mark a retained cache frame potentially modified before shared write.
 * @param phys Owned aligned frame, ignored if it is not cache-owned.
 * The downgrade is permanent until a future writeback protocol exists.
 */
void page_cache_mark_shared_writable(uint64_t phys);
/**
 * @brief Snapshot the count of idle, proven reloadable, unmodified frames.
 * @return Number of physical data pages eligible at this instant; no pin is acquired.
 */
uint64_t page_cache_reclaimable_pages(void);
struct page_cache_report { uint64_t resident, reclaimable, dirty; };
/** @brief Snapshot resident, reclaimable and known-dirty cache frame counts.
 * @param out Required output, NULL ignored. Counts describe cache-owned frames,
 * not swap slots. Conservative unknown state is not counted as known dirty.
 * Incremental counters are read under the cache lock; no scan, allocation,
 * I/O, or pin is performed.
 */
void page_cache_report(struct page_cache_report *out);
/** @brief Record an actual write to a retained cache-owned frame.
 * @param phys Aligned physical frame; non-cache frames are ignored. The caller
 * owns its page reference and must have completed an actual user-visible write,
 * or admitted a shared-cache write fault before writable PTE publication.
 */
void page_cache_note_write(uint64_t phys);
/**
 * @brief Reclaim no more than the requested number of clean physical data pages.
 * @param budget_pages Maximum pages to release, with zero a no-op.
 * @return Physical data pages released. IRQ-safe, no provider I/O or heap work;
 * each frame is detached under the cache lock and returned to PMM after unlocking.
 */
uint64_t page_cache_reclaim(uint64_t budget_pages);
/**
 * @brief Inspect a bounded slice of cached entries and release clean frames.
 * @param budget Candidate slots to inspect, capped at 32.
 * @param inspected Optional output receiving the actual number inspected.
 * @param cycle_complete Optional output set when a full cache pass ends.
 * @return Number of frames actually freed to PMM, not merely detached entries.
 * The internal cursor survives batches and tracks overflow retirement under
 * the cache lock. No allocation, provider operation or sleeping is performed.
 */
uint32_t page_cache_pressure_scan(uint32_t budget, uint32_t *inspected,
                                  bool *cycle_complete);
/** @brief Inspect the same bounded cache slice and count resident candidates.
 * @param budget Maximum slots. @param inspected Work output, optional.
 * @param cycle_complete Optional completed-cycle output. @param candidates
 * Optional count of resident entries visited. @return Frames actually freed.
 * The cache lock protects each observation; no allocation or provider I/O.
 */
uint32_t page_cache_pressure_scan_count(uint32_t budget, uint32_t *inspected,
                                       bool *cycle_complete, uint32_t *candidates);
/** @brief Snapshot the shared pressure scanner's completed-cycle sequence.
 * @return Modulo-64-bit sequence, incremented under the cache lock on each
 * completed cycle including retirement of the final overflow chunk. No pin,
 * allocation, provider operation or sleeping occurs.
 */
uint64_t page_cache_pressure_epoch(void);
/**
 * @brief Drop eligible idle resident pages matching one native backing identity.
 * @param node Borrowed type/volume/inode identity, nullable for a no-op.
 * Pinned and potentially modified or unknown entries remain resident.
 */
void page_cache_invalidate_node(const struct storage_node *node);
/**
 * @brief Retire a terminally destroyed inode's cache identity.
 * @param node Exact inode identity, nullable for a no-op.
 * Caller serializes destruction and reuse with file faults. Existing physical
 * pins remain cache-owned until released; new lookups cannot see retired pages.
 */
void page_cache_forget_node(const struct storage_node *node);
/** @brief Retire a volume's cache identities at authoritative teardown.
 * @param volume Backend slot. Caller excludes volume reuse and new faults.
 * Active frame pins survive until released; no provider I/O is performed.
 */
void page_cache_forget_volume(uint32_t volume);
/**
 * @brief Apply a committed file zero/punch to cached aliases, including edges.
 * @param node Exact inode identity. @param offset Inclusive first byte.
 * @param length Nonzero range length without overflow.
 * The caller serializes file faults and destruction with the provider operation.
 * Frames are retained under the cache lock and cleared after unlocking.
 */
void page_cache_zero_range(const struct storage_node *node, uint64_t offset, uint64_t length);
/** @brief Publish a committed current file size to resident cache pages.
 * @param node Stable file identity. @param old_size Prior committed size.
 * @param new_size New committed size. Shrink zeroes newly invalid bytes;
 * growth reloads newly valid bytes when a mapping next requests them. Equal
 * sizes refresh a previously stale authoritative cap; if a resident entry's
 * prior size reveals a shrink, the newly invalid range is also zeroed.
 * Caller serializes faults and mutation; in-range dirty bytes survive.
 */
void page_cache_resize_node(const struct storage_node *node,uint64_t old_size,uint64_t new_size);
/** @brief Snapshot resident entry and hit counts.
 * @param entries Optional entry output. @param hits Optional cumulative hit output.
 */
void page_cache_stats(uint32_t *entries, uint32_t *hits);

#endif
