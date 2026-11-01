/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/proc_stats.h>
#include <reliefnt/export.h>

static uint64_t tmpfs_pages;
/** @brief Record committed tmpfs data-page allocations and final frees.
 * @param delta Plus/minus one owned page, with provider exclusion held.
 */
void tmpfs_account_pages(int delta)
{
    __atomic_fetch_add(&tmpfs_pages, (uint64_t)(int64_t)delta, __ATOMIC_RELAXED);
}
/** @brief Return actual live tmpfs data memory, including unlinked memfd pages.
 * @return KiB, atomic and independent of optional filesystem code lifetime.
 */
uint64_t tmpfs_memory_kib(void)
{
    return __atomic_load_n(&tmpfs_pages, __ATOMIC_RELAXED) * 4;
}
EXPORT_SYMBOL(tmpfs_account_pages);
