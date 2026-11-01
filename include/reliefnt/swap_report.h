/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SWAP_REPORT_H
#define RELIEFNT_SWAP_REPORT_H
#include <reliefnt/mount.h>
#include <reliefnt/types.h>

struct swap_report_row {
    uint64_t total, used; /* usable pages, including a draining area's in-use slots */
    int32_t priority;
    bool raw;
    char path[VFS_PATH_MAX + sizeof(" (deleted)")];
};
struct swap_report { size_t count; struct swap_report_row rows[]; };

#ifdef CONFIG_SWAP
/** @brief Capture active and draining areas with their current retained paths.
 * @param out Required empty output receiving an owned snapshot, possibly NULL
 * when there are no entries. @return Zero or negative errno without a partial
 * snapshot. Execution writer pins the backing list and retained paths until
 * formatting ends; each area's slot counters are copied under its metadata lock.
 */
int swap_report_capture(struct swap_report **out);
/** @brief Free an owned snapshot, nullable. No backend or path pins remain. */
void swap_report_free(struct swap_report *report);
#else
/** @brief Disabled swap has no retained area entries. @param out Empty result.
 * @return Zero, or EINVAL for a missing output.
 */
static inline int swap_report_capture(struct swap_report **out)
{ if (!out) return -22; *out = NULL; return 0; }
/** @brief Disabled swap has no snapshot storage. @param report Ignored. */
static inline void swap_report_free(struct swap_report *report) { (void)report; }
#endif
#endif
