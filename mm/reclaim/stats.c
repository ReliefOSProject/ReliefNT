/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/memory_stats.h>
#include <reliefnt/mm.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/paging.h>
#include <reliefnt/sched.h>
#include <reliefnt/lock.h>
#include <reliefnt/swap.h>

static uint64_t events[MEMORY_EVENT_COUNT];

/** @brief Convert physical page counts to KiB without wrapping.
 * @param pages Count of 4096-byte pages. @return Saturated KiB count.
 */
static uint64_t pages_to_kib(uint64_t pages)
{ return pages > UINT64_MAX / 4 ? UINT64_MAX : pages * 4; }

/** @brief Gather real allocator, cache and swap domains in common KiB units.
 * @param out Required result, NULL ignored. Execution ownership pins lifetime;
 * producer locks are held separately and no sleeping/provider I/O occurs.
 */
void memory_snapshot_get(struct memory_snapshot *out)
{
    if (!out) return;
    struct page_cache_report cache; struct swap_stats swap;
    page_cache_report(&cache); swap_get_stats(&swap);
    struct memory_snapshot result = {.total_kib = mm_total_memory_kib(),
        .free_kib = mm_free_memory_kib(), .cached_kib = pages_to_kib(cache.resident),
        .dirty_kib = pages_to_kib(cache.dirty), .swap_total_kib = pages_to_kib(swap.total),
        .swap_free_kib = pages_to_kib(swap.free)};
    uint64_t reserve = pages_to_kib(mm_emergency_reserve_pages());
    uint64_t free = result.free_kib > reserve ? result.free_kib - reserve : 0;
    uint64_t reclaimable = pages_to_kib(cache.reclaimable);
    result.available_kib = reclaimable > UINT64_MAX - free ? UINT64_MAX : free + reclaimable;
    if (result.available_kib > result.total_kib) result.available_kib = result.total_kib;
    *out = result;
}

/** @brief Record completed VM events without locking or allocation.
 * @param event Supported event. @param count Actual event units; invalid IDs
 * are ignored. Atomic increments are safe from IRQ and task contexts.
 */
void memory_event_add(enum memory_event event, uint64_t count)
{
    if ((unsigned)event < MEMORY_EVENT_COUNT)
        __atomic_fetch_add(&events[event], count, __ATOMIC_RELAXED);
}

/** @brief Read the current per-event atomic counters.
 * @param out Required result, NULL ignored. No locks, I/O or ownership transfer.
 */
void memory_events_get(struct memory_events *out)
{
    if (!out) return;
    for (unsigned i = 0; i < MEMORY_EVENT_COUNT; ++i)
        out->value[i] = __atomic_load_n(&events[i], __ATOMIC_RELAXED);
}
