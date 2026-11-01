/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_MEMORY_STATS_H
#define RELIEFNT_MEMORY_STATS_H
#include <reliefnt/types.h>

/* KiB is explicit throughout this internal snapshot and sysinfo mem_unit ABI. */
struct memory_snapshot {
    uint64_t total_kib, free_kib, available_kib, cached_kib, dirty_kib;
    uint64_t swap_total_kib, swap_free_kib;
};
enum memory_event {
    MEMORY_EVENT_PGFAULT, MEMORY_EVENT_PGMAJFAULT, MEMORY_EVENT_PSWPIN,
    MEMORY_EVENT_PSWPOUT, MEMORY_EVENT_PGSCAN_DIRECT, MEMORY_EVENT_PGSCAN_KSWAPD,
    MEMORY_EVENT_PGSTEAL_DIRECT, MEMORY_EVENT_PGSTEAL_KSWAPD, MEMORY_EVENT_OOM_KILL,
    MEMORY_EVENT_COUNT
};
struct memory_events { uint64_t value[MEMORY_EVENT_COUNT]; };

/** @brief Snapshot physical RAM, proven cache, and usable swap in KiB.
 * @param out Required output, NULL ignored. Execution ownership pins producer
 * lifetimes; allocator, cache and swap locks are acquired separately. Physical
 * activity may change between producer snapshots. No provider I/O occurs.
 */
void memory_snapshot_get(struct memory_snapshot *out);
/** @brief Add completed real events to one atomic VM counter.
 * @param event Supported event identity. @param count Completed units.
 * IRQ-safe and allocation-free; invalid events are ignored.
 */
void memory_event_add(enum memory_event event, uint64_t count);
/** @brief Snapshot all atomic VM event counters.
 * @param out Required output, NULL ignored. Values may advance concurrently.
 */
void memory_events_get(struct memory_events *out);
#endif
