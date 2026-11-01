/* IRQ-safe physical allocation over bootstrap-reserved sparse RAM metadata. */
#include "metadata_internal.h"
#include <reliefnt/console.h>
#include <reliefnt/export.h>
#include <reliefnt/lock.h>
#include <reliefnt/mm.h>
#include <reliefnt/panic.h>
#include <reliefnt/paging.h>
#include <linux/errno.h>

#define MM_REF_PINNED UINT32_MAX

/** @brief Upper-bound hint for a zone's highest potentially free page. */
struct mm_hint {
    struct mm_page_section *section; /**< Section at or immediately above pfn. */
    uint64_t pfn;                    /**< Highest candidate page frame. */
};

static struct mm_metadata memory;
static const struct mm_topology *trusted_topology;
static struct mm_hint low_hint, high_hint;
static uint64_t free_page_count, total_kib, emergency_reserve_pages;
static bool runtime_ready;
static struct kernel_spinlock mm_lock = KERNEL_SPINLOCK_INIT;

/**
 * @brief Raise the appropriate free-page hint to include one available PFN.
 * @param section Section containing pfn.
 * @param pfn Newly free physical page frame.
 * Caller owns initialization or holds the IRQ-safe allocator lock.
 */
static void note_free(struct mm_page_section *section, uint64_t pfn)
{
    struct mm_hint *hint = pfn < MM_DMA32_PFN ? &low_hint : &high_hint;
    if (!hint->section || pfn > hint->pfn) *hint = (struct mm_hint){section, pfn};
}

/**
 * @brief Initialize zone hints from the highest seeded bits in each section.
 * Caller owns unpublished metadata; no allocation or lock is needed.
 */
static void initialize_hints(void)
{
    low_hint = (struct mm_hint){0}; high_hint = (struct mm_hint){0};
    for (struct mm_page_section *s = memory.first; s; s = s->next) {
        for (unsigned word = 0; word < MM_METADATA_WORDS; ++word) {
            uint64_t bits = s->free[word];
            if (!bits) continue;
            uint64_t pfn = s->first_pfn + word * 64u + 63u - (unsigned)__builtin_clzll(bits);
            note_free(s, pfn);
            /* A word can straddle the DMA32 boundary. */
            if (pfn >= MM_DMA32_PFN && s->first_pfn + word * 64u < MM_DMA32_PFN) {
                unsigned low_bits = (unsigned)(MM_DMA32_PFN - (s->first_pfn + word * 64u));
                uint64_t low = bits & ((1ULL << low_bits) - 1);
                if (low) note_free(s, s->first_pfn + word * 64u + 63u - (unsigned)__builtin_clzll(low));
            }
        }
    }
}

/**
 * @brief Publish sparse runtime metadata after all bootstrap reservations finish.
 * @param topology Current published bootstrap view; borrowed and mutable by bootstrap.
 * @return 0 or negative errno. BSP only, interrupts disabled, before other PMM users;
 * failed reservations remain bootstrap-owned and no free pool is published.
 */
int mm_runtime_init(const struct mm_topology *topology)
{
    if (runtime_ready) return -EBUSY;
    struct mm_metadata prepared;
    int rc = mm_metadata_prepare(topology, &prepared);
    if (rc) return rc;
    kernel_spin_init(&mm_lock);
    memory = prepared;
    free_page_count = prepared.free_pages;
    total_kib = prepared.free_pages * (MM_PHYS_PAGE_SIZE / 1024);
    /* Very small test/boot pools cannot dedicate 64 pages; keep one quarter
     * of their allocatable budget instead, up to the normal 64-page reserve. */
    emergency_reserve_pages = prepared.free_pages / 4;
    if (emergency_reserve_pages > 64) emergency_reserve_pages = 64;
    initialize_hints();
    mm_bootstrap_finish();
    trusted_topology = topology;
    runtime_ready = true;
    return 0;
}

/**
 * @brief Prepare the real boot topology and publish the physical page allocator.
 * @param boot Borrowed handoff and its reachable map, valid until prepare returns.
 * BSP only after the initial shared map and framebuffer are ready; no PMM recursion.
 * An invalid map or exhausted metadata halts boot instead of inventing RAM.
 */
void mm_init(const struct boot_info *boot)
{
    const struct mm_topology *topology = mm_bootstrap_topology();
    int rc = 0;
    if (!topology) {
        rc = mm_bootstrap_prepare(boot, paging_physical_bits());
        if (!rc) topology = mm_bootstrap_topology();
    }
    if (!rc) rc = mm_runtime_init(topology);
    if (rc) {
        console_fatal("[reliefnt] physical memory initialization failed rc=%d\n", rc);
        panic("Physical memory initialization failed");
    }
    console_info("[reliefnt] mm initialized usable=%llu KiB mapped_ram=%llu KiB\n",
                 (unsigned long long)total_kib,
                 (unsigned long long)(memory.mapped_ram_pages * (MM_PHYS_PAGE_SIZE / 1024)));
    paging_mmio_allocator_ready();
}

/** @brief Report initial allocatable capacity in KiB. @return Initial pool size; no ownership change. */
uint64_t mm_total_memory_kib(void) { return total_kib; }

/** @brief Report the pool's protected emergency budget in pages.
 * @return Immutable reserve after initialization, or zero beforehand.
 */
uint64_t mm_emergency_reserve_pages(void) { return emergency_reserve_pages; }

/**
 * @brief Snapshot unallocated physical page-pool capacity under the MM lock.
 * @return Free physical page KiB, excluding cached/heap allocations; IRQ-safe.
 */
uint64_t mm_free_memory_kib(void)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_lock, &flags);
    uint64_t result = free_page_count * (MM_PHYS_PAGE_SIZE / 1024);
    kernel_spin_unlock_irqrestore(&mm_lock, flags);
    return result;
}

/**
 * @brief Translate a mapped physical byte through the supervisor direct map.
 * @param phys Physical byte address.
 * @return Borrowed kernel alias, or NULL outside the installed map.
 */
void *mm_phys_to_virt(uint64_t phys) { return paging_kernel_direct_map(phys); }

/**
 * @brief Translate a direct-map or supervisor kernel-hole pointer to physical.
 * @param address Borrowed pointer in one of the supported kernel mappings.
 * @return Physical byte address or zero outside the kernel hole or actual
 * direct-map coverage; no ownership transfer.
 */
uint64_t mm_virt_to_phys(const void *address)
{
    uint64_t value = (uint64_t)(uintptr_t)address;
    if (value >= RELIEFNT_KERNEL_HOLE_START && value < RELIEFNT_KERNEL_HOLE_END) return value;
    if (value < RELIEFNT_KERNEL_DIRECT_MAP_BASE ||
        value - RELIEFNT_KERNEL_DIRECT_MAP_BASE >= RELIEFNT_KERNEL_DIRECT_MAP_SIZE) return 0;
    uint64_t physical = value - RELIEFNT_KERNEL_DIRECT_MAP_BASE;
    return paging_kernel_direct_map_range(physical, 1) ? physical : 0;
}

/**
 * @brief Check a section-local bitmap bit.
 * @param bits Borrowed section bitmap.
 * @param offset Page offset known to be inside its section.
 * @return True when the page's bit is set; no mutation.
 */
static bool test_bit(const uint64_t *bits, uint64_t offset)
{ return (bits[offset / 64] & (1ULL << (offset % 64))) != 0; }

/**
 * @brief Decrease a zone hint when a just-claimed run covers its upper bound.
 * @param hint Mutable zone hint under the allocator lock.
 * @param first_section Section containing the beginning of the claimed run.
 * @param first Inclusive first PFN of the run.
 * @param last Inclusive final PFN of the run.
 */
static void lower_hint(struct mm_hint *hint, struct mm_page_section *first_section,
                       uint64_t first, uint64_t last)
{
    if (hint->section && hint->pfn >= first && hint->pfn <= last) {
        hint->section = first_section;
        hint->pfn = first - 1;
    }
}

/**
 * @brief Atomically claim a known free contiguous run, including section crossings.
 * @param first_section Section containing the first PFN.
 * @param first First PFN in the owned run.
 * @param count Nonzero number of pages.
 * Caller holds mm_lock; caller clears pages after releasing it.
 */
static void claim(struct mm_page_section *first_section, uint64_t first, uint32_t count)
{
    struct mm_page_section *s = first_section;
    for (uint64_t pfn = first; pfn < first + count; ++pfn) {
        while (pfn >= s->first_pfn + s->pages) s = s->next;
        uint64_t offset = pfn - s->first_pfn;
        s->free[offset / 64] &= ~(1ULL << (offset % 64));
        s->refs[offset] = 1;
        --s->free_count;
    }
    free_page_count -= count;
    lower_hint(&low_hint, first_section, first, first + count - 1);
    lower_hint(&high_hint, first_section, first, first + count - 1);
}

/**
 * @brief Find and claim the highest available run inside an inclusive PFN interval.
 * @param hint Starting upper bound for the region, held under mm_lock.
 * @param minimum Inclusive minimum allowed PFN.
 * @param maximum Inclusive maximum allowed PFN.
 * @param count Requested page count.
 * @return Owned physical base or zero, without clearing; caller holds mm_lock.
 */
static uint64_t scan(struct mm_hint hint, uint64_t minimum, uint64_t maximum, uint32_t count)
{
    if (!hint.section || !count || count > free_page_count || maximum < minimum) return 0;
    if (hint.pfn > maximum) hint.pfn = maximum;
    uint64_t run = 0, previous = UINT64_MAX;
    for (struct mm_page_section *s = hint.section; s; s = s->prev) {
        if (s->first_pfn > hint.pfn) continue;
        uint64_t top = s->first_pfn + s->pages - 1;
        if (top > hint.pfn) top = hint.pfn;
        if (top < minimum) break;
        if (!s->free_count) { run = 0; previous = UINT64_MAX; continue; }
        uint64_t pfn = top;
        for (;;) {
            uint64_t offset = pfn - s->first_pfn;
            if (test_bit(s->free, offset)) {
                if (previous != UINT64_MAX && previous != pfn + 1) run = 0;
                ++run;
                if (run == count) { claim(s, pfn, count); return pfn * MM_PHYS_PAGE_SIZE; }
            } else {
                run = 0;
                if (!(s->free[offset / 64])) {
                    uint64_t skip = offset % 64;
                    if (skip > pfn - minimum) skip = pfn - minimum;
                    pfn -= skip;
                }
            }
            previous = pfn;
            if (pfn == s->first_pfn || pfn == minimum) break;
            --pfn;
        }
        hint.pfn = s->first_pfn ? s->first_pfn - 1 : 0;
    }
    return 0;
}

/**
 * @brief Clear a newly owned page run without holding the allocator lock.
 * @param phys First physical byte of the exclusively claimed run.
 * @param count Number of 4 KiB pages.
 */
static void clear_pages(uint64_t phys, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
        __builtin_memset(mm_phys_to_virt(phys + (uint64_t)i * MM_PHYS_PAGE_SIZE), 0, MM_PHYS_PAGE_SIZE);
}

/**
 * @brief Select an upper bound covering free pages in both allocation zones.
 * @return Highest zone hint, or an empty hint when neither zone has one.
 * Caller holds mm_lock; a crossing claim can move the high hint below low RAM.
 */
static struct mm_hint combined_hint(void)
{
    if (!high_hint.section || (low_hint.section && low_hint.pfn > high_hint.pfn))
        return low_hint;
    return high_hint;
}

/** @brief Claim one masked run with explicit reserve authority.
 * @param page_count Nonzero number of 4 KiB pages. @param mask Inclusive byte
 * maximum. @param emergency True only for explicitly admitted recovery.
 * @return Zeroed owned physical base or zero; no callbacks under the PMM lock.
 */
static uint64_t mm_allocate_pages_below(uint32_t page_count, uint64_t mask, bool emergency)
{
    uint64_t bytes = (uint64_t)page_count * MM_PHYS_PAGE_SIZE;
    if (!page_count || mask < bytes - 1 || mask < MM_PHYS_PAGE_SIZE - 1) return 0;
    uint64_t maximum = (mask - (MM_PHYS_PAGE_SIZE - 1)) / MM_PHYS_PAGE_SIZE;
    uint64_t flags, result;
    kernel_spin_lock_irqsave(&mm_lock, &flags);
    struct mm_hint hint = maximum >= MM_DMA32_PFN ? combined_hint() : low_hint;
    result = runtime_ready && free_page_count >= page_count &&
             (emergency || free_page_count - page_count >= emergency_reserve_pages) ?
             scan(hint, 1, maximum, page_count) : 0;
    kernel_spin_unlock_irqrestore(&mm_lock, flags);
    if (result) clear_pages(result, page_count);
    return result;
}

/** @brief Claim a normal run within a strict inclusive byte mask.
 * @param page_count Nonzero number of pages. @param mask Highest byte.
 * @return Zeroed owned physical base or zero; reserve remains protected.
 */
uint64_t mm_alloc_pages_below(uint32_t page_count, uint64_t mask)
{ return mm_allocate_pages_below(page_count, mask, false); }

/** @brief Claim a recovery run within a strict inclusive byte mask.
 * @param page_count Number of pages. @param mask Highest permitted byte.
 * @return Owned zeroed run or zero. No reclaim, callbacks, waits or I/O.
 */
uint64_t mm_alloc_emergency_pages_below(uint32_t page_count, uint64_t mask)
{ return mm_allocate_pages_below(page_count, mask, true); }

/** @brief Claim one run while applying the caller's explicit reserve authority.
 * @param page_count Number of contiguous pages. @param emergency True only for
 * audited recovery callers permitted to use reserve pages.
 * @return Zeroed owned base, or zero; no reclaim or callbacks under mm_lock.
 */
static uint64_t mm_allocate_pages(uint32_t page_count, bool emergency)
{
    if (!page_count) return 0;
    uint64_t flags, result = 0;
    kernel_spin_lock_irqsave(&mm_lock, &flags);
    if (runtime_ready && free_page_count >= page_count &&
        (emergency || free_page_count - page_count >= emergency_reserve_pages)) {
        result = scan(high_hint, MM_DMA32_PFN, UINT64_MAX, page_count);
        if (!result) {
            result = scan(combined_hint(), 1, UINT64_MAX, page_count);
        }
    }
    kernel_spin_unlock_irqrestore(&mm_lock, flags);
    if (result) clear_pages(result, page_count);
    return result;
}

/**
 * @brief Claim and clear a contiguous run, preferring the highest non-DMA32 RAM.
 * @param page_count Nonzero number of 4 KiB pages.
 * @return Zeroed owned physical base or zero; IRQ-safe scan, clearing outside lock.
 */
uint64_t mm_alloc_pages(uint32_t page_count)
{ return mm_allocate_pages(page_count, false); }

/** @brief Allocate one zeroed physical page. @return Owned physical address or zero; caller frees. */
uint64_t mm_alloc_page(void) { return mm_alloc_pages(1); }

/** @brief Allocate one recovery page with explicit emergency reserve authority.
 * @return Owned zeroed page or zero. No reclaim, waits, callbacks or I/O.
 */
uint64_t mm_alloc_emergency_page(void) { return mm_allocate_pages(1, true); }

/** @brief Claim a recovery run from the protected physical pool.
 * @param page_count Number of pages. @return Owned zeroed run or zero.
 * No reclaim, callbacks, waits or I/O.
 */
uint64_t mm_alloc_emergency_pages(uint32_t page_count)
{ return mm_allocate_pages(page_count, true); }

/**
 * @brief Query whether every byte belongs to trusted RAM in the frozen topology.
 * @param phys Inclusive physical byte address.
 * @param bytes Nonzero byte length; the exclusive end must not overflow.
 * @return True for complete trusted RAM, including reserved or currently unmapped RAM.
 * The immutable bootstrap view remains owned permanently; no mapping or reference is acquired.
 */
bool mm_phys_is_ram(uint64_t phys, uint64_t bytes)
{
    if (!runtime_ready || !bytes || bytes > UINT64_MAX - phys) return false;
    const struct mm_topology *topology = trusted_topology;
    uint64_t end = phys + bytes, cursor = phys;
    size_t lo = 0, hi = topology->range_count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (topology->ranges[middle].end <= cursor) lo = middle + 1;
        else hi = middle;
    }
    while (cursor < end && lo < topology->range_count) {
        struct mm_topology_range r = topology->ranges[lo++];
        if (r.start > cursor || !(r.flags & MM_TOPOLOGY_RAM)) return false;
        cursor = r.end < end ? r.end : end;
    }
    return cursor == end;
}

/**
 * @brief Snapshot a managed page's owned reference count without retaining it.
 * @param phys Page-aligned physical address.
 * @return Current references, UINT32_MAX for a saturated page, or zero if absent/free/reserved.
 * IRQ-safe; no ownership transfer.
 */
uint32_t mm_page_refcount(uint64_t phys)
{
    if (!phys || (phys & (MM_PHYS_PAGE_SIZE - 1))) return 0;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_lock, &flags);
    struct mm_page_section *s = runtime_ready ? mm_metadata_find(&memory, phys / MM_PHYS_PAGE_SIZE) : NULL;
    uint32_t refs = s ? s->refs[phys / MM_PHYS_PAGE_SIZE - s->first_pfn] : 0;
    kernel_spin_unlock_irqrestore(&mm_lock, flags);
    return refs;
}

/** @brief Release a reference to a verified usable page under the PMM lock.
 * @param section Metadata section containing pfn. @param pfn Physical page frame.
 * @return True when the page became free.
 */
static bool release_page_locked(struct mm_page_section *section, uint64_t pfn)
{
    uint64_t offset = pfn - section->first_pfn;
    uint32_t *refs = &section->refs[offset];
    if (!*refs || *refs == MM_REF_PINNED || --*refs) return false;
    section->free[offset / 64] |= 1ULL << (offset % 64);
    ++section->free_count; ++free_page_count;
    note_free(section, pfn);
    return true;
}

/**
 * @brief Consume one reference per page of an entirely managed run.
 * @param phys Page-aligned physical base; zero, holes and reserved runs are ignored.
 * @param page_count Number of pages; zero or overflowing runs are ignored.
 * IRQ-safe, no reclaim; caller first retires mappings, DMA, and callbacks.
 */
void mm_free_pages(uint64_t phys, uint32_t page_count)
{
    uint64_t bytes = (uint64_t)page_count * MM_PHYS_PAGE_SIZE;
    if (!phys || !page_count || (phys & (MM_PHYS_PAGE_SIZE - 1)) || bytes > UINT64_MAX - phys) return;
    uint64_t flags, first = phys / MM_PHYS_PAGE_SIZE;
    kernel_spin_lock_irqsave(&mm_lock, &flags);
    struct mm_page_section *s = runtime_ready ? mm_metadata_find(&memory, first) : NULL;
    struct mm_page_section *start = s;
    for (uint64_t pfn = first; s && pfn < first + page_count; ++pfn) {
        while (s && pfn >= s->first_pfn + s->pages) s = s->next;
        if (!s || pfn < s->first_pfn || !test_bit(s->usable, pfn - s->first_pfn)) { s = NULL; break; }
    }
    if (s) {
        s = start;
        for (uint64_t pfn = first; pfn < first + page_count; ++pfn) {
            while (pfn >= s->first_pfn + s->pages) s = s->next;
            (void)release_page_locked(s, pfn);
        }
    }
    kernel_spin_unlock_irqrestore(&mm_lock, flags);
}

/** @brief Release one physical page reference. @param phys Owned aligned page address. */
void mm_free_page(uint64_t phys) { mm_free_pages(phys, 1); }

/** @brief Release one reference and atomically observe actual reclamation.
 * @param phys Owned aligned physical page. @return True only if it became free.
 * Invalid, free, reserved and saturated pages remain unchanged; IRQ-safe.
 */
bool mm_free_page_reclaimed(uint64_t phys)
{
    if (!phys || (phys & (MM_PHYS_PAGE_SIZE - 1)) ||
        phys > UINT64_MAX - MM_PHYS_PAGE_SIZE) return false;
    uint64_t flags, pfn = phys / MM_PHYS_PAGE_SIZE;
    kernel_spin_lock_irqsave(&mm_lock, &flags);
    struct mm_page_section *s = runtime_ready ? mm_metadata_find(&memory, pfn) : NULL;
    bool freed = s && test_bit(s->usable, pfn - s->first_pfn) &&
                 release_page_locked(s, pfn);
    kernel_spin_unlock_irqrestore(&mm_lock, flags);
    return freed;
}

/**
 * @brief Retain one owned page; saturation permanently pins it.
 * @param phys Aligned physical page address; unowned, absent and reserved pages are ignored.
 * IRQ-safe; caller must already own a reference and synchronize object lifetime.
 */
void mm_retain_page(uint64_t phys)
{
    if (!phys || (phys & (MM_PHYS_PAGE_SIZE - 1))) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_lock, &flags);
    struct mm_page_section *s = runtime_ready ? mm_metadata_find(&memory, phys / MM_PHYS_PAGE_SIZE) : NULL;
    if (s) {
        uint32_t *refs = &s->refs[phys / MM_PHYS_PAGE_SIZE - s->first_pfn];
        if (*refs && *refs != MM_REF_PINNED) ++*refs;
    }
    kernel_spin_unlock_irqrestore(&mm_lock, flags);
}

EXPORT_SYMBOL(mm_alloc_pages);
EXPORT_SYMBOL(mm_alloc_pages_below);
EXPORT_SYMBOL(mm_free_pages);
EXPORT_SYMBOL(mm_phys_to_virt);
EXPORT_SYMBOL(mm_virt_to_phys);
EXPORT_SYMBOL(mm_free_memory_kib);
