/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>
#include <reliefnt/memory_stats.h>
#include <reliefnt/swap_report.h>
#include <reliefnt/text_stream.h>
#include <linux/errno.h>

enum memory_view_kind { MEMORY_VIEW_INFO, MEMORY_VIEW_VMSTAT, MEMORY_VIEW_SWAPS };
struct proc_memory_view {
    enum memory_view_kind kind;
    struct memory_snapshot memory;
    struct memory_events events;
    struct swap_report *swap;
};

/** @brief Recognize one intrinsic memory file key.
 * @param name Borrowed key, nullable. @return True for the three standard files.
 */
bool proc_memory_is_file(const char *name)
{
    return name && (!__builtin_strcmp(name, "/proc/meminfo") ||
                    !__builtin_strcmp(name, "/proc/vmstat") ||
                    !__builtin_strcmp(name, "/proc/swaps"));
}

/** @brief Capture one stable report while its producers are execution-owned.
 * @param name Intrinsic key. @param out Required owned result, empty on error.
 * @return Zero or negative errno. No retained path, area or task escapes.
 */
int proc_memory_open(const char *name, void **out)
{
    if (!out || !proc_memory_is_file(name)) return -EINVAL;
    *out = NULL; struct proc_memory_view *view = kernel_malloc(sizeof(*view));
    if (!view) {  return -ENOMEM; }
    *view = (struct proc_memory_view){0}; int rc = 0;
    if (!__builtin_strcmp(name, "/proc/meminfo")) {
        view->kind = MEMORY_VIEW_INFO; memory_snapshot_get(&view->memory);
    } else if (!__builtin_strcmp(name, "/proc/vmstat")) {
        view->kind = MEMORY_VIEW_VMSTAT; memory_events_get(&view->events);
    } else {
        view->kind = MEMORY_VIEW_SWAPS; rc = swap_report_capture(&view->swap);
    }
    if (rc) { kernel_free(view); return rc; }
    *out = view; return 0;
}

/** @brief Emit one named KiB field. @param s Output slice. @param name Key.
 * @param value Snapshot KiB value. No allocation or ownership change.
 */
static void memory_field(struct text_stream *s, const char *name, uint64_t value)
{
    text_string(s, name); text_string(s, ": "); text_unsigned(s, value); text_string(s, " kB\n");
}

/** @brief Emit one named event field. @param s Output slice. @param name Key.
 * @param value Completed event count. No allocation or ownership change.
 */
static void event_field(struct text_stream *s, const char *name, uint64_t value)
{
    text_string(s, name); text_string(s, " "); text_unsigned(s, value); text_string(s, "\n");
}

/** @brief Render a live-path snapshot with Linux's four octal escapes.
 * @param s Output slice. @param path Captured NUL-terminated pathname.
 */
static void swap_path(struct text_stream *s, const char *path)
{
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\\') {
            char escaped[] = {'\\', (char)('0' + ((*p >> 6) & 7)),
                (char)('0' + ((*p >> 3) & 7)), (char)('0' + (*p & 7))};
            text_bytes(s, escaped, sizeof(escaped));
        } else text_bytes(s, p, 1);
    }
}

/** @brief Convert a captured page count to KiB, saturating rather than wrapping.
 * @param pages Physical or slot pages. @return KiB in the 4096-byte page ABI.
 */
static uint64_t swap_kib(uint64_t pages)
{ return pages > UINT64_MAX / 4 ? UINT64_MAX : pages * 4; }

/** @brief Render an immutable snapshot through an arbitrary offset slice.
 * @param value Borrowed capture. @param offset Requested byte position.
 * @param buffer Destination, nullable when capacity is zero. @param capacity
 * Maximum bytes. @param read Required actual count. @return Zero or -EINVAL.
 * No producer callback or additional ownership occurs during rendering.
 */
int proc_memory_read(void *value, uint64_t offset, void *buffer, uint32_t capacity, uint32_t *read)
{
    if (!read) return -EINVAL;
    *read = 0;
    struct proc_memory_view *view = value;
    if (!view || (!buffer && capacity)) return -EINVAL;
    struct text_stream s = {.offset = offset, .buffer = buffer, .capacity = capacity};
    if (view->kind == MEMORY_VIEW_INFO) {
        const struct memory_snapshot *m = &view->memory;
        memory_field(&s, "MemTotal", m->total_kib);
        memory_field(&s, "MemFree", m->free_kib);
        memory_field(&s, "MemAvailable", m->available_kib);
        memory_field(&s, "Cached", m->cached_kib);
        memory_field(&s, "Dirty", m->dirty_kib);
        memory_field(&s, "SwapCached", 0);
        memory_field(&s, "SwapTotal", m->swap_total_kib);
        memory_field(&s, "SwapFree", m->swap_free_kib);
    } else if (view->kind == MEMORY_VIEW_VMSTAT) {
        static const char *const names[MEMORY_EVENT_COUNT] = {"pgfault", "pgmajfault", "pswpin",
            "pswpout", "pgscan_direct", "pgscan_kswapd", "pgsteal_direct", "pgsteal_kswapd", "oom_kill"};
        for (unsigned i = 0; i < MEMORY_EVENT_COUNT; ++i) event_field(&s, names[i], view->events.value[i]);
    } else if (view->kind == MEMORY_VIEW_SWAPS) {
        text_string(&s, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
        for (size_t i = 0; view->swap && i < view->swap->count; ++i) {
            const struct swap_report_row *row = &view->swap->rows[i];
            swap_path(&s, row->path); text_string(&s, "\t");
            text_string(&s, row->raw ? "partition" : "file"); text_string(&s, "\t");
            text_unsigned(&s, swap_kib(row->total)); text_string(&s, "\t");
            text_unsigned(&s, swap_kib(row->used)); text_string(&s, "\t");
            text_signed(&s, row->priority); text_string(&s, "\n");
        }
    } else return -EINVAL;
    *read = s.written; return 0;
}

/** @brief Release the complete immutable proc memory capture.
 * @param value Owned view, nullable. No provider callback or page pin remains.
 */
void proc_memory_close(void *value)
{
    struct proc_memory_view *view = value;
    if (!view) return;
    swap_report_free(view->swap); kernel_free(view);
}
