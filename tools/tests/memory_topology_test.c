#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/memory_topology.h>
#include <reliefnt/multiboot2.h>
#include "../../mm/physical/topology_internal.h"

#define PAGE 4096ULL
#define ARENA_BASE 0x30000000ULL
#define ARENA_PAGES 256u
static _Alignas(4096) unsigned char arena[ARENA_PAGES * PAGE];
static struct framebuffer display;
static uint64_t direct_base = ARENA_BASE;
static uintptr_t borrowed_map;
static size_t borrowed_map_bytes;
static uint64_t fixture_acpi_start, fixture_acpi_bytes;

/** @brief Count the optional fixture ACPI reservation.
 * @return One when present, otherwise zero.
 */
uint32_t power_acpi_resource_count(void) { return fixture_acpi_bytes ? 1 : 0; }
/** @brief Return the optional fixture ACPI reservation.
 * @param index Resource index. @param start Required base output.
 * @param bytes Required length output.
 * @return True for the only present fixture resource.
 */
bool power_acpi_resource(uint32_t index, uint64_t *start, uint64_t *bytes)
{
    if (index || !fixture_acpi_bytes || !start || !bytes) return false;
    *start = fixture_acpi_start; *bytes = fixture_acpi_bytes; return true;
}

/**
 * @brief Supply the host fixture's early framebuffer descriptor.
 * @return Borrowed process-lifetime descriptor; no allocation or ownership transfer.
 */
const struct framebuffer *framebuffer_get(void) { return &display; }
/**
 * @brief Test complete reachability in the host-backed arena or registered map.
 * @param phys Inclusive emulated physical start address.
 * @param len Byte length of the requested range.
 * @return True only if every requested byte fits one fixture mapping.
 * Host fixture only; no mapping or ownership change.
 */
bool paging_kernel_direct_map_range(uint64_t phys, uint64_t len)
{
    if (phys >= borrowed_map && phys - borrowed_map <= borrowed_map_bytes &&
        len <= borrowed_map_bytes - (phys - borrowed_map)) return true;
    return phys >= direct_base && phys - direct_base <= sizeof(arena) &&
           len <= sizeof(arena) - (phys - direct_base);
}
/** @brief Find the fixture arena's mapped overlap with a physical interval.
 * @param start Inclusive aligned search start. @param end Exclusive search end.
 * @param mapped_start Required first mapped page output.
 * @param mapped_end Required exclusive mapped end output.
 * @return True for a nonempty arena overlap, false when absent.
 */
bool paging_kernel_direct_map_next(uint64_t start, uint64_t end,
                                   uint64_t *mapped_start, uint64_t *mapped_end)
{
    if (!mapped_start || !mapped_end || start >= end) return false;
    uint64_t first = start > direct_base ? start : direct_base;
    uint64_t last = end < direct_base + sizeof(arena) ? end : direct_base + sizeof(arena);
    if (first >= last) return false;
    *mapped_start = first;
    *mapped_end = last;
    return true;
}
/**
 * @brief Translate an emulated arena address into host-owned fixture storage.
 * @param phys Physical byte address within the emulated arena.
 * @return Borrowed fixture pointer for a reachable arena byte, otherwise NULL.
 * Host fixture only; the caller must retain and serialize the arena.
 */
void *paging_kernel_direct_map(uint64_t phys)
{
    return phys >= direct_base && phys - direct_base < sizeof(arena) ?
           arena + (phys - direct_base) : NULL;
}
/**
 * @brief Construct a borrowed Multiboot map and register its host-readable extent.
 * @param entries Readable descriptor storage owned by the calling fixture.
 * @param count Number of descriptor records in entries.
 * @param stride Bytes between descriptor records.
 * @return Decoded boot value borrowing entries until the fixture finishes.
 * Host-only fixture; updates the registered mapping without transferring ownership.
 */
static struct boot_info mb(const void *entries, uint32_t count, uint32_t stride)
{
    struct boot_info b = {0};
    borrowed_map = (uintptr_t)entries;
    borrowed_map_bytes = (size_t)count * stride;
    b.mmap_addr = (uintptr_t)entries;
    b.mmap_entry_count = count;
    b.mmap_entry_size = stride;
    return b;
}
/**
 * @brief Build a real topology into exactly sized host-owned workspace.
 * @param b Borrowed readable boot handoff and descriptor storage.
 * @param bits CPU physical address width supplied to the production builder.
 * @param memory Required output receiving allocated workspace for caller to free.
 * @return Workspace-backed topology view; aborts if sizing or construction fails.
 * Host fixture only; the view expires when the caller frees *memory.
 */
static struct mm_topology build(const struct boot_info *b, uint8_t bits, void **memory)
{
    size_t bytes = 0;
    assert(mm_topology_workspace_size(b, &bytes) == 0 && bytes);
    *memory = malloc(bytes);
    assert(*memory);
    struct mm_topology result = {0};
    assert(mm_topology_build(b, bits, *memory, bytes, &result) == 0);
    return result;
}
/**
 * @brief Assert topology ordering, alignment, flags, and trusted RAM accounting.
 * @param t Borrowed stable topology view whose workspace remains live.
 * Host fixture aborts on the first violated invariant.
 */
static void valid(const struct mm_topology *t)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < t->range_count; ++i) {
        const struct mm_topology_range *r = &t->ranges[i];
        assert(r->start < r->end && !(r->start % PAGE) && !(r->end % PAGE));
        assert(r->end <= t->physical_limit);
        if (i) {
            assert(t->ranges[i - 1].end <= r->start);
            assert(t->ranges[i - 1].end != r->start || t->ranges[i - 1].flags != r->flags);
        }
        if (r->flags & MM_TOPOLOGY_RAM) sum += r->end - r->start;
        if (r->flags & MM_TOPOLOGY_USABLE)
            assert((r->flags & (MM_TOPOLOGY_RAM | MM_TOPOLOGY_RESERVED)) == MM_TOPOLOGY_RAM);
    }
    assert(sum == t->total_ram_bytes);
}
/**
 * @brief Look up the flags covering a physical byte in a topology view.
 * @param t Borrowed stable topology view whose workspace remains live.
 * @param addr Physical byte address to query.
 * @return Covering range flags or zero if addr is outside all ranges.
 * Host fixture only; does not alter the topology or transfer ownership.
 */
static uint32_t at(const struct mm_topology *t, uint64_t addr)
{
    for (size_t i = 0; i < t->range_count; ++i)
        if (addr >= t->ranges[i].start && addr < t->ranges[i].end) return t->ranges[i].flags;
    return 0;
}
/**
 * @brief Verify sparse extents, overlap union and limits at physical boundaries.
 * Host-process fixture aborts if an assertion fails.
 */
static void sparse(void)
{
    struct multiboot2_mmap_entry e[] = {
        {0x100000, 0x2000, 1, 0}, {0x102000, 0x2000, 1, 0},
        {0x100000, 0x2000, 1, 0}, {0xffffe000ULL, 0x4000, 1, 0},
        {0x3ffffe000ULL, 0x4000, 1, 0}, {0x100000010000ULL, 0x2000, 1, 0},
    };
    struct boot_info b = mb(e, 6, sizeof(e[0]));
    void *storage;
    struct mm_topology t = build(&b, 48, &storage);
    valid(&t);
    assert(t.physical_limit == 0x400000000000ULL && t.clipped_descriptors == 0);
    assert(t.total_ram_bytes == 14 * PAGE && t.range_count == 6);
    assert(at(&t, 0) == MM_TOPOLOGY_RESERVED);
    assert(at(&t, 0x100000) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE));
    assert(at(&t, 0x103000) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE));
    assert(at(&t, 0x104000) == 0);
    assert(at(&t, 0x8000000) == MM_TOPOLOGY_RESERVED);
    assert(at(&t, 0xfffff000) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE));
    assert(at(&t, 0x100001000ULL) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE));
    assert(at(&t, 0x400000000ULL) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE));
    assert(at(&t, 0x100000010000ULL) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE));
    mm_topology_destroy(&t); assert(!t.ranges && !t.range_count); free(storage);
    t = build(&b, 32, &storage);
    valid(&t);
    assert(t.physical_limit == 0x100000000ULL && t.clipped_descriptors == 3);
    assert(t.total_ram_bytes == 6 * PAGE);
    free(storage);
}
/**
 * @brief Ensure partial-page exclusions, modules, framebuffer and reserved precedence.
 * Host-process fixture aborts if an assertion fails.
 */
static void reserve(void)
{
    struct multiboot2_mmap_entry e[] = {{0x200001, 0xa233, 1, 0}, {0x204123, 0x1700, 2, 0}};
    struct boot_info b = mb(e, 2, sizeof(e[0]));
    b.module_count = 1; b.modules[0].start = 0x202123; b.modules[0].end = 0x203010;
    b.framebuffer_addr = 0x207800; b.framebuffer_pitch = 4096; b.framebuffer_height = 1;
    void *storage;
    struct mm_topology t = build(&b, 48, &storage);
    valid(&t);
    assert(t.total_ram_bytes == 7 * PAGE);
    assert(at(&t, 0x200000) == 0);
    assert(at(&t, 0x201000) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE));
    assert(at(&t, 0x202000) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_RESERVED));
    assert(at(&t, 0x203000) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_RESERVED));
    assert(at(&t, 0x204000) == MM_TOPOLOGY_RESERVED);
    assert(at(&t, 0x205000) == MM_TOPOLOGY_RESERVED);
    assert(at(&t, 0x206000) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE));
    assert(at(&t, 0x207000) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_RESERVED));
    assert(at(&t, 0x208000) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_RESERVED));
    assert(at(&t, 0x209000) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE));
    assert(at(&t, 0x20a000) == 0);
    free(storage);
}
/**
 * @brief Verify extended unaligned EFI strides and conservative firmware types.
 * Host-process fixture aborts if an assertion fails.
 */
static void efi(void)
{
    unsigned char raw[5 * 41] = {0};
    struct efi_memory_descriptor d[] = {
        {.type=7,.physical_start=0x1000000,.number_of_pages=4},
        {.type=9,.physical_start=0x1001000,.number_of_pages=1},
        {.type=7,.physical_start=0x1002000,.number_of_pages=1,.attribute=1ULL<<63},
        {.type=4,.physical_start=0x3000000,.number_of_pages=1},
        {.type=7,.physical_start=0x4000000,.number_of_pages=1},
    };
    for (unsigned i = 0; i < 5; ++i) memcpy(raw + 41 * i, &d[i], sizeof(d[i]));
    struct multiboot2_mmap_entry stale = {0x5000000, PAGE, 1, 0};
    struct boot_info b = mb(&stale, 1, sizeof(stale));
    b.efi_mmap_addr = (uintptr_t)raw; b.efi_mmap_entry_count = 5; b.efi_mmap_entry_size = 41;
    void *storage;
    struct mm_topology t = build(&b, 48, &storage);
    valid(&t);
    assert(t.total_ram_bytes == 5 * PAGE);
    assert(at(&t, 0x1000000) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE));
    assert(at(&t, 0x1001000) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_RESERVED | MM_TOPOLOGY_RECLAIMABLE));
    assert(at(&t, 0x1002000) == MM_TOPOLOGY_RESERVED);
    assert(at(&t, 0x1003000) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE));
    assert(at(&t, 0x3000000) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_RESERVED | MM_TOPOLOGY_RECLAIMABLE));
    assert(at(&t, 0x4000000) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE));
    assert(at(&t, 0x5000000) == 0);
    free(storage);
}
/**
 * @brief Reject missing, overflowing and insufficient inputs without publishing a view.
 * Host-process fixture aborts if an assertion fails.
 */
static void errors(void)
{
    struct boot_info b = {0}; size_t n = 123;
    assert(mm_topology_workspace_size(&b, &n) < 0 && !n);
    struct multiboot2_mmap_entry e = {0x100000, PAGE, 1, 0};
    b = mb(&e, 1, sizeof(e));
    assert(mm_topology_workspace_size(&b, &n) == 0 && n);
    void *storage = malloc(n); assert(storage);
    struct mm_topology t; memset(&t, 0xa5, sizeof(t));
    assert(mm_topology_build(&b, 48, storage, n-1, &t) < 0 && !t.ranges && !t.range_count);
    assert(mm_topology_build(&b, 31, storage, n, &t) < 0 && !t.ranges);
    assert(mm_topology_build(&b, 53, storage, n, &t) < 0 && !t.ranges);
    b.mmap_entry_size = 23; assert(mm_topology_workspace_size(&b, &n) < 0);
    b = mb((void *)(uintptr_t)(UINTPTR_MAX-7), 2, 24);
    assert(mm_topology_workspace_size(&b, &n) < 0);
    b = mb(&e, 1, sizeof(e)); e.addr = UINT64_MAX-1023; e.len = 4096;
    assert(mm_topology_workspace_size(&b, &n) < 0);
    struct mm_topology failed = {.ranges = (void *)1, .range_count = 99};
    assert(mm_topology_build(&b, 48, storage, 4096, &failed) < 0);
    assert(!failed.ranges && !failed.range_count);
    struct efi_memory_descriptor huge = {.type=7,.physical_start=PAGE,
                                         .number_of_pages=UINT64_MAX/PAGE};
    struct boot_info efib = {0};
    efib.efi_mmap_addr = (uintptr_t)&huge; efib.efi_mmap_entry_count = 1;
    efib.efi_mmap_entry_size = sizeof(huge);
    assert(mm_topology_workspace_size(&efib, &n) == -EOVERFLOW);
    b.mmap_addr = (uintptr_t)&e; b.mmap_entry_count = UINT32_MAX;
    b.mmap_entry_size = UINT32_MAX;
    assert(mm_topology_workspace_size(&b, &n) == -EOVERFLOW);
    b = mb(&e, 1, sizeof(e));
    e.addr = 0x100000; e.len = PAGE;
    b.module_count = 17; assert(mm_topology_workspace_size(&b, &n) < 0);
    b.module_count = 1; b.modules[0].start = 2; b.modules[0].end = 1;
    assert(mm_topology_workspace_size(&b, &n) < 0);
    free(storage);
}
/**
 * @brief Admit more than 256 sparse ranges with metadata proportional to entries.
 * Host-process fixture aborts if an assertion fails.
 */
static void many(void)
{
    enum { N = 320 };
    struct multiboot2_mmap_entry e[N];
    for (unsigned i = 0; i < N; ++i)
        e[i] = (struct multiboot2_mmap_entry){0x20000000ULL + 2 * PAGE * i, PAGE, 1, 0};
    struct boot_info b = mb(e, N, sizeof(e[0])); size_t low_bytes, high_bytes;
    assert(mm_topology_workspace_size(&b, &low_bytes) == 0);
    e[N-1].addr = 0x300000000000ULL;
    assert(mm_topology_workspace_size(&b, &high_bytes) == 0 && high_bytes == low_bytes);
    void *storage;
    struct mm_topology t = build(&b, 48, &storage); valid(&t);
    assert(t.total_ram_bytes == N * PAGE && t.range_count == N + 2);
    for (unsigned i = 0; i < N; ++i)
        assert(at(&t, e[i].addr) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE));
    free(storage);
}
/**
 * @brief Allocate only once-owned mapped pages while preserving every reservation.
 * Host-process fixture aborts if an assertion fails.
 */
static void bootstrap(void)
{
    struct multiboot2_mmap_entry e[] = {
        {ARENA_BASE, sizeof(arena), 1, 0}, {ARENA_BASE + 2*PAGE, PAGE, 2, 0},
    };
    struct boot_info b = mb(e, 2, sizeof(e[0]));
    b.module_count = 1; b.modules[0].start = ARENA_BASE+5*PAGE; b.modules[0].end = ARENA_BASE+7*PAGE;
    b.framebuffer_addr = ARENA_BASE+9*PAGE; b.framebuffer_pitch = PAGE; b.framebuffer_height = 2;
    display.auxiliary_reservation_start = ARENA_BASE+13*PAGE; display.auxiliary_reservation_bytes = PAGE;
    assert(mm_bootstrap_prepare(&b, 48) == 0);
    const struct mm_topology *t = mm_bootstrap_topology(); assert(t); valid(t);
    assert(at(t, ARENA_BASE+2*PAGE) == MM_TOPOLOGY_RESERVED);
    assert(at(t, ARENA_BASE+5*PAGE) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_RESERVED));
    assert(at(t, ARENA_BASE+9*PAGE) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_RESERVED));
    assert(at(t, ARENA_BASE+13*PAGE) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_RESERVED));
    size_t available = 0, initial_owned = 0;
    for (unsigned i = 0; i < ARENA_PAGES; ++i) {
        uint32_t f = at(t, ARENA_BASE + i*PAGE);
        if (f & MM_TOPOLOGY_USABLE) ++available;
        if (f & MM_TOPOLOGY_BOOTSTRAP) ++initial_owned;
    }
    assert(initial_owned && available);
    assert(mm_bootstrap_alloc(0, UINT64_MAX) == 0);
    assert(mm_bootstrap_alloc(UINT32_MAX, UINT64_MAX) == 0);
    assert(mm_bootstrap_alloc(1, ARENA_BASE + PAGE - 2) == 0);
    /* Workspace occupies page zero; mask applies to the entire next page. */
    assert(mm_bootstrap_alloc(1, ARENA_BASE + 2*PAGE - 2) == 0);
    assert(mm_bootstrap_alloc(1, ARENA_BASE + 2*PAGE - 1) == ARENA_BASE + PAGE);
    assert(mm_bootstrap_alloc(1, ARENA_BASE + 2*PAGE - 1) == 0);
    bool seen[ARENA_PAGES] = {0}; seen[1] = true;
    size_t allocated = 1; uint64_t p;
    while ((p = mm_bootstrap_alloc(1, UINT64_MAX)) != 0) {
        assert(p >= ARENA_BASE && p < ARENA_BASE + sizeof(arena) && !(p % PAGE));
        unsigned i = (unsigned)((p - ARENA_BASE)/PAGE);
        assert(!seen[i]); seen[i] = true; ++allocated;
        assert((at(t, p) & (MM_TOPOLOGY_RAM|MM_TOPOLOGY_RESERVED|MM_TOPOLOGY_BOOTSTRAP)) ==
               (MM_TOPOLOGY_RAM|MM_TOPOLOGY_RESERVED|MM_TOPOLOGY_BOOTSTRAP));
    }
    assert(allocated == available); valid(t);
    mm_bootstrap_finish();
    assert(mm_bootstrap_topology() == t && mm_bootstrap_alloc(1, UINT64_MAX) == 0);
    for (unsigned i = 0; i < ARENA_PAGES; ++i)
        if (seen[i]) assert(at(t, ARENA_BASE+i*PAGE) & MM_TOPOLOGY_BOOTSTRAP);
    assert(mm_bootstrap_prepare(&b, 48) < 0);
}
/**
 * @brief Fail when metadata cannot fit and when no reachable page remains.
 * Host-process fixture aborts if an assertion fails.
 */
static void exhaust(void)
{
    struct multiboot2_mmap_entry e[320] = {{ARENA_BASE, PAGE, 1, 0}};
    struct boot_info b = mb(e, 320, sizeof(e[0]));
    assert(mm_bootstrap_prepare(&b, 48) < 0 && !mm_bootstrap_topology());
    b.mmap_entry_count = 1;
    assert(mm_bootstrap_prepare(&b, 48) == 0);
    assert(mm_bootstrap_alloc(1, UINT64_MAX) == 0);
    mm_bootstrap_finish();
}
/**
 * @brief Keep unaligned extended Multiboot entries and stale reservations exact.
 * Host-process fixture aborts if an assertion fails.
 */
static void extended(void)
{
    unsigned char raw[3 * 25] = {0};
    struct multiboot2_mmap_entry d[] = {
        {0x2100000, 4*PAGE, 1, 0}, {0x2101000, PAGE, 2, 0},
        {0x2103000, PAGE, 3, 0},
    };
    for (unsigned i = 0; i < 3; ++i) memcpy(raw + i*25, &d[i], sizeof(d[i]));
    struct boot_info b = mb(raw, 3, 25);
    void *storage;
    struct mm_topology t = build(&b, 48, &storage); valid(&t);
    assert(at(&t, 0x2100000) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_USABLE));
    assert(at(&t, 0x2101000) == MM_TOPOLOGY_RESERVED);
    assert(at(&t, 0x2102000) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_USABLE));
    assert(at(&t, 0x2103000) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_RESERVED|MM_TOPOLOGY_RECLAIMABLE));
    free(storage);
    struct efi_memory_descriptor e = {.type=7,.physical_start=0x2100000,.number_of_pages=4};
    b.efi_mmap_addr = (uintptr_t)&e; b.efi_mmap_entry_count = 1;
    b.efi_mmap_entry_size = sizeof(e);
    t = build(&b, 48, &storage); valid(&t);
    assert(at(&t, 0x2101000) == MM_TOPOLOGY_RESERVED);
    assert(at(&t, 0x2103000) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_RESERVED|MM_TOPOLOGY_RECLAIMABLE));
    free(storage);
    e.number_of_pages = UINT64_MAX;
    size_t bytes = 100;
    assert(mm_topology_workspace_size(&b, &bytes) == -EOVERFLOW && bytes == 0);
}
/**
 * @brief Compare each page against an independent overlap oracle over sparse input.
 * Host-process fixture aborts if an assertion fails.
 */
static void conservation(void)
{
    enum { N = 80, PAGES = 128 };
    const uint64_t base = 0x20000000;
    struct multiboot2_mmap_entry entries[N];
    uint32_t seed = 0x189c03u;
    for (unsigned i = 0; i < N; ++i) {
        seed = seed * 1664525u + 1013904223u;
        uint64_t start = base + (seed % (PAGES * PAGE));
        seed = seed * 1664525u + 1013904223u;
        uint64_t length = 1 + (seed % (PAGE * 9));
        seed = seed * 1664525u + 1013904223u;
        uint32_t type = (seed % 3) + 1;
        entries[i] = (struct multiboot2_mmap_entry){start, length, type, 0};
    }
    struct boot_info b = mb(entries, N, sizeof(entries[0]));
    void *storage;
    struct mm_topology t = build(&b, 48, &storage); valid(&t);
    for (unsigned i = 0; i < PAGES + 10; ++i) {
        uint64_t page = base + i*PAGE;
        unsigned char usable_bytes[PAGE] = {0};
        bool reserved = false, reclaim = false;
        for (unsigned j = 0; j < N; ++j) {
            uint64_t end = entries[j].addr + entries[j].len;
            if (entries[j].addr >= page + PAGE || end <= page) continue;
            if (entries[j].type == 1) {
                uint64_t lo = entries[j].addr > page ? entries[j].addr : page;
                uint64_t hi = end < page + PAGE ? end : page + PAGE;
                memset(usable_bytes + (lo - page), 1, (size_t)(hi - lo));
            } else if (entries[j].type == 3) reclaim = true;
            else reserved = true;
        }
        bool usable = true;
        for (size_t j = 0; j < PAGE; ++j)
            if (!usable_bytes[j]) { usable = false; break; }
        uint32_t want = reserved ? MM_TOPOLOGY_RESERVED :
                        reclaim ? MM_TOPOLOGY_RAM|MM_TOPOLOGY_RECLAIMABLE|MM_TOPOLOGY_RESERVED :
                        usable ? MM_TOPOLOGY_RAM|MM_TOPOLOGY_USABLE : 0;
        assert(at(&t, page) == want);
    }
    free(storage);
}
/**
 * @brief Keep stale Multiboot reservations ahead of newer EFI usable claims.
 * Host-process fixture aborts if an assertion fails.
 */
static void source_conflict(void)
{
    struct multiboot2_mmap_entry old = {0x2201000, PAGE, 2, 0};
    struct efi_memory_descriptor final = {.type=7,.physical_start=0x2200000,.number_of_pages=3};
    struct boot_info b = mb(&old, 1, sizeof(old));
    b.efi_mmap_addr = (uintptr_t)&final; b.efi_mmap_entry_count = 1;
    b.efi_mmap_entry_size = sizeof(final);
    void *storage; struct mm_topology t = build(&b, 48, &storage); valid(&t);
    assert(at(&t, 0x2200000) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_USABLE));
    assert(at(&t, 0x2201000) == MM_TOPOLOGY_RESERVED);
    assert(at(&t, 0x2202000) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_USABLE));
    assert(t.total_ram_bytes == 2 * PAGE);
    free(storage);
}
/**
 * @brief Honor a 32-bit mask across the last complete page below 4 GiB.
 * Host-process fixture aborts if an assertion fails.
 */
static void mask_4g(void)
{
    direct_base = 0xffffc000ULL;
    struct multiboot2_mmap_entry d = {direct_base, 8*PAGE, 1, 0};
    struct boot_info b = mb(&d, 1, sizeof(d));
    assert(mm_bootstrap_prepare(&b, 48) == 0);
    assert(mm_bootstrap_alloc(4, UINT32_MAX) == 0);
    assert(mm_bootstrap_alloc(3, UINT32_MAX) == 0xffffd000ULL);
    assert(mm_bootstrap_alloc(1, UINT32_MAX) == 0);
    assert(mm_bootstrap_alloc(1, UINT64_MAX) == 0x100000000ULL);
    valid(mm_bootstrap_topology());
    mm_bootstrap_finish();
}
/**
 * @brief Refuse input whose complete map buffer is outside the current mapping.
 * Host-process fixture aborts if an assertion fails.
 */
static void unreachable(void)
{
    struct multiboot2_mmap_entry e = {ARENA_BASE, sizeof(arena), 1, 0};
    struct boot_info b = mb(&e, 1, sizeof(e));
    borrowed_map_bytes = sizeof(e) - 1;
    assert(mm_bootstrap_prepare(&b, 48) == -EFAULT);
    assert(mm_bootstrap_topology() == NULL);
}
/**
 * @brief Keep the raw map itself reserved before choosing bootstrap workspace.
 * Host-process fixture aborts if an assertion fails.
 */
static void map_collision(void)
{
    void *raw = mmap((void *)(uintptr_t)ARENA_BASE, PAGE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(raw == (void *)(uintptr_t)ARENA_BASE);
    struct multiboot2_mmap_entry d = {ARENA_BASE, sizeof(arena), 1, 0};
    memcpy(raw, &d, sizeof(d));
    struct boot_info b = mb(raw, 1, sizeof(d));
    assert(mm_bootstrap_prepare(&b, 48) == 0);
    const struct mm_topology *t = mm_bootstrap_topology();
    assert(at(t, ARENA_BASE) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_RESERVED));
    assert(at(t, ARENA_BASE+PAGE) & MM_TOPOLOGY_BOOTSTRAP);
    assert(mm_bootstrap_alloc(1, UINT64_MAX) == ARENA_BASE+2*PAGE);
    assert(memcmp(raw, &d, sizeof(d)) == 0);
    assert(munmap(raw, PAGE) == 0);
}
/**
 * @brief Reserve an entire real parsed handoff including borrowed strings and module.
 * Host-process fixture aborts if an assertion fails.
 */
static void boot_block(void)
{
    unsigned char *raw = mmap((void *)(uintptr_t)ARENA_BASE, PAGE, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(raw == (void *)(uintptr_t)ARENA_BASE);
    struct multiboot2_info *info = (void *)raw; info->total_size = 96;
    struct multiboot2_tag_string *cmd = (void *)(raw + 8);
    cmd->type = MULTIBOOT2_TAG_TYPE_CMDLINE; cmd->size = 13;
    memcpy(cmd->string, "root", 5);
    struct multiboot2_tag_mmap *map = (void *)(raw + 24);
    map->type = MULTIBOOT2_TAG_TYPE_MMAP; map->size = 40; map->entry_size = 24;
    map->entries[0] = (struct multiboot2_mmap_entry){ARENA_BASE, 8*PAGE, 1, 0};
    struct multiboot2_tag_module *mod = (void *)(raw + 64);
    mod->type = MULTIBOOT2_TAG_TYPE_MODULE; mod->size = 18;
    mod->mod_start = ARENA_BASE+3*PAGE; mod->mod_end = ARENA_BASE+4*PAGE;
    memcpy(mod->string, "x", 2);
    struct multiboot2_tag *end = (void *)(raw + 88);
    end->type = MULTIBOOT2_TAG_TYPE_END; end->size = 8;
    struct boot_info b;
    assert(multiboot2_parse(MULTIBOOT2_BOOTLOADER_MAGIC, (uintptr_t)raw, &b) == 0);
    borrowed_map = (uintptr_t)raw; borrowed_map_bytes = PAGE;
    assert(mm_bootstrap_prepare(&b, 48) == 0);
    const struct mm_topology *t = mm_bootstrap_topology();
    assert(at(t, ARENA_BASE) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_RESERVED));
    assert(at(t, ARENA_BASE+3*PAGE) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_RESERVED));
    assert(mm_bootstrap_alloc(1, UINT64_MAX) == ARENA_BASE+2*PAGE);
    assert(mm_bootstrap_alloc(1, UINT64_MAX) == ARENA_BASE+4*PAGE);
    assert(!strcmp(b.cmdline, "root") && !strcmp(b.modules[0].name, "x"));
    valid(t); mm_bootstrap_finish();
}
/**
 * @brief Empty Multiboot tag must not hide a valid populated EFI map.
 * Host-process fixture aborts if an assertion fails.
 */
static void empty_secondary(void)
{
    unsigned char empty[1] = {0};
    struct efi_memory_descriptor d = {.type=7,.physical_start=0x2600000,.number_of_pages=1};
    struct boot_info b = {0};
    b.mmap_addr = (uintptr_t)empty; b.mmap_entry_size = 24;
    b.efi_mmap_addr = (uintptr_t)&d; b.efi_mmap_entry_size = sizeof(d); b.efi_mmap_entry_count = 1;
    void *storage; struct mm_topology t = build(&b, 48, &storage); valid(&t);
    assert(at(&t, 0x2600000) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_USABLE));
    free(storage);
}
/**
 * @brief A framebuffer based at physical zero still reserves its full extent.
 * Host-process fixture aborts if an assertion fails.
 */
static void zero_framebuffer(void)
{
    struct multiboot2_mmap_entry d = {0x100000, 0x200000, 1, 0};
    struct boot_info b = mb(&d, 1, sizeof(d));
    b.framebuffer_addr = 0; b.framebuffer_pitch = 4096; b.framebuffer_height = 512;
    void *storage; struct mm_topology t = build(&b, 48, &storage); valid(&t);
    assert(at(&t, 0x100000) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_RESERVED));
    assert(at(&t, 0x1ff000) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_RESERVED));
    assert(at(&t, 0x200000) == (MM_TOPOLOGY_RAM|MM_TOPOLOGY_USABLE));
    free(storage);
}
/**
 * @brief Reject an unrepresentable reservation without changing any range.
 * Host-process fixture aborts if an assertion fails.
 */
static void capacity(void)
{
    struct mm_topology_range ranges[3] = {
        {0x200000, 0x206000, MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE},
        {0xdead000, 0xdead000, 0xdead}, {0xbeef000, 0xbeef000, 0xbeef},
    };
    struct mm_topology_range before[3];
    memcpy(before, ranges, sizeof(ranges));
    struct mm_topology t = {ranges, 1, 1, 0x100000000ULL, 6 * PAGE, 0};
    assert(mm_topology_reserve(&t, 0x202000, 0x203000, MM_TOPOLOGY_BOOTSTRAP) == -ENOSPC);
    assert(t.range_count == 1 && memcmp(ranges, before, sizeof(ranges)) == 0);
}
/**
 * @brief Admit a page proved usable by two adjacent partial descriptors.
 * Host-process fixture aborts if an assertion fails.
 */
static void partial_adjacent(void)
{
    struct multiboot2_mmap_entry e[] = {
        {ARENA_BASE, PAGE/2, 1, 0}, {ARENA_BASE + PAGE/2, PAGE/2, 1, 0},
    };
    struct boot_info b = mb(e, 2, sizeof(e[0])); void *storage;
    struct mm_topology t = build(&b, 48, &storage); valid(&t);
    assert(at(&t, ARENA_BASE) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE));
    assert(t.total_ram_bytes == PAGE);
    free(storage);
}
/**
 * @brief Admit a page proved usable by overlapping partial descriptors.
 * Host-process fixture aborts if an assertion fails.
 */
static void partial_overlap(void)
{
    struct multiboot2_mmap_entry e[] = {
        {ARENA_BASE + PAGE/2, PAGE/2, 1, 0}, {ARENA_BASE, 3*PAGE/4, 1, 0},
    };
    struct boot_info b = mb(e, 2, sizeof(e[0])); void *storage;
    struct mm_topology t = build(&b, 48, &storage); valid(&t);
    assert(at(&t, ARENA_BASE) == (MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE));
    assert(t.total_ram_bytes == PAGE);
    free(storage);
}
/**
 * @brief A one-byte gap or one reserved byte prevents allocating a page.
 * Host-process fixture aborts if an assertion fails.
 */
static void partial_gap_reserved(void)
{
    struct multiboot2_mmap_entry e[] = {
        {ARENA_BASE, PAGE/2, 1, 0}, {ARENA_BASE + PAGE/2+1, PAGE/2-1, 1, 0},
        {ARENA_BASE + PAGE, PAGE/2, 1, 0}, {ARENA_BASE + 3*PAGE/2, PAGE/2, 1, 0},
        {ARENA_BASE + PAGE + 17, 1, 2, 0},
    };
    struct boot_info b = mb(e, 5, sizeof(e[0])); void *storage;
    struct mm_topology t = build(&b, 48, &storage); valid(&t);
    assert(at(&t, ARENA_BASE) == 0);
    assert(at(&t, ARENA_BASE + PAGE) == MM_TOPOLOGY_RESERVED);
    assert(t.total_ram_bytes == 0);
    free(storage);
}
/**
 * @brief Bootstrap a workspace spanning unordered adjacent one-page entries.
 * Host-process fixture aborts if an assertion fails.
 */
static void fragmented_adjacent(void)
{
    enum { N = 24 };
    struct multiboot2_mmap_entry e[N];
    for (unsigned i = 0; i < N; ++i)
        e[i] = (struct multiboot2_mmap_entry){ARENA_BASE + ((i*7)%N)*PAGE, PAGE, 1, 0};
    struct boot_info b = mb(e, N, sizeof(e[0])); size_t bytes = 0;
    assert(mm_topology_workspace_size(&b, &bytes) == 0 && bytes > PAGE && bytes <= 2*PAGE);
    assert(mm_bootstrap_prepare(&b, 48) == 0);
    const struct mm_topology *t = mm_bootstrap_topology(); valid(t);
    assert((at(t, ARENA_BASE) & MM_TOPOLOGY_BOOTSTRAP) != 0);
    assert((at(t, ARENA_BASE + PAGE) & MM_TOPOLOGY_BOOTSTRAP) != 0);
    assert(mm_bootstrap_alloc(1, UINT64_MAX) == ARENA_BASE + 2*PAGE);
    mm_bootstrap_finish();
}
/**
 * @brief Bootstrap a workspace spanning overlapping sub-workspace entries.
 * Host-process fixture aborts if an assertion fails.
 */
static void fragmented_overlap(void)
{
    enum { N = 24 };
    struct multiboot2_mmap_entry e[N];
    for (unsigned i = 0; i < N; ++i)
        e[i] = (struct multiboot2_mmap_entry){ARENA_BASE + i*(PAGE/2), PAGE, 1, 0};
    struct boot_info b = mb(e, N, sizeof(e[0])); size_t bytes = 0;
    assert(mm_topology_workspace_size(&b, &bytes) == 0 && bytes > PAGE && bytes <= 2*PAGE);
    assert(mm_bootstrap_prepare(&b, 48) == 0);
    const struct mm_topology *t = mm_bootstrap_topology(); valid(t);
    assert((at(t, ARENA_BASE) & MM_TOPOLOGY_BOOTSTRAP) != 0);
    assert((at(t, ARENA_BASE + PAGE) & MM_TOPOLOGY_BOOTSTRAP) != 0);
    assert(mm_bootstrap_alloc(1, UINT64_MAX) == ARENA_BASE + 2*PAGE);
    mm_bootstrap_finish();
}
/**
 * @brief Gaps between one-page spans cannot hold a two-page workspace.
 * Host-process fixture aborts if an assertion fails.
 */
static void fragmented_gap(void)
{
    enum { N = 24 };
    struct multiboot2_mmap_entry e[N];
    for (unsigned i = 0; i < N; ++i)
        e[i] = (struct multiboot2_mmap_entry){ARENA_BASE + 2*i*PAGE, PAGE, 1, 0};
    struct boot_info b = mb(e, N, sizeof(e[0])); size_t bytes = 0;
    assert(mm_topology_workspace_size(&b, &bytes) == 0 && bytes > PAGE);
    assert(mm_bootstrap_prepare(&b, 48) == -ENOMEM);
    assert(mm_bootstrap_topology() == NULL);
}
/**
 * @brief A one-byte reservation blocks an otherwise two-page workspace.
 * Host-process fixture aborts if an assertion fails.
 */
static void fragmented_reserved(void)
{
    struct multiboot2_mmap_entry e[24] = {
        {ARENA_BASE, 2*PAGE, 1, 0}, {ARENA_BASE + PAGE, 1, 2, 0},
    };
    struct boot_info b = mb(e, 24, sizeof(e[0])); size_t bytes = 0;
    assert(mm_topology_workspace_size(&b, &bytes) == 0 && bytes > PAGE);
    assert(mm_bootstrap_prepare(&b, 48) == -ENOMEM);
    assert(mm_bootstrap_topology() == NULL);
}
/**
 * @brief Skip a reserved byte before choosing a fragmented contiguous workspace.
 * Host-process fixture aborts if an assertion fails.
 */
static void fragmented_skip_reserved(void)
{
    enum { N = 24 };
    struct multiboot2_mmap_entry e[N + 1];
    for (unsigned i = 0; i < N; ++i)
        e[i] = (struct multiboot2_mmap_entry){ARENA_BASE + i*PAGE, PAGE, 1, 0};
    e[N] = (struct multiboot2_mmap_entry){ARENA_BASE + 17, 1, 2, 0};
    struct boot_info b = mb(e, N + 1, sizeof(e[0]));
    assert(mm_bootstrap_prepare(&b, 48) == 0);
    const struct mm_topology *t = mm_bootstrap_topology(); valid(t);
    assert(at(t, ARENA_BASE) == MM_TOPOLOGY_RESERVED);
    assert(at(t, ARENA_BASE + PAGE) & MM_TOPOLOGY_BOOTSTRAP);
    assert(at(t, ARENA_BASE + 2*PAGE) & MM_TOPOLOGY_BOOTSTRAP);
    mm_bootstrap_finish();
}
/**
 * @brief Join fragmented EFI conventional entries while avoiding reserved pages.
 * Host-process fixture aborts if an assertion fails.
 */
static void fragmented_efi(void)
{
    enum { N = 24 };
    struct efi_memory_descriptor e[N + 1];
    for (unsigned i = 0; i < N; ++i)
        e[i] = (struct efi_memory_descriptor){.type=7,
                    .physical_start=ARENA_BASE + ((i*7)%N)*PAGE, .number_of_pages=1};
    e[N] = (struct efi_memory_descriptor){.type=0,.physical_start=ARENA_BASE,.number_of_pages=1};
    struct boot_info b = {0};
    b.efi_mmap_addr = (uintptr_t)e; b.efi_mmap_entry_count = N + 1;
    b.efi_mmap_entry_size = sizeof(e[0]);
    borrowed_map = (uintptr_t)e; borrowed_map_bytes = sizeof(e);
    assert(mm_bootstrap_prepare(&b, 48) == 0);
    const struct mm_topology *t = mm_bootstrap_topology(); valid(t);
    assert(at(t, ARENA_BASE) == MM_TOPOLOGY_RESERVED);
    assert(at(t, ARENA_BASE + PAGE) & MM_TOPOLOGY_BOOTSTRAP);
    assert(at(t, ARENA_BASE + 2*PAGE) & MM_TOPOLOGY_BOOTSTRAP);
    assert(mm_bootstrap_alloc(1, UINT64_MAX) == ARENA_BASE + 3*PAGE);
    mm_bootstrap_finish();
}

/**
 * @brief Reject malformed standard EFI handoff tags before any firmware access.
 * The tag-18 marker has no payload and the tag-20 image handle has eight bytes.
 */
static void malformed_efi_handoff(void)
{
    _Alignas(8) uint8_t raw[48] = {0};
    struct multiboot2_info *info = (void *)raw;
    struct multiboot2_tag *tag = (void *)(raw + 8);
    struct multiboot2_tag *end = (void *)(raw + 24);
    struct boot_info boot;

    info->total_size = 32;
    tag->type = 18;
    tag->size = 16;
    end->type = 0;
    end->size = 8;
    assert(multiboot2_parse(MULTIBOOT2_BOOTLOADER_MAGIC, (uintptr_t)raw, &boot) == -EINVAL);

    tag->type = 20;
    tag->size = 8;
    end = (void *)(raw + 16);
    end->type = 0;
    end->size = 8;
    info->total_size = 24;
    assert(multiboot2_parse(MULTIBOOT2_BOOTLOADER_MAGIC, (uintptr_t)raw, &boot) == -EINVAL);
}

/** @brief Decode the standard live-services marker and 64-bit image handle. */
static void valid_efi_handoff(void)
{
    _Alignas(8) uint8_t raw[40] = {0};
    struct multiboot2_info *info = (void *)raw;
    struct multiboot2_tag *services = (void *)(raw + 8);
    struct multiboot2_tag *handle = (void *)(raw + 16);
    struct multiboot2_tag *end = (void *)(raw + 32);
    uint64_t expected = 0x123456789abcdef0ULL;
    struct boot_info boot;

    info->total_size = sizeof(raw);
    services->type = MULTIBOOT2_TAG_TYPE_EFI_BS;
    services->size = 8;
    handle->type = MULTIBOOT2_TAG_TYPE_EFI64_IH;
    handle->size = 16;
    memcpy(raw + 24, &expected, sizeof(expected));
    end->type = MULTIBOOT2_TAG_TYPE_END;
    end->size = 8;
    assert(multiboot2_parse(MULTIBOOT2_BOOTLOADER_MAGIC, (uintptr_t)raw, &boot) == 0);
    assert(boot.efi_boot_services_active && boot.efi_image_handle == expected);
}

/** @brief Find bootstrap storage after an unmapped prefix of usable RAM. */
static void mapped_workspace_after_hole(void)
{
    struct multiboot2_mmap_entry entry = {ARENA_BASE, 12 * PAGE, 1, 0};
    struct boot_info boot = mb(&entry, 1, sizeof(entry));
    direct_base = ARENA_BASE + 2 * PAGE;
    assert(mm_bootstrap_prepare(&boot, 48) == 0);
    uint64_t page = mm_bootstrap_alloc(1, UINT64_MAX);
    assert(page >= direct_base && page < ARENA_BASE + 12 * PAGE);
}

/** @brief Exclude captured ACPI spans before workspace and runtime allocations. */
static void reserve_acpi_resources(void)
{
    struct multiboot2_mmap_entry entry = {ARENA_BASE, 12 * PAGE, 1, 0};
    struct boot_info boot = mb(&entry, 1, sizeof(entry));
    fixture_acpi_start = ARENA_BASE + 17;
    fixture_acpi_bytes = PAGE;
    assert(mm_bootstrap_prepare(&boot, 48) == 0);
    const struct mm_topology *topology = mm_bootstrap_topology();
    assert((at(topology, ARENA_BASE) & (MM_TOPOLOGY_RESERVED | MM_TOPOLOGY_USABLE)) ==
           MM_TOPOLOGY_RESERVED);
    assert((at(topology, ARENA_BASE + PAGE) & (MM_TOPOLOGY_RESERVED | MM_TOPOLOGY_USABLE)) ==
           MM_TOPOLOGY_RESERVED);
    assert(mm_bootstrap_alloc(1, UINT64_MAX) >= ARENA_BASE + 2 * PAGE);
}
/**
 * @brief Dispatch one production-linked topology or bootstrap host scenario.
 * @param argc Number of command-line arguments; must equal two.
 * @param argv Borrowed argument vector; argv[1] selects one isolated scenario.
 * @return 0 after success, 2 for an unknown name; failed assertions abort.
 * Each invocation owns the process-local mock mappings and bootstrap state.
 */
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "sparse")) sparse();
    else if (!strcmp(argv[1], "reserve")) reserve();
    else if (!strcmp(argv[1], "efi")) efi();
    else if (!strcmp(argv[1], "errors")) errors();
    else if (!strcmp(argv[1], "many")) many();
    else if (!strcmp(argv[1], "bootstrap")) bootstrap();
    else if (!strcmp(argv[1], "exhaust")) exhaust();
    else if (!strcmp(argv[1], "capacity")) capacity();
    else if (!strcmp(argv[1], "source_conflict")) source_conflict();
    else if (!strcmp(argv[1], "mask_4g")) mask_4g();
    else if (!strcmp(argv[1], "boot_block")) boot_block();
    else if (!strcmp(argv[1], "empty_secondary")) empty_secondary();
    else if (!strcmp(argv[1], "zero_framebuffer")) zero_framebuffer();
    else if (!strcmp(argv[1], "extended")) extended();
    else if (!strcmp(argv[1], "conservation")) conservation();
    else if (!strcmp(argv[1], "unreachable")) unreachable();
    else if (!strcmp(argv[1], "map_collision")) map_collision();
    else if (!strcmp(argv[1], "partial_adjacent")) partial_adjacent();
    else if (!strcmp(argv[1], "partial_overlap")) partial_overlap();
    else if (!strcmp(argv[1], "partial_gap_reserved")) partial_gap_reserved();
    else if (!strcmp(argv[1], "fragmented_adjacent")) fragmented_adjacent();
    else if (!strcmp(argv[1], "fragmented_overlap")) fragmented_overlap();
    else if (!strcmp(argv[1], "fragmented_gap")) fragmented_gap();
    else if (!strcmp(argv[1], "fragmented_reserved")) fragmented_reserved();
    else if (!strcmp(argv[1], "fragmented_skip_reserved")) fragmented_skip_reserved();
    else if (!strcmp(argv[1], "fragmented_efi")) fragmented_efi();
    else if (!strcmp(argv[1], "malformed_efi_handoff")) malformed_efi_handoff();
    else if (!strcmp(argv[1], "valid_efi_handoff")) valid_efi_handoff();
    else if (!strcmp(argv[1], "mapped_workspace_after_hole")) mapped_workspace_after_hole();
    else if (!strcmp(argv[1], "reserve_acpi_resources")) reserve_acpi_resources();
    else return 2;
    printf("PASS %s\n", argv[1]);
    return 0;
}
