/* Normalize borrowed firmware maps before a physical allocator exists. */
#include "topology_internal.h"
#include <reliefnt/paging.h>
#include <linux/errno.h>

#define EFI_MEMORY_RUNTIME (1ULL << 63)

struct topology_event {
    uint64_t position;
    uint32_t kind;
    int32_t delta;
};

struct event_writer {
    struct topology_event *events;
    size_t count;
    size_t capacity;
};

/**
 * @brief Convert a current identity or shared direct-map pointer to physical.
 * @param pointer Borrowed kernel virtual pointer as an integer.
 * @return Underlying physical address for the shared alias, otherwise identity.
 */
uint64_t mm_topology_pointer_phys(uint64_t pointer)
{
    if (pointer >= RELIEFNT_KERNEL_DIRECT_MAP_BASE &&
        pointer - RELIEFNT_KERNEL_DIRECT_MAP_BASE < RELIEFNT_KERNEL_DIRECT_MAP_SIZE)
        return pointer - RELIEFNT_KERNEL_DIRECT_MAP_BASE;
    return pointer;
}

/**
 * @brief Validate a counted borrowed descriptor buffer without reading it.
 * @param address Borrowed CPU-readable buffer address, zero only when count is zero.
 * @param stride Declared descriptor stride in bytes.
 * @param count Number of descriptors.
 * @param minimum Minimum valid descriptor stride.
 * @return 0 or negative errno on inconsistent fields or arithmetic overflow.
 */
static int validate_buffer(uint64_t address, uint32_t stride, uint32_t count,
                           size_t minimum)
{
    if (!count) return 0;
    if (!address || stride < minimum || address > UINTPTR_MAX) return -EINVAL;
    if ((uint64_t)count * stride > UINTPTR_MAX - address) return -EOVERFLOW;
    return 0;
}

/**
 * @brief Emit usable bytes or an outward-aligned conservative reserved span.
 * @param start Original inclusive physical address.
 * @param length Original byte length, possibly zero.
 * @param kind Source classification; usable is left unaligned for later union.
 * @param limit Exclusive aligned physical limit.
 * @param visit Optional callback to receive nonempty clipped intervals.
 * @param context Borrowed callback context.
 * @param clipped Optional descriptor count incremented when truncated by limit.
 * @return 0 or negative errno; never emits a wrapped range.
 */
static int visit_span(uint64_t start, uint64_t length, enum mm_input_kind kind,
                      uint64_t limit, mm_input_visitor visit, void *context,
                      uint64_t *clipped)
{
    if (!length) return 0;
    if (length > UINT64_MAX - start) return -EOVERFLOW;
    uint64_t end = start + length;
    if (clipped && end > limit) ++*clipped;
    if (start >= limit) return 0;
    if (end > limit) end = limit;
    if (kind != MM_INPUT_USABLE) {
        start &= ~(MM_TOPOLOGY_PAGE_SIZE - 1);
        end = (end + MM_TOPOLOGY_PAGE_SIZE - 1) & ~(MM_TOPOLOGY_PAGE_SIZE - 1);
    }
    if (start >= end || !visit) return 0;
    return visit(start, end, kind, context);
}

/**
 * @brief Classify an EFI descriptor without assuming boot services have ended.
 * @param entry Borrowed copied descriptor, including its attribute flags.
 * @return Usable, reclaimable or conservatively firmware-reserved kind.
 */
static enum mm_input_kind efi_kind(const struct efi_memory_descriptor *entry)
{
    if (entry->attribute & EFI_MEMORY_RUNTIME) return MM_INPUT_FIRMWARE_RESERVED;
    if (entry->type == 7) return MM_INPUT_USABLE;
    if (entry->type == 1 || entry->type == 2 || entry->type == 3 ||
        entry->type == 4 || entry->type == 9) return MM_INPUT_RECLAIMABLE;
    return MM_INPUT_FIRMWARE_RESERVED;
}

/**
 * @brief Classify a Multiboot descriptor according to conservative type priority.
 * @param type Firmware-defined Multiboot memory type.
 * @return Usable, ACPI-reclaimable or firmware-reserved kind.
 */
static enum mm_input_kind multiboot_kind(uint32_t type)
{
    if (type == 1) return MM_INPUT_USABLE;
    if (type == 3) return MM_INPUT_RECLAIMABLE;
    return MM_INPUT_FIRMWARE_RESERVED;
}

/**
 * @brief Copy a possibly unaligned Multiboot descriptor at its declared stride.
 * @param boot Borrowed previously validated boot map.
 * @param index Valid descriptor index.
 * @param out Caller-owned decoded descriptor.
 */
static void read_multiboot(const struct boot_info *boot, uint32_t index,
                           struct multiboot2_mmap_entry *out)
{
    const void *p = (const void *)(uintptr_t)(boot->mmap_addr +
                                              (uint64_t)index * boot->mmap_entry_size);
    __builtin_memcpy(out, p, sizeof(*out));
}

/**
 * @brief Copy a possibly unaligned EFI descriptor at its declared stride.
 * @param boot Borrowed previously validated EFI map.
 * @param index Valid descriptor index.
 * @param out Caller-owned decoded descriptor.
 */
static void read_efi(const struct boot_info *boot, uint32_t index,
                     struct efi_memory_descriptor *out)
{
    const void *p = (const void *)(uintptr_t)(boot->efi_mmap_addr +
                                              (uint64_t)index * boot->efi_mmap_entry_size);
    __builtin_memcpy(out, p, sizeof(*out));
}

/**
 * @brief Verify optional boot strings and RSDP remain inside the retained block.
 * @param boot Borrowed decoded boot information.
 * @param start Inclusive address of the validated borrowed Multiboot block.
 * @param end Exclusive end of that block.
 * @return 0 if every present borrowed pointer lies inside it, otherwise -EINVAL.
 */
static int validate_borrowed_pointers(const struct boot_info *boot, uint64_t start,
                                      uint64_t end)
{
    uint64_t ptrs[] = {(uintptr_t)boot->cmdline, (uintptr_t)boot->bootloader,
                       boot->rsdp_addr};
    for (size_t i = 0; i < sizeof(ptrs) / sizeof(ptrs[0]); ++i)
        if (ptrs[i] && (ptrs[i] < start || ptrs[i] >= end)) return -EINVAL;
    for (uint32_t i = 0; i < boot->module_count; ++i) {
        uint64_t name = (uintptr_t)boot->modules[i].name;
        if (name && (name < start || name >= end)) return -EINVAL;
    }
    return 0;
}

/**
 * @brief Visit validated raw usable bytes and aligned reserved physical intervals.
 * @param boot Borrowed readable handoff and input buffers.
 * @param limit Aligned exclusive physical limit, at most 64 TiB.
 * @param visit Optional callback for each interval.
 * @param context Borrowed callback context.
 * @param clipped Optional count of firmware descriptors beyond the limit.
 * @return 0 or negative errno; firmware reservations win over all usable entries.
 */
int mm_topology_visit(const struct boot_info *boot, uint64_t limit,
                      mm_input_visitor visit, void *context, uint64_t *clipped)
{
    if (clipped) *clipped = 0;
    if (!boot || !limit || limit > MM_TOPOLOGY_MAX_LIMIT ||
        (limit & (MM_TOPOLOGY_PAGE_SIZE - 1)) ||
        boot->module_count > sizeof(boot->modules) / sizeof(boot->modules[0])) return -EINVAL;
    int rc = validate_buffer(boot->mmap_addr, boot->mmap_entry_size,
                             boot->mmap_entry_count, sizeof(struct multiboot2_mmap_entry));
    if (rc) return rc;
    rc = validate_buffer(boot->efi_mmap_addr, boot->efi_mmap_entry_size,
                         boot->efi_mmap_entry_count, sizeof(struct efi_memory_descriptor));
    if (rc) return rc;
    if (!boot->mmap_entry_count && !boot->efi_mmap_entry_count) return -EINVAL;

    for (uint32_t i = 0; i < boot->mmap_entry_count; ++i) {
        struct multiboot2_mmap_entry entry;
        read_multiboot(boot, i, &entry);
        enum mm_input_kind kind = multiboot_kind(entry.type);
        /* A final EFI map supersedes older Multiboot free claims, but older
         * reservation claims still exclude overlapping EFI conventional RAM. */
        mm_input_visitor callback = boot->efi_mmap_entry_count && kind == MM_INPUT_USABLE ? NULL : visit;
        rc = visit_span(entry.addr, entry.len, kind, limit, callback, context, clipped);
        if (rc) return rc;
    }
    for (uint32_t i = 0; i < boot->efi_mmap_entry_count; ++i) {
        struct efi_memory_descriptor entry;
        read_efi(boot, i, &entry);
        if (entry.number_of_pages > (UINT64_MAX - entry.physical_start) / MM_TOPOLOGY_PAGE_SIZE)
            return -EOVERFLOW;
        rc = visit_span(entry.physical_start, entry.number_of_pages * MM_TOPOLOGY_PAGE_SIZE,
                        efi_kind(&entry), limit, visit, context, clipped);
        if (rc) return rc;
    }

#define VISIT_BOOT(base, length) do { \
    rc = visit_span((base), (length), MM_INPUT_BOOT_RESERVED, limit, visit, context, NULL); \
    if (rc) return rc; \
} while (0)
    VISIT_BOOT(0, 0x100000ULL);
    VISIT_BOOT(RELIEFNT_KERNEL_HOLE_START, RELIEFNT_KERNEL_HOLE_END - RELIEFNT_KERNEL_HOLE_START);
    VISIT_BOOT(mm_topology_pointer_phys((uintptr_t)boot), sizeof(*boot));
    if (boot->mmap_entry_count)
        VISIT_BOOT(mm_topology_pointer_phys(boot->mmap_addr),
                   (uint64_t)boot->mmap_entry_count * boot->mmap_entry_size);
    if (boot->efi_mmap_entry_count)
        VISIT_BOOT(mm_topology_pointer_phys(boot->efi_mmap_addr),
                   (uint64_t)boot->efi_mmap_entry_count * boot->efi_mmap_entry_size);
    uint64_t mb_start = boot->multiboot_info, mb_end = mb_start;
    if (mb_start) {
        if ((mb_start & 7u) || mb_start > UINTPTR_MAX - sizeof(struct multiboot2_info))
            return -EINVAL;
        const struct multiboot2_info *info = (const void *)(uintptr_t)mb_start;
        if (info->total_size < 16 || (info->total_size & 7u) ||
            mb_start > UINTPTR_MAX - info->total_size) return -EINVAL;
        mb_end += info->total_size;
        VISIT_BOOT(mm_topology_pointer_phys(mb_start), info->total_size);
    }
    rc = validate_borrowed_pointers(boot, mb_start, mb_end);
    if (rc) return rc;
    if (boot->efi_system_table) VISIT_BOOT(boot->efi_system_table, 1);
    if (boot->framebuffer_pitch && boot->framebuffer_height)
        VISIT_BOOT(boot->framebuffer_addr, (uint64_t)boot->framebuffer_pitch * boot->framebuffer_height);
    for (uint32_t i = 0; i < boot->module_count; ++i) {
        if (boot->modules[i].end < boot->modules[i].start) return -EINVAL;
        VISIT_BOOT(boot->modules[i].start, boot->modules[i].end - boot->modules[i].start);
    }
#undef VISIT_BOOT
    return 0;
}

/**
 * @brief Read one usable candidate from the selected validated firmware map.
 * @param boot Borrowed handoff validated by mm_topology_visit.
 * @param index Valid descriptor index in the selected map.
 * @param limit Exclusive aligned physical limit.
 * @param start Required output for the inclusive raw byte start.
 * @param end Required output for the exclusive raw byte end.
 * @return 1 for a nonempty usable candidate, 0 otherwise, or a negative errno.
 */
int mm_topology_candidate(const struct boot_info *boot, uint32_t index,
                          uint64_t limit, uint64_t *start, uint64_t *end)
{
    uint64_t address, length;
    enum mm_input_kind kind;
    if (boot->efi_mmap_entry_count) {
        if (index >= boot->efi_mmap_entry_count) return -EINVAL;
        struct efi_memory_descriptor entry;
        read_efi(boot, index, &entry);
        if (entry.number_of_pages > (UINT64_MAX - entry.physical_start) / MM_TOPOLOGY_PAGE_SIZE)
            return -EOVERFLOW;
        address = entry.physical_start;
        length = entry.number_of_pages * MM_TOPOLOGY_PAGE_SIZE;
        kind = efi_kind(&entry);
    } else {
        if (index >= boot->mmap_entry_count) return -EINVAL;
        struct multiboot2_mmap_entry entry;
        read_multiboot(boot, index, &entry);
        address = entry.addr; length = entry.len; kind = multiboot_kind(entry.type);
    }
    if (length > UINT64_MAX - address) return -EOVERFLOW;
    if (kind != MM_INPUT_USABLE || !length || address >= limit) return 0;
    uint64_t top = address + length;
    if (top > limit) top = limit;
    *start = address; *end = top;
    return 1;
}

/**
 * @brief Compute checked event and range capacities from input descriptor count.
 * @param boot Borrowed validated boot information.
 * @param events Required output for maximum sweep event count.
 * @param ranges Required output for range capacity including later reservations.
 * @param bytes Required output for total workspace bytes.
 * @return 0 or -EOVERFLOW; capacity grows with entries, never physical address.
 */
static int workspace_layout(const struct boot_info *boot, size_t *events,
                            size_t *ranges, size_t *bytes)
{
    uint64_t n64 = (uint64_t)boot->mmap_entry_count + boot->efi_mmap_entry_count +
                   boot->module_count + 10u;
    if (n64 > (SIZE_MAX - 16u) / 4u) return -EOVERFLOW;
    size_t n = (size_t)n64;
    *events = 2u * n;
    *ranges = 4u * n + 16u;
    if (*events > SIZE_MAX / sizeof(struct topology_event) ||
        *ranges > SIZE_MAX / sizeof(struct mm_topology_range)) return -EOVERFLOW;
    size_t eb = *events * sizeof(struct topology_event);
    size_t rb = *ranges * sizeof(struct mm_topology_range);
    if (rb > SIZE_MAX - eb) return -EOVERFLOW;
    *bytes = eb + rb;
    return 0;
}

/**
 * @brief Validate borrowed input and calculate required caller-owned storage.
 * @param boot Borrowed readable handoff and complete map/boot buffers.
 * @param bytes Required writable byte-count output, zeroed on failure.
 * @return 0, -EINVAL or -EOVERFLOW; no allocation or ownership transfer.
 */
int mm_topology_workspace_size(const struct boot_info *boot, size_t *bytes)
{
    if (!bytes) return -EINVAL;
    *bytes = 0;
    int rc = mm_topology_visit(boot, MM_TOPOLOGY_MAX_LIMIT, NULL, NULL, NULL);
    if (rc) return rc;
    size_t events, ranges;
    return workspace_layout(boot, &events, &ranges, bytes);
}

/**
 * @brief Append one pair of sweep events to pre-sized caller workspace.
 * @param start Inclusive range start; usable may be unaligned.
 * @param end Exclusive range end; usable may be unaligned.
 * @param kind Validated source category.
 * @param context Event-writer state backed by caller workspace.
 * @return 0 or -ENOSPC without appending an incomplete event pair.
 */
static int write_events(uint64_t start, uint64_t end, enum mm_input_kind kind, void *context)
{
    struct event_writer *w = context;
    if (w->capacity - w->count < 2) return -ENOSPC;
    w->events[w->count++] = (struct topology_event){start, (uint32_t)kind, 1};
    w->events[w->count++] = (struct topology_event){end, (uint32_t)kind, -1};
    return 0;
}

/**
 * @brief Compare sweep events, placing starts before ends at equal positions.
 * @param a First event by value.
 * @param b Second event by value.
 * @return True if a sorts before b; events at one position may otherwise tie.
 */
static bool event_before(struct topology_event a, struct topology_event b)
{
    if (a.position != b.position) return a.position < b.position;
    return a.delta > b.delta;
}

/**
 * @brief Restore one max-heap branch in an event array.
 * @param events Writable array of sweep events.
 * @param root Index of the branch root.
 * @param count Number of active heap entries.
 */
static void sift_events(struct topology_event *events, size_t root, size_t count)
{
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && event_before(events[child], events[child + 1])) ++child;
        if (!event_before(events[root], events[child])) break;
        struct topology_event tmp = events[root]; events[root] = events[child]; events[child] = tmp;
        root = child;
    }
}

/**
 * @brief Sort sweep events in place with bounded stack and no allocation.
 * @param events Writable event array.
 * @param count Number of entries.
 */
static void sort_events(struct topology_event *events, size_t count)
{
    for (size_t i = count / 2; i > 0; --i) sift_events(events, i - 1, count);
    for (size_t n = count; n > 1; --n) {
        struct topology_event tmp = events[0]; events[0] = events[n - 1]; events[n - 1] = tmp;
        sift_events(events, 0, n - 1);
    }
}

/**
 * @brief Union usable bytes before admitting only wholly covered physical pages.
 * @param events Writable event array containing raw usable and aligned reservations.
 * @param count Number of input events; every usable interval supplies two events.
 * @return Event count after merging usable bytes and page-aligning their union.
 * The array is partitioned in place; no event capacity or allocation is added.
 */
static size_t normalize_usable_events(struct topology_event *events, size_t count)
{
    size_t usable_count = 0;
    for (size_t i = 0; i < count; ++i) {
        if (events[i].kind == MM_INPUT_USABLE) {
            struct topology_event tmp = events[usable_count];
            events[usable_count++] = events[i];
            events[i] = tmp;
        }
    }
    sort_events(events, usable_count);
    size_t written = 0;
    uint64_t active = 0, beginning = 0;
    for (size_t i = 0; i < usable_count; ++i) {
        struct topology_event event = events[i];
        if (event.delta > 0) {
            if (!active) beginning = event.position;
            ++active;
        } else {
            --active;
            if (!active) {
                uint64_t start = (beginning + MM_TOPOLOGY_PAGE_SIZE - 1) &
                                 ~(MM_TOPOLOGY_PAGE_SIZE - 1);
                uint64_t end = event.position & ~(MM_TOPOLOGY_PAGE_SIZE - 1);
                if (start < end) {
                    events[written++] = (struct topology_event){start, MM_INPUT_USABLE, 1};
                    events[written++] = (struct topology_event){end, MM_INPUT_USABLE, -1};
                }
            }
        }
    }
    for (size_t i = usable_count; i < count; ++i) events[written++] = events[i];
    return written;
}

/**
 * @brief Test two overflow-checked virtual byte intervals for overlap.
 * @param a First starting pointer.
 * @param alen First byte length.
 * @param b Second starting pointer.
 * @param blen Second byte length.
 * @return True if the intervals overlap or cannot be represented safely.
 */
static bool overlap(uintptr_t a, size_t alen, uintptr_t b, size_t blen)
{
    if (!alen || !blen) return false;
    if (alen > UINTPTR_MAX - a || blen > UINTPTR_MAX - b) return true;
    return a < b + blen && b < a + alen;
}

/**
 * @brief Validate that caller workspace does not overwrite borrowed source data.
 * @param boot Borrowed, already validated handoff.
 * @param workspace Writable byte-addressed workspace.
 * @param bytes Workspace capacity in bytes.
 * @param out Writable topology result.
 * @return True if the workspace and result are disjoint from inputs and each other.
 */
static bool workspace_disjoint(const struct boot_info *boot, void *workspace,
                               size_t bytes, struct mm_topology *out)
{
    uintptr_t p = (uintptr_t)workspace;
    if (overlap(p, bytes, (uintptr_t)boot, sizeof(*boot)) ||
        overlap(p, bytes, (uintptr_t)out, sizeof(*out)) ||
        overlap(p, bytes, (uintptr_t)boot->mmap_addr,
                (size_t)boot->mmap_entry_count * boot->mmap_entry_size) ||
        overlap(p, bytes, (uintptr_t)boot->efi_mmap_addr,
                (size_t)boot->efi_mmap_entry_count * boot->efi_mmap_entry_size)) return false;
    if (boot->multiboot_info) {
        const struct multiboot2_info *info = (const void *)(uintptr_t)boot->multiboot_info;
        if (overlap(p, bytes, (uintptr_t)info, info->total_size)) return false;
    }
    return true;
}

/**
 * @brief Normalize source spans into a sorted conservative workspace-backed view.
 * @param boot Borrowed readable decoded handoff and map/boot buffers.
 * @param physical_bits CPU physical-address width from 32 through 52.
 * @param workspace Aligned caller-owned storage, disjoint from inputs and out.
 * @param workspace_bytes Byte capacity, at least mm_topology_workspace_size().
 * @param out Writable view, cleared on failure; workspace owns all range storage.
 * @return 0 or negative errno; no allocation, PMM calls, locks or partial view.
 */
int mm_topology_build(const struct boot_info *boot, uint8_t physical_bits,
                      void *workspace, size_t workspace_bytes, struct mm_topology *out)
{
    if (!out) return -EINVAL;
    *out = (struct mm_topology){0};
    if (physical_bits < 32 || physical_bits > 52 || !workspace ||
        ((uintptr_t)workspace % _Alignof(struct topology_event))) return -EINVAL;
    size_t required, event_capacity, range_capacity;
    int rc = mm_topology_workspace_size(boot, &required);
    if (rc) return rc;
    if (workspace_bytes < required) return -ENOSPC;
    if (!workspace_disjoint(boot, workspace, required, out)) return -EINVAL;
    rc = workspace_layout(boot, &event_capacity, &range_capacity, &required);
    if (rc) return rc;
    uint64_t limit = 1ULL << physical_bits;
    if (limit > MM_TOPOLOGY_MAX_LIMIT) limit = MM_TOPOLOGY_MAX_LIMIT;
    struct topology_event *events = workspace;
    struct mm_topology_range *ranges = (void *)(events + event_capacity);
    struct event_writer writer = {events, 0, event_capacity};
    uint64_t clipped;
    rc = mm_topology_visit(boot, limit, write_events, &writer, &clipped);
    if (rc) return rc;
    writer.count = normalize_usable_events(events, writer.count);
    sort_events(events, writer.count);
    uint64_t counts[4] = {0}, previous = 0, total = 0;
    size_t used = 0;
    for (size_t i = 0; i < writer.count; ++i) {
        struct topology_event event = events[i];
        uint32_t flags = 0;
        if (counts[MM_INPUT_FIRMWARE_RESERVED]) flags = MM_TOPOLOGY_RESERVED;
        else if (counts[MM_INPUT_RECLAIMABLE])
            flags = MM_TOPOLOGY_RAM | MM_TOPOLOGY_RECLAIMABLE | MM_TOPOLOGY_RESERVED;
        else if (counts[MM_INPUT_USABLE]) flags = MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE;
        if (counts[MM_INPUT_BOOT_RESERVED]) flags = (flags & ~MM_TOPOLOGY_USABLE) | MM_TOPOLOGY_RESERVED;
        if (event.position > previous && flags) {
            if (flags & MM_TOPOLOGY_RAM) total += event.position - previous;
            if (used && ranges[used - 1].end == previous && ranges[used - 1].flags == flags)
                ranges[used - 1].end = event.position;
            else {
                if (used == range_capacity) return -ENOSPC;
                ranges[used++] = (struct mm_topology_range){previous, event.position, flags};
            }
        }
        previous = event.position;
        if (event.delta > 0) ++counts[event.kind];
        else {
            if (!counts[event.kind]) return -EINVAL;
            --counts[event.kind];
        }
    }
    *out = (struct mm_topology){ranges, used, range_capacity, limit, total, clipped};
    return 0;
}

/**
 * @brief Revoke a view without releasing caller-owned workspace bytes.
 * @param topology Writable topology view, or NULL.
 */
void mm_topology_destroy(struct mm_topology *topology)
{
    if (topology) *topology = (struct mm_topology){0};
}

/**
 * @brief Insert one range into spare workspace capacity and shift the suffix.
 * @param topology Writable topology with at least one unused range slot.
 * @param index Insertion index, at most range_count.
 * @param range Range value to insert.
 */
static void insert_range(struct mm_topology *topology, size_t index,
                         struct mm_topology_range range)
{
    for (size_t i = topology->range_count; i > index; --i)
        topology->ranges[i] = topology->ranges[i - 1];
    topology->ranges[index] = range;
    ++topology->range_count;
}

/**
 * @brief Overlay a reservation while retaining all existing trusted RAM flags.
 * @param topology Writable workspace-backed view with spare capacity.
 * @param start Inclusive physical start, rounded down.
 * @param end Exclusive physical end, rounded up and clipped to the limit.
 * @param extra Additional MM_TOPOLOGY_BOOTSTRAP flag, or zero.
 * @return 0 or negative errno; a capacity failure leaves the view unchanged.
 */
int mm_topology_reserve(struct mm_topology *topology, uint64_t start,
                        uint64_t end, uint32_t extra)
{
    if (!topology || !topology->ranges || end < start ||
        (extra & ~MM_TOPOLOGY_BOOTSTRAP)) return -EINVAL;
    if (start == end || start >= topology->physical_limit) return 0;
    if (end > topology->physical_limit) end = topology->physical_limit;
    start &= ~(MM_TOPOLOGY_PAGE_SIZE - 1);
    end = (end + MM_TOPOLOGY_PAGE_SIZE - 1) & ~(MM_TOPOLOGY_PAGE_SIZE - 1);
    size_t additional = 0;
    uint64_t cursor = start;
    for (size_t i = 0; i < topology->range_count && cursor < end; ++i) {
        struct mm_topology_range r = topology->ranges[i];
        if (r.end <= cursor) continue;
        if (r.start >= end) break;
        if (cursor < r.start) { ++additional; cursor = r.start; }
        if (r.start < cursor) ++additional;
        if (r.end > end) ++additional;
        cursor = r.end < end ? r.end : end;
    }
    if (cursor < end) ++additional;
    if (additional > topology->range_capacity - topology->range_count) return -ENOSPC;
    cursor = start;
    while (cursor < end) {
        size_t i = 0;
        while (i < topology->range_count && topology->ranges[i].end <= cursor) ++i;
        if (i == topology->range_count || topology->ranges[i].start > cursor) {
            uint64_t gap_end = i < topology->range_count && topology->ranges[i].start < end ?
                               topology->ranges[i].start : end;
            insert_range(topology, i, (struct mm_topology_range){cursor, gap_end,
                         MM_TOPOLOGY_RESERVED | extra});
            cursor = gap_end;
            continue;
        }
        struct mm_topology_range r = topology->ranges[i];
        if (r.start < cursor) {
            topology->ranges[i].end = cursor;
            r.start = cursor;
            insert_range(topology, ++i, r);
        }
        if (r.end > end) {
            topology->ranges[i].end = end;
            insert_range(topology, i + 1, (struct mm_topology_range){end, r.end, r.flags});
        }
        topology->ranges[i].flags = (r.flags & ~MM_TOPOLOGY_USABLE) |
                                    MM_TOPOLOGY_RESERVED | extra;
        cursor = topology->ranges[i].end;
    }
    for (size_t i = 1; i < topology->range_count;) {
        struct mm_topology_range *previous = &topology->ranges[i - 1];
        struct mm_topology_range *current = &topology->ranges[i];
        if (previous->end == current->start && previous->flags == current->flags) {
            previous->end = current->end;
            for (size_t j = i + 1; j < topology->range_count; ++j)
                topology->ranges[j - 1] = topology->ranges[j];
            --topology->range_count;
        } else ++i;
    }
    return 0;
}
