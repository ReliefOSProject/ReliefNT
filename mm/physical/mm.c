/*
 * ReliefOS physical memory manager: tracks usable and reserved page ranges.
 * Allocates, frees, zeroes, and reference-counts physical memory pages.
 */
#include <reliefnt/export.h>
#include <reliefnt/console.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/mm.h>
#include <reliefnt/paging.h>
#include <reliefnt/lock.h>

/* Bounds of the kernel image as placed by arch/x86_64/linker.ld. */
extern char __kernel_start[];
extern char __kernel_end[];

static uint64_t total_kib;
static uint64_t unsupported_ram_bytes;
static uint32_t unsupported_ram_ranges;

#define PAGE_SIZE 4096ULL
/* Keep the conventional first MiB out of the allocator. */
#define MM_LOW_RESERVED_END 0x100000ULL
/* Page references and the supervisor direct map currently cover [0, 16 GiB). */
#define PAGE_ALLOC_LIMIT RELIEFNT_KERNEL_DIRECT_MAP_SIZE
#define FALLBACK_ALLOC_START 0x0a000000ULL
#define FALLBACK_MEMORY_KIB (512ULL * 1024ULL)
#define MM_MAX_FREE_RANGES 256u
#define MM_MAX_RESERVED_RANGES 64u
#define MM_PAGE_COUNT (PAGE_ALLOC_LIMIT / PAGE_SIZE)
#define MM_PAGE_REF_PINNED UINT32_MAX

struct phys_range {
    uint64_t start;
    uint64_t end;
    const char *name;
};

static struct phys_range free_ranges[MM_MAX_FREE_RANGES];
static uint32_t free_range_count;
static struct phys_range reserved_ranges[MM_MAX_RESERVED_RANGES];
static uint32_t reserved_range_count;
/* The allocator tracks 4,194,304 pages below 16 GiB.  A 32-bit count costs
 * 16 MiB and supports large shared mappings.  UINT32_MAX is a pinned state:
 * if a page ever reaches that count, later frees cannot release it early. */
static uint32_t page_refs[MM_PAGE_COUNT];
/* Runtime free pages cannot be represented by a bounded list of fragments. */
static uint64_t *free_pages;
static uint32_t free_page_count;
static uint32_t allocation_hint;
static struct kernel_spinlock mm_lock = KERNEL_SPINLOCK_INIT;

/**
 * @brief Convert a physical address to its index in the page_refs array.
 */
static uint32_t page_index(uint64_t phys)
{
    return (uint32_t)(phys / PAGE_SIZE);
}

/**
 * @brief True for EFI memory types the allocator may hand out (conventional, boot, loader, etc.).
 */
static int efi_memory_usable(uint32_t type)
{
    return type == 1 || type == 2 || type == 3 || type == 4 || type == 7;
}

/**
 * @brief Round value down to a multiple of align, which must be a power of two.
 */
static uint64_t align_down(uint64_t value, uint64_t align)
{
    return value & ~(align - 1);
}

/**
 * @brief Round value up to a multiple of align, which must be a power of two.
 */
static uint64_t align_up(uint64_t value, uint64_t align)
{
    return (value + align - 1) & ~(align - 1);
}

/**
 * @brief Zero the full 4 KiB page through the supervisor direct map.
 */
static void zero_page(uint64_t phys)
{
    uint8_t *p = (uint8_t *)mm_phys_to_virt(phys);
    if (!p) return;
    for (uint64_t i = 0; i < PAGE_SIZE; ++i) {
        p[i] = 0;
    }
}

/**
 * @brief Zero page_count consecutive pages starting at phys.
 */
static void zero_pages(uint64_t phys, uint32_t page_count)
{
    for (uint32_t i = 0; i < page_count; ++i) {
        zero_page(phys + (uint64_t)i * PAGE_SIZE);
    }
}

/**
 * @brief Clip a usable range to direct-map bounds and append its aligned pages.
 */
static void add_free_range(uint64_t start, uint64_t end)
{
    if (end < start) {
        console_warn("[reliefnt] rejecting wrapped usable RAM range start=0x%llx end=0x%llx\n",
                     (unsigned long long)start, (unsigned long long)end);
        return;
    }
    if (end > PAGE_ALLOC_LIMIT) {
        uint64_t unsupported_start = start > PAGE_ALLOC_LIMIT ? start : PAGE_ALLOC_LIMIT;
        if (unsupported_start < end) {
            unsupported_ram_bytes += end - unsupported_start;
            ++unsupported_ram_ranges;
        }
        end = PAGE_ALLOC_LIMIT;
    }
    start = align_up(start, PAGE_SIZE);
    end = align_down(end, PAGE_SIZE);
    if (start < MM_LOW_RESERVED_END) {
        start = MM_LOW_RESERVED_END;
    }
    if (start >= end || free_range_count >= MM_MAX_FREE_RANGES) {
        return;
    }
    free_ranges[free_range_count++] = (struct phys_range){start, end, "free"};
}

/**
 * @brief Grow [start,end) outward to page boundaries and append it to the reserved table.
 */
static void add_reserved_range(uint64_t start, uint64_t end, const char *name)
{
    start = align_down(start, PAGE_SIZE);
    end = align_up(end, PAGE_SIZE);
    if (start >= end || reserved_range_count >= MM_MAX_RESERVED_RANGES) {
        return;
    }
    reserved_ranges[reserved_range_count++] = (struct phys_range){start, end, name};
}

/**
 * @brief Drop free_ranges[index] and shift the remaining entries left to close the gap.
 */
static void remove_free_range(uint32_t index)
{
    if (index >= free_range_count) {
        return;
    }
    for (uint32_t i = index + 1; i < free_range_count; ++i) {
        free_ranges[i - 1] = free_ranges[i];
    }
    --free_range_count;
}

/**
 * @brief Order free ranges by ascending start address (simple selection sort).
 */
static void sort_free_ranges(void)
{
    for (uint32_t i = 0; i < free_range_count; ++i) {
        for (uint32_t j = i + 1; j < free_range_count; ++j) {
            if (free_ranges[j].start < free_ranges[i].start) {
                struct phys_range tmp = free_ranges[i];
                free_ranges[i] = free_ranges[j];
                free_ranges[j] = tmp;
            }
        }
    }
}

/**
 * @brief Sort then merge adjacent or overlapping free ranges into the fewest contiguous spans.
 */
static void coalesce_free_ranges(void)
{
    sort_free_ranges();
    for (uint32_t i = 0; i + 1 < free_range_count;) {
        if (free_ranges[i].end >= free_ranges[i + 1].start) {
            if (free_ranges[i + 1].end > free_ranges[i].end) {
                free_ranges[i].end = free_ranges[i + 1].end;
            }
            remove_free_range(i + 1);
            continue;
        }
        ++i;
    }
}

/**
 * @brief Carve [start,end) out of every free range it overlaps, splitting ranges as needed.
 */
static void reserve_from_free(uint64_t start, uint64_t end)
{
    start = align_down(start, PAGE_SIZE);
    end = align_up(end, PAGE_SIZE);
    if (start >= end) {
        return;
    }
    for (uint32_t i = 0; i < free_range_count;) {
        struct phys_range r = free_ranges[i];
        if (end <= r.start || start >= r.end) {
            ++i;
            continue;
        }
        if (start <= r.start && end >= r.end) {
            remove_free_range(i);
            continue;
        }
        if (start <= r.start) {
            free_ranges[i].start = end < r.end ? end : r.end;
            ++i;
            continue;
        }
        if (end >= r.end) {
            free_ranges[i].end = start > r.start ? start : r.start;
            ++i;
            continue;
        }
        if (free_range_count < MM_MAX_FREE_RANGES) {
            free_ranges[i].end = start;
            free_ranges[free_range_count++] = (struct phys_range){end, r.end, "free"};
        } else {
            free_ranges[i].end = start;
        }
        ++i;
    }
}

/**
 * @brief Record a reserved range and simultaneously remove those pages from the free list.
 */
static void reserve_range(uint64_t start, uint64_t end, const char *name)
{
    add_reserved_range(start, end, name);
    reserve_from_free(start, end);
}

/**
 * @brief True when [start,end) intersects any previously reserved range.
 */
static int overlaps_reserved(uint64_t start, uint64_t end)
{
    for (uint32_t i = 0; i < reserved_range_count; ++i) {
        if (start < reserved_ranges[i].end && end > reserved_ranges[i].start) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Guard against null or overflowing ranges, then reserve [start, start+bytes).
 */
static void reserve_bytes(uint64_t start, uint64_t bytes, const char *name)
{
    if (!start || !bytes || start + bytes < start) {
        return;
    }
    reserve_range(start, start + bytes, name);
}

/**
 * @brief Register a synthetic free range when the loader supplied no usable memory map.
 */
static void add_fallback_free_range(void)
{
    uint64_t end = (total_kib ? total_kib : FALLBACK_MEMORY_KIB) * 1024ULL;
    if (end > PAGE_ALLOC_LIMIT) {
        end = PAGE_ALLOC_LIMIT;
    }
    if (end <= FALLBACK_ALLOC_START) {
        end = FALLBACK_ALLOC_START + 128ULL * 1024ULL * 1024ULL;
    }
    add_free_range(FALLBACK_ALLOC_START, end);
}

/**
 * @brief Populate the free list from the multiboot or EFI memory map, falling back to a guess.
 */
static void seed_free_ranges_from_boot(const struct boot_info *boot)
{
    if (boot->mmap_addr && boot->mmap_entry_count) {
        total_kib = 0;
        const uint8_t *cursor = (const uint8_t *)mm_phys_to_virt(boot->mmap_addr);
        if (!cursor) {
            console_warn("[reliefnt] multiboot memory map is outside the kernel direct map\n");
            add_fallback_free_range();
            return;
        }
        for (uint32_t i = 0; i < boot->mmap_entry_count; ++i) {
            const struct multiboot2_mmap_entry *entry =
                (const struct multiboot2_mmap_entry *)(cursor + (uint64_t)i * boot->mmap_entry_size);
            if (entry->type == 1) {
                add_free_range(entry->addr, entry->addr + entry->len);
            }
        }
        return;
    }
    if (boot->efi_mmap_addr && boot->efi_mmap_entry_count) {
        total_kib = 0;
        const uint8_t *cursor = (const uint8_t *)mm_phys_to_virt(boot->efi_mmap_addr);
        if (!cursor) {
            console_warn("[reliefnt] EFI memory map is outside the kernel direct map\n");
            add_fallback_free_range();
            return;
        }
        for (uint32_t i = 0; i < boot->efi_mmap_entry_count; ++i) {
            const struct efi_memory_descriptor *entry =
                (const struct efi_memory_descriptor *)(cursor + (uint64_t)i * boot->efi_mmap_entry_size);
            uint64_t len = entry->number_of_pages * PAGE_SIZE;
            if (efi_memory_usable(entry->type)) {
                add_free_range(entry->physical_start, entry->physical_start + len);
            }
        }
        return;
    }
    total_kib = 0;
    add_fallback_free_range();
}

static void recompute_total_allocatable_kib(void)
{
    total_kib = 0;
    for (uint32_t i = 0; i < free_range_count; ++i) {
        if (free_ranges[i].end > free_ranges[i].start) {
            total_kib += (free_ranges[i].end - free_ranges[i].start) / 1024ULL;
        }
    }
}

/**
 * @brief Protect the kernel image, framebuffer, modules, and boot structures from allocation.
 */
static void reserve_boot_ranges(const struct boot_info *boot)
{
    uint64_t boot_framebuffer_start = 0;
    uint64_t boot_framebuffer_bytes = 0;
    reserve_range(0, MM_LOW_RESERVED_END, "lowmem");
    if (boot->mmap_addr && boot->mmap_entry_count) {
        reserve_bytes(boot->mmap_addr,
                      (uint64_t)boot->mmap_entry_count * boot->mmap_entry_size,
                      "mmap");
    }
    if (boot->efi_mmap_addr && boot->efi_mmap_entry_count) {
        reserve_bytes(boot->efi_mmap_addr,
                      (uint64_t)boot->efi_mmap_entry_count * boot->efi_mmap_entry_size,
                      "efi-mmap");
    }
    if (boot->multiboot_info) {
        const struct multiboot2_info *info = (const struct multiboot2_info *)
            mm_phys_to_virt(boot->multiboot_info);
        uint64_t bytes = info && info->total_size ? info->total_size : 4096;
        reserve_bytes(boot->multiboot_info, bytes, "multiboot");
    }
    if (boot->rsdp_addr) {
        reserve_bytes(boot->rsdp_addr, 4096, "acpi-rsdp");
    }
    if (boot->framebuffer_addr && boot->framebuffer_pitch && boot->framebuffer_height) {
        boot_framebuffer_start = boot->framebuffer_addr;
        boot_framebuffer_bytes = (uint64_t)boot->framebuffer_pitch * boot->framebuffer_height;
        reserve_bytes(boot_framebuffer_start, boot_framebuffer_bytes,
                      "framebuffer");
    }
    {
        const struct framebuffer *fb = framebuffer_get();
        uint64_t reservation_start = fb->reservation_start;
        uint64_t reservation_bytes = fb->reservation_bytes;
        if (!reservation_start && fb->available && fb->pixels && fb->pitch && fb->height) {
            reservation_start = mm_virt_to_phys(fb->pixels);
            reservation_bytes = (uint64_t)fb->pitch * fb->height;
        }
        if (reservation_start && reservation_bytes &&
            (reservation_start != boot_framebuffer_start ||
             reservation_bytes != boot_framebuffer_bytes)) {
            reserve_bytes(reservation_start, reservation_bytes,
                          "framebuffer-vram");
        }
        if (fb->auxiliary_reservation_start && fb->auxiliary_reservation_bytes &&
            (fb->auxiliary_reservation_start != reservation_start ||
             fb->auxiliary_reservation_bytes != reservation_bytes)) {
            reserve_bytes(fb->auxiliary_reservation_start,
                          fb->auxiliary_reservation_bytes,
                          "framebuffer-device");
        }
    }
    for (uint32_t i = 0; i < boot->module_count; ++i) {
        reserve_range(boot->modules[i].start, boot->modules[i].end, "module");
    }
    /* The kernel ELF bounds cover every loaded section and .bss scratch
     * buffer (including the early EFI memory-map capture). */
    reserve_range((uint64_t)(uintptr_t)__kernel_start,
                  (uint64_t)(uintptr_t)__kernel_end, "kernel");
}

/**
 * @brief Dump the free and reserved range tables to the console for diagnostics.
 */
static void print_memory_map(void)
{
    console_info("[reliefnt] physical memory free ranges=%u reserved=%u\n",
                   free_range_count, reserved_range_count);
    for (uint32_t i = 0; i < reserved_range_count; ++i) {
        console_debug("[reliefnt] reserved[%u] %s 0x%llx-0x%llx (%llu KiB)\n",
                       i,
                       reserved_ranges[i].name ? reserved_ranges[i].name : "reserved",
                       (unsigned long long)reserved_ranges[i].start,
                       (unsigned long long)reserved_ranges[i].end,
                       (unsigned long long)((reserved_ranges[i].end - reserved_ranges[i].start) / 1024));
    }
    for (uint32_t i = 0; i < free_range_count; ++i) {
        console_debug("[reliefnt] free[%u] 0x%llx-0x%llx (%llu KiB)\n",
                       i,
                       (unsigned long long)free_ranges[i].start,
                       (unsigned long long)free_ranges[i].end,
                       (unsigned long long)((free_ranges[i].end - free_ranges[i].start) / 1024));
    }
    if (unsupported_ram_ranges) {
        console_warn("[reliefnt] unsupported usable RAM ranges=%u bytes=%llu above direct-map limit=0x%llx\n",
                     unsupported_ram_ranges,
                     (unsigned long long)unsupported_ram_bytes,
                     (unsigned long long)PAGE_ALLOC_LIMIT);
    }
}

/**
 * @brief Reset accounting, seed free ranges from boot, reserve kernel-owned areas, then coalesce.
 */
void mm_init(const struct boot_info *boot)
{
    kernel_spin_init(&mm_lock);
    total_kib = 0;
    unsupported_ram_bytes = 0;
    unsupported_ram_ranges = 0;
    free_range_count = 0;
    reserved_range_count = 0;
    for (uint32_t i = 0; i < MM_PAGE_COUNT; ++i) {
        page_refs[i] = 0;
    }
    seed_free_ranges_from_boot(boot);
    reserve_boot_ranges(boot);
    coalesce_free_ranges();
    /**
 * @brief Report the same range the allocator can actually serve. Previously this value came from the legacy lower/upper fields and could include low reserved memory or addresses above the allocator limit.
 */
    recompute_total_allocatable_kib();
    const uint64_t bitmap_bytes = MM_PAGE_COUNT / 8;
    free_pages = NULL;
    for (uint32_t i = 0; i < free_range_count; ++i) {
        if (free_ranges[i].end - free_ranges[i].start >= bitmap_bytes) {
            free_pages = (uint64_t *)mm_phys_to_virt(free_ranges[i].start);
            if (!free_pages) {
                console_fatal("[reliefnt] page bitmap is outside the kernel direct map\n");
                break;
            }
            reserve_range(free_ranges[i].start, free_ranges[i].start + bitmap_bytes, "page-bitmap");
            break;
        }
    }
    if (!free_pages) {
        console_fatal("[reliefnt] no memory for physical page bitmap\n");
        free_page_count = 0;
        allocation_hint = MM_PAGE_COUNT;
        return;
    }
    __builtin_memset(free_pages, 0, bitmap_bytes);
    free_page_count = 0;
    allocation_hint = MM_PAGE_COUNT;
    for (uint32_t i = 0; i < free_range_count; ++i) {
        for (uint64_t page = free_ranges[i].start; page < free_ranges[i].end; page += PAGE_SIZE) {
            uint32_t index = page_index(page);
            free_pages[index / 64] |= 1ULL << (index % 64);
            ++free_page_count;
            if (index < allocation_hint) allocation_hint = index;
        }
    }

    console_info("[reliefnt] mm initialized usable=%llu KiB mmap_entries=%u efi_mmap_entries=%u\n",
                   (unsigned long long)total_kib,
                   boot->mmap_entry_count,
                   boot->efi_mmap_entry_count);
    print_memory_map();
    /* Early framebuffer BAR setup may have used pinned page tables from the
     * kernel image. After this successful initialization, future MMIO splits
     * can safely consume allocator-owned pages. */
    paging_mmio_allocator_ready();
}

/**
 * @brief Return the total allocatable memory in KiB, computed once during mm_init.
 */
uint64_t mm_total_memory_kib(void)
{
    return total_kib;
}

/**
 * @brief Snapshot unallocated physical page-pool capacity under the MM lock.
 * @return Free physical page bytes divided by 1024; excludes reclaimable cached/heap allocations.
 * Task/IRQ-safe snapshot after physical allocator setup; no allocation, callbacks or ownership transfer.
 */
uint64_t mm_free_memory_kib(void)
{
    uint64_t free_kib = 0;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_lock, &flags);
    free_kib = (uint64_t)free_page_count * (PAGE_SIZE / 1024);
    kernel_spin_unlock_irqrestore(&mm_lock, flags);
    return free_kib;
}

/**
 * @brief Translate a supported physical address through the supervisor direct map.
 * @param phys Physical byte address in the mapped direct-map range.
 * @return Kernel pointer including the page offset, or NULL outside that range.
 */
void *mm_phys_to_virt(uint64_t phys)
{
    return paging_kernel_direct_map(phys);
}

/**
 * @brief Translate a supported supervisor kernel pointer to a physical address.
 * @param address Pointer in the direct-map window or linked kernel identity hole.
 * @return Physical byte address, or zero outside those two bounded mappings.
 * The low identity interval is limited to the supervisor kernel hole; ordinary
 * low user addresses and other low physical-range integers are not accepted.
 * Pure IRQ/task-safe translation; no allocation, lock, I/O or ownership transfer.
 */
uint64_t mm_virt_to_phys(const void *address)
{
    uint64_t value = (uint64_t)(uintptr_t)address;
    if (value >= RELIEFNT_KERNEL_HOLE_START &&
        value < RELIEFNT_KERNEL_HOLE_END) {
        return value;
    }
    if (value < RELIEFNT_KERNEL_DIRECT_MAP_BASE ||
        value - RELIEFNT_KERNEL_DIRECT_MAP_BASE >= RELIEFNT_KERNEL_DIRECT_MAP_SIZE) {
        return 0;
    }
    return value - RELIEFNT_KERNEL_DIRECT_MAP_BASE;
}

/**
 * @brief Atomically claim a contiguous run whose final byte fits the DMA mask.
 * @param page_count Nonzero number of 4 KiB pages; checked before scanning.
 * @param mask Inclusive maximum bus address; no high-address truncation.
 * @return Identity-mapped owned physical run, or 0. IRQ-safe allocator lock;
 * all pages are claimed together and zeroed before publication. Caller frees.
 */
uint64_t mm_alloc_pages_below(uint32_t page_count, uint64_t mask)
{
    uint64_t bytes = (uint64_t)page_count * PAGE_SIZE;
    if (!page_count || bytes - 1 > mask || page_count > MM_PAGE_COUNT) return 0;
    uint64_t flags, result = 0;
    kernel_spin_lock_irqsave(&mm_lock, &flags);
    uint64_t last = mask / PAGE_SIZE;
    if (last >= MM_PAGE_COUNT) last = MM_PAGE_COUNT - 1;
    uint32_t run = 0;
    if (free_pages && page_count <= free_page_count) for (uint32_t i = 1; i <= last; ++i) {
        if (!(free_pages[i / 64] & (1ULL << (i % 64)))) {
            run = 0;
            if (!(i % 64) && !free_pages[i / 64] && i + 63 <= last) i += 63;
            continue;
        }
        if (++run < page_count) continue;
        uint32_t first = i + 1 - page_count;
        uint64_t phys = (uint64_t)first * PAGE_SIZE;
        if (phys > mask || bytes - 1 > mask - phys) continue;
        for (uint32_t page = first; page <= i; ++page) {
            free_pages[page / 64] &= ~(1ULL << (page % 64)); page_refs[page] = 1;
        }
        free_page_count -= page_count;
        if (first == allocation_hint) allocation_hint = i + 1;
        result = phys;
        break;
    }
    kernel_spin_unlock_irqrestore(&mm_lock, flags);
    /* Claimed pages are private; zero outside the IRQ-disabled scan. */
    if (result) zero_pages(result, page_count);
    return result;
}
/**
 * @brief Allocate an unrestricted contiguous run, retaining the allocator hint.
 * @param page_count Requested pages.
 * @return Owned zeroed physical run, or 0. IRQ-safe; caller frees the run.
 */
uint64_t mm_alloc_pages(uint32_t page_count)
{
    uint64_t result = 0;
    uint64_t flags;
    if (!page_count) {
        return 0;
    }
    kernel_spin_lock_irqsave(&mm_lock, &flags);
    uint32_t run = 0;
    if (page_count <= free_page_count) for (uint32_t i = allocation_hint; i < MM_PAGE_COUNT; ++i) {
        if (!(free_pages[i / 64] & (1ULL << (i % 64)))) {
            run = 0;
            if (!(i % 64) && !free_pages[i / 64]) i += 63;
            continue;
        }
        if (++run < page_count) continue;
        uint32_t first = i + 1 - page_count;
        for (uint32_t page = first; page <= i; ++page) {
            free_pages[page / 64] &= ~(1ULL << (page % 64));
            page_refs[page] = 1;
        }
        free_page_count -= page_count;
        /* A multi-page search can skip smaller holes; retain those for later. */
        if (page_count == 1 || first == allocation_hint) allocation_hint = i + 1;
        result = (uint64_t)first * PAGE_SIZE;
        break;
    }
    kernel_spin_unlock_irqrestore(&mm_lock, flags);
    /* The claimed run cannot be reused while it is private to this call.
     * Avoid holding the allocator lock during a potentially large clear. */
    if (result) zero_pages(result, page_count);
    return result;
}

/**
 * @brief Allocate a single page via mm_alloc_pages(1).
 */
uint64_t mm_alloc_page(void)
{
    return mm_alloc_pages(1);
}

/**
 * @brief Consume one owned physical-page reference per page and reclaim pages whose count reaches zero.
 * @param phys Page-aligned base of an owned run; zero, invalid, reserved or out-of-range runs are ignored.
 * @param page_count Number of consecutive 4 KiB pages; zero is a no-op.
 * @par Context
 * IRQ-safe allocator lock; pinned and already-free pages remain unchanged. Caller must first retire every DMA, mapping and callback using the consumed references; no user access or scheduling.
 */
void mm_free_pages(uint64_t phys, uint32_t page_count)
{
    uint64_t flags;
    if (!phys || !page_count || (phys & (PAGE_SIZE - 1))) {
        return;
    }
    uint64_t end = phys + (uint64_t)page_count * PAGE_SIZE;
    if (end <= phys || end > PAGE_ALLOC_LIMIT || overlaps_reserved(phys, end)) {
        return;
    }
    kernel_spin_lock_irqsave(&mm_lock, &flags);
    for (uint32_t page = 0; page < page_count; ++page) {
        uint64_t current = phys + (uint64_t)page * PAGE_SIZE;
        uint32_t *refs = &page_refs[page_index(current)];
        if (!*refs || *refs == MM_PAGE_REF_PINNED) {
            continue;
        }
        --*refs;
        if (*refs) {
            continue;
        }
        uint32_t index = page_index(current);
        free_pages[index / 64] |= 1ULL << (index % 64);
        ++free_page_count;
        if (index < allocation_hint) allocation_hint = index;
    }
    kernel_spin_unlock_irqrestore(&mm_lock, flags);
}

/**
 * @brief Free a single page via mm_free_pages(phys, 1).
 */
void mm_free_page(uint64_t phys)
{
    mm_free_pages(phys, 1);
}

/**
 * @brief Retain a page, pinning it on reference-count overflow.
 * @param phys Physical page address to retain.
 */
void mm_retain_page(uint64_t phys)
{
    uint64_t flags;
    if (!phys || (phys & (PAGE_SIZE - 1)) || phys >= PAGE_ALLOC_LIMIT) {
        return;
    }
    kernel_spin_lock_irqsave(&mm_lock, &flags);
    uint32_t *refs = &page_refs[page_index(phys)];
    if (*refs && *refs != MM_PAGE_REF_PINNED) {
        if (*refs == MM_PAGE_REF_PINNED - 1) {
            *refs = MM_PAGE_REF_PINNED;
        } else {
            ++*refs;
        }
    }
    kernel_spin_unlock_irqrestore(&mm_lock, flags);
}

/* Native filesystem modules share this core-owned interface; CRC/release
 * checks bind it to the exact kernel, without a stable cross-release API. */
EXPORT_SYMBOL(mm_alloc_pages);
EXPORT_SYMBOL(mm_free_pages);
EXPORT_SYMBOL(mm_phys_to_virt);

EXPORT_SYMBOL(mm_virt_to_phys);

EXPORT_SYMBOL(mm_free_memory_kib);
