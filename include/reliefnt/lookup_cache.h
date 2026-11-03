/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_LOOKUP_CACHE_H
#define RELIEFNT_LOOKUP_CACHE_H
#include <reliefnt/types.h>

/* All operations require the kernel execution writer. Entries own only their
 * allocation: no inode, path, module or mount references, callbacks or I/O on
 * eviction. The mount must purge its weak entries before retiring its backend. */
struct vfs_lookup_cache_stats {
    uint64_t entries,bytes,budget,hits,negative_hits,misses,invalidations,evictions;
};
/** @brief Release up to budget least-recently-used name results, without I/O.
 * @param budget Maximum cache allocations to free; zero does nothing.
 * @return Number of entries freed, not a physical-page count. Task context
 * under execution ownership; provider callbacks and inode drops are forbidden.
 */
uint32_t vfs_lookup_cache_reclaim(uint32_t budget);
/** @brief Snapshot internal cache accounting without allocation or callbacks.
 * @param out Required caller-owned output. Execution writer required.
 */
void vfs_lookup_cache_stats(struct vfs_lookup_cache_stats *out);
#endif
