/* Build sparse PMM metadata without entering the runtime allocator. */
#include "metadata_internal.h"
#include <reliefnt/paging.h>
#include <linux/errno.h>

_Static_assert(sizeof(struct mm_page_section) <= MM_PHYS_PAGE_SIZE,
               "one metadata section must fit in one bootstrap page");

/**
 * @brief Count sections for one maximal contiguous trusted RAM run.
 * @param start Inclusive page-aligned start.
 * @param end Exclusive page-aligned end.
 * @return Metadata pages required for the run.
 */
static uint64_t run_sections(uint64_t start, uint64_t end)
{
    uint64_t pages = (end - start) / MM_PHYS_PAGE_SIZE;
    return pages / MM_METADATA_PAGES_PER_SECTION +
           (pages % MM_METADATA_PAGES_PER_SECTION != 0);
}

/**
 * @brief Find the next mapped run within the union of adjacent trusted RAM ranges.
 * @param topology Borrowed stable topology for this call.
 * @param cursor Inclusive page-aligned physical search start.
 * @param start Required output for the first mapped page.
 * @param end Required output for the exclusive mapped end.
 * @return 1 for a run, 0 at end, or -EINVAL for an inconsistent paging answer.
 * Bootstrap reservations may split topology ranges, so adjacent RAM ranges
 * are joined before querying coverage and no range pointer survives this call.
 */
static int next_mapped_ram(const struct mm_topology *topology, uint64_t cursor,
                           uint64_t *start, uint64_t *end)
{
    size_t i = 0;
    while (i < topology->range_count) {
        struct mm_topology_range r = topology->ranges[i++];
        if (!(r.flags & MM_TOPOLOGY_RAM) || r.end <= cursor) continue;
        uint64_t lo = r.start > cursor ? r.start : cursor;
        uint64_t hi = r.end;
        while (i < topology->range_count && topology->ranges[i].start == hi &&
               (topology->ranges[i].flags & MM_TOPOLOGY_RAM)) {
            hi = topology->ranges[i++].end;
        }
        if (paging_kernel_direct_map_next(lo, hi, start, end)) {
            if (*start < lo || *start >= *end || *end > hi ||
                ((*start | *end) & (MM_PHYS_PAGE_SIZE - 1)) ||
                !paging_kernel_direct_map_range(*start, *end - *start)) return -EINVAL;
            return 1;
        }
        cursor = hi;
    }
    return 0;
}

/**
 * @brief Validate and count maximal mapped trusted RAM runs without retaining iterators.
 * @param topology Borrowed bootstrap topology, not mutated during this pass.
 * @param sections Required metadata page count output.
 * @param ram_pages Mapped trusted RAM page count output.
 * @return 0 or negative errno; no allocation or ownership transfer.
 */
static int measure(const struct mm_topology *topology, uint64_t *sections,
                   uint64_t *ram_pages)
{
    uint64_t previous = 0;
    *sections = *ram_pages = 0;
    for (size_t i = 0; i < topology->range_count; ++i) {
        struct mm_topology_range r = topology->ranges[i];
        if (r.start >= r.end || (r.start | r.end) & (MM_PHYS_PAGE_SIZE - 1) ||
            r.start < previous || r.end > topology->physical_limit ||
            ((r.flags & MM_TOPOLOGY_USABLE) &&
             (!(r.flags & MM_TOPOLOGY_RAM) || (r.flags & MM_TOPOLOGY_RESERVED)))) return -EINVAL;
        previous = r.end;
    }
    uint64_t start, end, cursor = 0;
    int rc;
    while ((rc = next_mapped_ram(topology, cursor, &start, &end)) > 0) {
        *ram_pages += (end - start) / MM_PHYS_PAGE_SIZE;
        *sections += run_sections(start, end);
        cursor = end;
    }
    if (rc < 0) return rc;
    return *sections ? 0 : -ENOMEM;
}

/**
 * @brief Reserve metadata pages in bounded contiguous chunks, linking their storage.
 * @param count Number of pages to reserve.
 * @param pool Output list of direct-mapped pages, writable by the caller.
 * @return 0 or negative errno. Claims persist on failure and are never published free.
 */
static int reserve_pages(uint64_t count, struct mm_page_section **pool)
{
    *pool = NULL;
    while (count) {
        uint32_t chunk = count < 64 ? (uint32_t)count : 64;
        uint64_t phys = 0;
        while (chunk && !(phys = mm_bootstrap_alloc(chunk, UINT64_MAX))) chunk /= 2;
        if (!phys) return -ENOMEM;
        for (uint32_t i = 0; i < chunk; ++i) {
            struct mm_page_section *section = paging_kernel_direct_map(phys + (uint64_t)i * MM_PHYS_PAGE_SIZE);
            if (!section) return -EFAULT;
            section->next = *pool;
            *pool = section;
        }
        count -= chunk;
    }
    return 0;
}

/**
 * @brief Append sections for one maximal RAM run using already reserved pages.
 * @param out Unpublished metadata output.
 * @param pool Available reserved-page list, consumed by this call.
 * @param start Inclusive aligned RAM start.
 * @param end Exclusive aligned RAM end.
 */
static void append_run(struct mm_metadata *out, struct mm_page_section **pool,
                       uint64_t start, uint64_t end)
{
    uint64_t pfn = start / MM_PHYS_PAGE_SIZE, last = end / MM_PHYS_PAGE_SIZE;
    while (pfn < last) {
        struct mm_page_section *section = *pool;
        *pool = section->next;
        __builtin_memset(section, 0, sizeof(*section));
        section->first_pfn = pfn;
        uint64_t remaining = last - pfn;
        section->pages = remaining < MM_METADATA_PAGES_PER_SECTION ?
                         (uint32_t)remaining : MM_METADATA_PAGES_PER_SECTION;
        section->prev = out->last;
        if (out->last) out->last->next = section;
        else out->first = section;
        out->last = section;
        pfn += section->pages;
    }
}

/**
 * @brief Set usable/free bits in a section for a bounded half-open PFN interval.
 * @param section Prepared section owning the interval.
 * @param first Inclusive PFN within the section.
 * @param end Exclusive PFN within the section.
 */
static void seed_section(struct mm_page_section *section, uint64_t first, uint64_t end)
{
    uint64_t pos = first - section->first_pfn, finish = end - section->first_pfn;
    section->free_count += (uint32_t)(finish - pos);
    while (pos < finish) {
        unsigned bit = (unsigned)(pos % 64);
        unsigned count = (unsigned)(finish - pos);
        if (count > 64 - bit) count = 64 - bit;
        uint64_t mask = count == 64 ? UINT64_MAX : ((1ULL << count) - 1) << bit;
        section->usable[pos / 64] |= mask;
        section->free[pos / 64] |= mask;
        pos += count;
    }
}

/**
 * @brief Build a balanced immutable lookup tree from the ordered section list.
 * @param cursor Next section to consume; advanced by count entries.
 * @param count Number of list entries in the resulting subtree.
 * @return Borrowed subtree root; recursion depth is logarithmic in count.
 */
static struct mm_page_section *build_tree(struct mm_page_section **cursor, uint64_t count)
{
    if (!count) return NULL;
    struct mm_page_section *left = build_tree(cursor, count / 2);
    struct mm_page_section *root = *cursor;
    *cursor = root->next;
    root->left = left;
    root->right = build_tree(cursor, count - count / 2 - 1);
    return root;
}

/**
 * @brief Reserve, construct and seed sparse metadata from the current bootstrap view.
 * @param topology Published bootstrap view, whose range storage can move during reservation.
 * @param out Writable unpublished result, cleared on failure.
 * @return 0 or negative errno; BSP only, no PMM, heap, locks, or reclaim.
 * Reserved bootstrap pages stay reserved on failure and become permanent metadata on success.
 */
int mm_metadata_prepare(const struct mm_topology *topology, struct mm_metadata *out)
{
    if (!out) return -EINVAL;
    *out = (struct mm_metadata){0};
    if (!topology || topology != mm_bootstrap_topology() || !topology->ranges ||
        !topology->physical_limit || (topology->physical_limit & (MM_PHYS_PAGE_SIZE - 1))) return -EINVAL;
    uint64_t sections, ram_pages;
    int rc = measure(topology, &sections, &ram_pages);
    if (rc) return rc;
    struct mm_page_section *pool;
    rc = reserve_pages(sections, &pool);
    if (rc) return rc;
    struct mm_metadata result = {0};
    uint64_t start, end, cursor_phys = 0;
    /* Bootstrap allocations can split or merge ranges: restart from index zero. */
    while ((rc = next_mapped_ram(topology, cursor_phys, &start, &end)) > 0) {
        append_run(&result, &pool, start, end);
        cursor_phys = end;
    }
    if (rc < 0) return rc;
    if (pool) return -EINVAL;
    result.mapped_ram_pages = ram_pages;
    struct mm_page_section *cursor = result.first;
    result.root = build_tree(&cursor, sections);
    struct mm_page_section *section = result.first;
    for (size_t i = 0; i < topology->range_count; ++i) {
        struct mm_topology_range r = topology->ranges[i];
        if (!(r.flags & MM_TOPOLOGY_USABLE)) continue;
        uint64_t first = r.start / MM_PHYS_PAGE_SIZE, last = r.end / MM_PHYS_PAGE_SIZE;
        while (first < last) {
            while (section && first >= section->first_pfn + section->pages) section = section->next;
            if (!section || section->first_pfn >= last) break;
            if (first < section->first_pfn) first = section->first_pfn;
            uint64_t stop = section->first_pfn + section->pages;
            if (stop > last) stop = last;
            seed_section(section, first, stop);
            result.free_pages += stop - first;
            first = stop;
        }
    }
    if (!result.free_pages) return -ENOMEM;
    *out = result;
    return 0;
}

/**
 * @brief Find a represented page through the immutable balanced section tree.
 * @param metadata Borrowed prepared metadata with a stable root.
 * @param pfn Physical page frame number.
 * @return Borrowed containing section or NULL for a hole; no ownership transfer.
 */
struct mm_page_section *mm_metadata_find(const struct mm_metadata *metadata, uint64_t pfn)
{
    struct mm_page_section *s = metadata->root;
    while (s) {
        if (pfn < s->first_pfn) s = s->left;
        else if (pfn - s->first_pfn >= s->pages) s = s->right;
        else return s;
    }
    return NULL;
}
