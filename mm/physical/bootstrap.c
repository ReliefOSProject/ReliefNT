/* Reserve early metadata and page tables without invoking the PMM or heap. */
#include "topology_internal.h"
#include <reliefnt/framebuffer.h>
#include <reliefnt/paging.h>
#include <reliefnt/power.h>
#include <linux/errno.h>

struct bootstrap_state {
    struct mm_topology topology;
    const struct boot_info *boot;
    bool published;
    bool finished;
};

struct physical_span {
    uint64_t start, end;
};

struct conflict_scan {
    uint64_t start, end, skip_to;
};

static struct bootstrap_state bootstrap;

/**
 * @brief Validate one complete borrowed buffer under the installed shared map.
 * @param address Current identity or direct-map pointer to readable bytes.
 * @param count Descriptor count, or one for an ordinary byte buffer.
 * @param stride Bytes per descriptor.
 * @return 0 or negative errno; never probes an unproven firmware pointer.
 */
static int reachable_input(uint64_t address, uint32_t count, uint32_t stride)
{
    if (!count) return 0;
    if (!address || !stride || address > UINTPTR_MAX) return -EINVAL;
    uint64_t bytes = (uint64_t)count * stride;
    if (bytes > UINTPTR_MAX - address) return -EOVERFLOW;
    uint64_t physical = mm_topology_pointer_phys(address);
    return paging_kernel_direct_map_range(physical, bytes) ? 0 : -EFAULT;
}

/**
 * @brief Prove complete input-map and Multiboot block reachability before reading.
 * @param boot Borrowed readable handoff; firmware buffers are not yet trusted.
 * @return 0 or negative errno before any potentially unreachable buffer access.
 */
static int inputs_reachable(const struct boot_info *boot)
{
    if (!boot) return -EINVAL;
    int rc = reachable_input(boot->mmap_addr, boot->mmap_entry_count, boot->mmap_entry_size);
    if (rc) return rc;
    rc = reachable_input(boot->efi_mmap_addr, boot->efi_mmap_entry_count,
                         boot->efi_mmap_entry_size);
    if (rc) return rc;
    if (boot->multiboot_info) {
        rc = reachable_input(boot->multiboot_info, 1, sizeof(struct multiboot2_info));
        if (rc) return rc;
        const struct multiboot2_info *info = (const void *)(uintptr_t)boot->multiboot_info;
        rc = reachable_input(boot->multiboot_info, 1, info->total_size);
        if (rc) return rc;
    }
    return 0;
}

/**
 * @brief Validate a framebuffer reservation and retain its raw physical extent.
 * @param start Inclusive physical start, zero only for an absent reservation.
 * @param length Byte length, zero only for an absent reservation.
 * @param out Writable span output; zeroed for an absent reservation.
 * @return 0 or negative errno on an inconsistent or overflowing extent.
 */
static int display_span(uint64_t start, uint64_t length, struct physical_span *out)
{
    *out = (struct physical_span){0};
    if (!start && !length) return 0;
    if (!start || !length) return -EINVAL;
    if (length > UINT64_MAX - start) return -EOVERFLOW;
    *out = (struct physical_span){start, start + length};
    return 0;
}

/**
 * @brief Snapshot both active display reservations before selecting workspace.
 * @param spans Two writable physical span slots, absent entries zeroed.
 * @return 0 or negative errno when display ownership cannot be established.
 */
static int display_spans(struct physical_span spans[2])
{
    const struct framebuffer *fb = framebuffer_get();
    if (!fb) return -EINVAL;
    uint64_t start = fb->reservation_start, bytes = fb->reservation_bytes;
    if (!start && !bytes && fb->available) {
        uint64_t pointer = (uintptr_t)fb->pixels;
        bytes = (uint64_t)fb->pitch * fb->height;
        if (!pointer || !bytes || pointer < RELIEFNT_KERNEL_DIRECT_MAP_BASE ||
            pointer - RELIEFNT_KERNEL_DIRECT_MAP_BASE >= RELIEFNT_KERNEL_DIRECT_MAP_SIZE)
            return -EINVAL;
        start = pointer - RELIEFNT_KERNEL_DIRECT_MAP_BASE;
        if (!paging_kernel_direct_map_range(start, bytes)) return -EFAULT;
    }
    int rc = display_span(start, bytes, &spans[0]);
    if (rc) return rc;
    return display_span(fb->auxiliary_reservation_start,
                        fb->auxiliary_reservation_bytes, &spans[1]);
}

/**
 * @brief Find the end of each reserved interval overlapping one candidate.
 * @param start Inclusive aligned interval start.
 * @param end Exclusive aligned interval end.
 * @param kind Validated source classification.
 * @param context Writable candidate scan state.
 * @return Always 0; no memory is allocated or reserved by this callback.
 */
static int scan_conflict(uint64_t start, uint64_t end, enum mm_input_kind kind,
                         void *context)
{
    struct conflict_scan *scan = context;
    if (kind != MM_INPUT_USABLE && start < scan->end && end > scan->start &&
        end > scan->skip_to) scan->skip_to = end;
    return 0;
}

/**
 * @brief Include a display span in a candidate's conservative conflict scan.
 * @param scan Writable candidate state.
 * @param span Borrowed original framebuffer physical interval.
 * @param limit Aligned exclusive supported physical limit.
 */
static void scan_display(struct conflict_scan *scan, struct physical_span span,
                         uint64_t limit)
{
    if (span.start >= span.end || span.start >= limit) return;
    uint64_t start = span.start & ~(MM_TOPOLOGY_PAGE_SIZE - 1);
    uint64_t end = span.end > limit ? limit : span.end;
    end = (end + MM_TOPOLOGY_PAGE_SIZE - 1) & ~(MM_TOPOLOGY_PAGE_SIZE - 1);
    if (start < scan->end && end > scan->start && end > scan->skip_to)
        scan->skip_to = end;
}

/**
 * @brief Locate a reachable workspace within the union of raw usable byte ranges.
 * @param boot Borrowed already validated handoff.
 * @param limit Exclusive supported physical address limit.
 * @param bytes Page-aligned nonzero required workspace bytes.
 * @param spans Two validated active framebuffer reservations.
 * @param chosen Required output for physical workspace start on success.
 * @return 0 or negative errno; no allocation, heap, PMM, or topology build recursion.
 */
static int find_workspace(const struct boot_info *boot, uint64_t limit, uint64_t bytes,
                          const struct physical_span spans[2], uint64_t *chosen)
{
    uint32_t count = boot->efi_mmap_entry_count ? boot->efi_mmap_entry_count : boot->mmap_entry_count;
    uint64_t reachable_end = limit < RELIEFNT_KERNEL_DIRECT_MAP_SIZE ?
                             limit : RELIEFNT_KERNEL_DIRECT_MAP_SIZE;
    uint64_t search_at = 0;
    while (search_at < reachable_end) {
        uint64_t component_start = reachable_end;
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t start, end;
            int rc = mm_topology_candidate(boot, i, limit, &start, &end);
            if (rc < 0) return rc;
            if (!rc || end <= search_at || start >= reachable_end) continue;
            if (start < search_at) start = search_at;
            if (start < component_start) component_start = start;
        }
        if (component_start == reachable_end) break;

        /* Repeatedly extend the raw byte union. Input need not be sorted, and
         * adjacent endpoints join exactly; a one-byte gap does not. */
        uint64_t component_end = component_start, previous;
        do {
            previous = component_end;
            for (uint32_t i = 0; i < count; ++i) {
                uint64_t start, end;
                int rc = mm_topology_candidate(boot, i, limit, &start, &end);
                if (rc < 0) return rc;
                if (rc && start <= component_end && end > component_end)
                    component_end = end < reachable_end ? end : reachable_end;
            }
        } while (component_end > previous && component_end < reachable_end);

        uint64_t start = (component_start + MM_TOPOLOGY_PAGE_SIZE - 1) &
                         ~(MM_TOPOLOGY_PAGE_SIZE - 1);
        uint64_t end = component_end & ~(MM_TOPOLOGY_PAGE_SIZE - 1);
        while (start < end && bytes <= end - start) {
            uint64_t mapped_start, mapped_end;
            if (!paging_kernel_direct_map_next(start, end, &mapped_start, &mapped_end)) break;
            start = mapped_start;
            if (bytes > mapped_end - start) { start = mapped_end; continue; }
            struct conflict_scan scan = {start, start + bytes, start};
            int rc = mm_topology_visit(boot, limit, scan_conflict, &scan, NULL);
            if (rc) return rc;
            scan_display(&scan, spans[0], limit);
            scan_display(&scan, spans[1], limit);
            for (uint32_t i = 0; i < power_acpi_resource_count(); ++i) {
                uint64_t base, length;
                if (!power_acpi_resource(i, &base, &length) || length > UINT64_MAX - base)
                    return -EINVAL;
                scan_display(&scan, (struct physical_span){base, base + length}, limit);
            }
            if (scan.skip_to > start) { start = scan.skip_to; continue; }
            *chosen = start;
            return 0;
        }
        search_at = component_end;
    }
    return -ENOMEM;
}

/**
 * @brief Reserve reachable metadata workspace and publish a normalized topology.
 * @param boot Borrowed parsed handoff and complete current-mapping-readable input.
 * @param physical_bits CPU physical width from 32 through 52 inclusive.
 * @return 0 or negative errno, with no published state on failure. BSP only,
 * interrupts disabled, after shared direct map and framebuffer initialization,
 * before PMM or heap initialization. Workspace stays owned after finish.
 */
int mm_bootstrap_prepare(const struct boot_info *boot, uint8_t physical_bits)
{
    if (bootstrap.published) return -EBUSY;
    if (physical_bits < 32 || physical_bits > 52) return -EINVAL;
    int rc = inputs_reachable(boot);
    if (rc) return rc;
    size_t bytes;
    rc = mm_topology_workspace_size(boot, &bytes);
    if (rc) return rc;
    size_t resource_count = power_acpi_resource_count();
    /* Reservations can fill existing topology gaps as well as split their
     * boundary ranges; one initial range-capacity bound covers all such gaps. */
    size_t extra_ranges = resource_count ? bytes / sizeof(struct mm_topology_range) + 1u : 0;
    if (resource_count > (SIZE_MAX - extra_ranges) / 2u) return -EOVERFLOW;
    extra_ranges += resource_count * 2u;
    if (extra_ranges > (SIZE_MAX - bytes) / sizeof(struct mm_topology_range)) return -EOVERFLOW;
    bytes += extra_ranges * sizeof(struct mm_topology_range);
    if (bytes > SIZE_MAX - (MM_TOPOLOGY_PAGE_SIZE - 1)) return -EOVERFLOW;
    uint64_t aligned_bytes = (bytes + MM_TOPOLOGY_PAGE_SIZE - 1) & ~(MM_TOPOLOGY_PAGE_SIZE - 1);
    uint64_t limit = 1ULL << physical_bits;
    if (limit > MM_TOPOLOGY_MAX_LIMIT) limit = MM_TOPOLOGY_MAX_LIMIT;
    struct physical_span spans[2];
    rc = display_spans(spans);
    if (rc) return rc;
    uint64_t physical = 0;
    rc = find_workspace(boot, limit, aligned_bytes, spans, &physical);
    if (rc) return rc;
    void *workspace = paging_kernel_direct_map(physical);
    if (!workspace) return -EFAULT;
    struct mm_topology result;
    rc = mm_topology_build(boot, physical_bits, workspace, (size_t)aligned_bytes, &result);
    if (rc) return rc;
    result.range_capacity += extra_ranges;
    for (size_t i = 0; i < 2; ++i) {
        rc = mm_topology_reserve(&result, spans[i].start, spans[i].end, 0);
        if (rc) return rc;
    }
    for (uint32_t i = 0; i < power_acpi_resource_count(); ++i) {
        uint64_t base, length;
        if (!power_acpi_resource(i, &base, &length) || length > UINT64_MAX - base) return -EINVAL;
        rc = mm_topology_reserve(&result, base, base + length, 0);
        if (rc) return rc;
    }
    rc = mm_topology_reserve(&result, physical, physical + aligned_bytes, MM_TOPOLOGY_BOOTSTRAP);
    if (rc) return rc;
    bootstrap.topology = result;
    bootstrap.boot = boot;
    bootstrap.published = true;
    bootstrap.finished = false;
    return 0;
}

/**
 * @brief Claim a contiguous mapped run from a usable topology range.
 * @param pages Nonzero number of 4 KiB pages.
 * @param inclusive_max Inclusive maximum physical byte in the entire run.
 * @return Owned physical start or 0 for invalid/exhausted requests; claims are
 * kept reserved through finish and must transfer to subsequent owners.
 */
uint64_t mm_bootstrap_alloc(uint32_t pages, uint64_t inclusive_max)
{
    if (!bootstrap.published || bootstrap.finished || !pages) return 0;
    uint64_t bytes = (uint64_t)pages * MM_TOPOLOGY_PAGE_SIZE;
    for (size_t i = 0; i < bootstrap.topology.range_count; ++i) {
        struct mm_topology_range r = bootstrap.topology.ranges[i];
        if (!(r.flags & MM_TOPOLOGY_USABLE)) continue;
        uint64_t start = r.start ? r.start : MM_TOPOLOGY_PAGE_SIZE, end = r.end;
        if (inclusive_max != UINT64_MAX) {
            uint64_t limit = (inclusive_max + 1) & ~(MM_TOPOLOGY_PAGE_SIZE - 1);
            if (end > limit) end = limit;
        }
        while (start < end && bytes <= end - start) {
            uint64_t mapped_start, mapped_end;
            if (!paging_kernel_direct_map_next(start, end, &mapped_start, &mapped_end)) break;
            if (bytes <= mapped_end - mapped_start) {
                if (mm_topology_reserve(&bootstrap.topology, mapped_start, mapped_start + bytes,
                                        MM_TOPOLOGY_BOOTSTRAP)) return 0;
                return mapped_start;
            }
            start = mapped_end;
        }
    }
    return 0;
}

/**
 * @brief Borrow the stable bootstrap-owned topology and all claimed pages.
 * @return Published view through and after finish, or NULL before preparation.
 */
const struct mm_topology *mm_bootstrap_topology(void)
{
    return bootstrap.published ? &bootstrap.topology : NULL;
}

/**
 * @brief Borrow the boot resources that must survive the direct-map transition.
 * @return The caller-owned handoff while bootstrap is active, otherwise NULL.
 * The BSP retains the handoff and its reserved buffers through map construction.
 */
const struct boot_info *mm_bootstrap_boot_info(void)
{
    return bootstrap.published && !bootstrap.finished ? bootstrap.boot : NULL;
}

/**
 * @brief Close the early allocator without returning its storage or owned pages.
 */
void mm_bootstrap_finish(void)
{
    if (bootstrap.published) bootstrap.finished = true;
}
