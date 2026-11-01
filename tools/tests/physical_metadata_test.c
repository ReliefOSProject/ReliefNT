#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <reliefnt/console.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <reliefnt/memory_topology.h>
#include <reliefnt/mm.h>
#include <reliefnt/paging.h>
#include "../../mm/physical/metadata_internal.h"

extern bool mm_free_page_reclaimed(uint64_t phys) __attribute__((weak));

#define PAGE 4096ULL
#define LOW 0x60000000ULL
#define HIGH 0x100000000ULL
#define WIDE 0x100000010000ULL
char __kernel_start[1], __kernel_end[1];
static struct framebuffer display;
static uint64_t map_limit = 16ULL * 1024 * 1024 * 1024;
static uintptr_t borrowed_map;
static size_t borrowed_bytes;
/** @brief Host storage for one emulated physical RAM extent. */
struct fixture_mapping {
    uint64_t base; /**< First emulated physical byte. */
    uint64_t bytes; /**< Complete extent size in bytes. */
    unsigned char *host; /**< Mapped host backing, owned for the process lifetime. */
};
static struct fixture_mapping mappings[4];
static unsigned mapping_count;
static bool sparse_mapping;
static uint64_t clear_watch;
static unsigned clear_checks;
static jmp_buf panic_resume;
static bool panic_expected;
/** @brief Report no extra ACPI reservations in physical allocator fixtures.
 * @return Zero fixture resources.
 */
uint32_t power_acpi_resource_count(void) { return 0; }
/** @brief Report no ACPI resource in physical allocator fixtures.
 * @param index Requested index. @param start Unused output. @param bytes Unused output.
 * @return False because this fixture has no ACPI resources.
 */
bool power_acpi_resource(uint32_t index, uint64_t *start, uint64_t *bytes)
{ (void)index; (void)start; (void)bytes; return false; }

/** @brief Borrow the test framebuffer descriptor. @return Fixture-owned descriptor. */
const struct framebuffer *framebuffer_get(void) { return &display; }
/** @brief Discard fixture diagnostics. @param level Log level. @param format Format string. */
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }
/**
 * @brief Report emulated direct-map coverage or the borrowed map buffer.
 * @param phys Physical byte address, including registered host pointer for the boot map.
 * @param len Byte length to validate in full.
 * @return True when the complete interval is reachable; no ownership transfer.
 */
bool paging_kernel_direct_map_range(uint64_t phys, uint64_t len)
{
    if (phys >= borrowed_map && phys - borrowed_map <= borrowed_bytes &&
        len <= borrowed_bytes - (phys - borrowed_map)) return true;
    if (sparse_mapping) {
        for (unsigned i = 0; i < mapping_count; ++i)
            if (phys >= mappings[i].base && phys - mappings[i].base < mappings[i].bytes &&
                len <= mappings[i].bytes - (phys - mappings[i].base)) return true;
        return false;
    }
    return phys < map_limit && len <= map_limit - phys;
}

/**
 * @brief Find the first mapped physical run using the fixture mapping inventory.
 * @param start Inclusive page-aligned search start.
 * @param end Exclusive page-aligned search end.
 * @param mapped_start Required output for first covered page.
 * @param mapped_end Required output for exclusive contiguous end.
 * @return True for a nonempty mapped run; no ownership transfer.
 */
bool paging_kernel_direct_map_next(uint64_t start, uint64_t end,
                                   uint64_t *mapped_start, uint64_t *mapped_end)
{
    if (!mapped_start || !mapped_end || start >= end || (start | end) & (PAGE - 1)) return false;
    if (!sparse_mapping) {
        if (start >= map_limit) return false;
        *mapped_start = start;
        *mapped_end = end < map_limit ? end : map_limit;
        return true;
    }
    uint64_t first = end, last = end;
    for (unsigned i = 0; i < mapping_count; ++i) {
        uint64_t lo = mappings[i].base, hi = lo + mappings[i].bytes;
        if (hi <= start || lo >= end) continue;
        if (lo < start) lo = start;
        if (hi > end) hi = end;
        if (lo < first) { first = lo; last = hi; }
    }
    if (first == end) return false;
    for (unsigned i = 0; i < mapping_count; ++i) {
        uint64_t lo = mappings[i].base, hi = lo + mappings[i].bytes;
        if (lo <= last && hi > last) last = hi < end ? hi : end;
    }
    *mapped_start = first;
    *mapped_end = last;
    return true;
}
/**
 * @brief Translate emulated physical addresses to mapped host storage.
 * @param phys Physical byte within a mapped fixture RAM extent.
 * @return Borrowed mapped pointer or NULL outside the emulated direct map.
 */
void *paging_kernel_direct_map(uint64_t phys)
{
    if (!paging_kernel_direct_map_range(phys, 1)) return NULL;
    for (unsigned i = 0; i < mapping_count; ++i)
        if (phys >= mappings[i].base && phys - mappings[i].base < mappings[i].bytes)
            return mappings[i].host + (phys - mappings[i].base);
    return NULL;
}
/**
 * @brief Translate a host-backed heap pointer at the direct-map test boundary.
 * @param address Borrowed pointer in an emulated physical RAM extent.
 * @return Its physical byte address, or zero outside the host fixture mappings.
 * The production direct-map pointer translation is covered separately by module-memory tests.
 */
uint64_t fixture_heap_virt_to_phys(const void *address)
{
    uintptr_t value = (uintptr_t)address;
    for (unsigned i = 0; i < mapping_count; ++i) {
        uintptr_t base = (uintptr_t)mappings[i].host;
        if (value >= base && value - base < mappings[i].bytes)
            return mappings[i].base + value - base;
    }
    return 0;
}
/**
 * @brief Model a fatal early-boot stop without terminating the test process.
 * @param message Borrowed fatal diagnostic, required by the fixture.
 * Does not return; unexpected panic terminates the fixture.
 */
__attribute__((noreturn)) void panic(const char *message)
{
    assert(message);
    if (panic_expected) longjmp(panic_resume, 1);
    abort();
}
/** @brief Return the emulated CPU physical-address width. @return 48 bits. */
uint8_t paging_physical_bits(void) { return 48; }
/** @brief Model paging's allocator-ready transition without host page tables. */
void paging_mmio_allocator_ready(void) { }
/** @brief Initialize fixture lock. @param lock Unpublished lock. */
void kernel_spin_init(struct kernel_spinlock *lock) { __atomic_store_n(&lock->state, 0, __ATOMIC_RELAXED); }
/** @brief Acquire fixture lock. @param lock Lock to acquire. @param flags Saved flags output. */
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ while (__atomic_exchange_n(&lock->state, 1, __ATOMIC_ACQUIRE)) sched_yield(); *flags = 0; }
/**
 * @brief Release fixture lock and optionally verify private pages precede zeroing.
 * @param lock Held lock. @param flags Saved flags from acquisition.
 */
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    (void)flags; assert(__atomic_load_n(&lock->state, __ATOMIC_RELAXED)); __atomic_store_n(&lock->state, 0, __ATOMIC_RELEASE);
    uint64_t watched = __atomic_exchange_n(&clear_watch, 0, __ATOMIC_RELAXED);
    if (watched) {
        assert(*(unsigned char *)paging_kernel_direct_map(watched) == 0xa5);
        for (unsigned i = 0; i < 8; ++i) assert(mm_page_refcount(watched + i * PAGE) == 1);
        uint64_t other = mm_alloc_page();
        assert(other && (other < watched || other >= watched + 8 * PAGE));
        mm_free_page(other);
        ++clear_checks;
    }
}
/**
 * @brief Map a physical fixture extent at its matching host address.
 * @param base Nonzero page-aligned physical and host address.
 * @param pages Number of contiguous pages.
 */
static void map_ram(uint64_t base, uint64_t pages)
{
    size_t bytes = (size_t)(pages * PAGE);
    assert(mapping_count < sizeof(mappings) / sizeof(mappings[0]));
    void *host = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(host != MAP_FAILED);
    mappings[mapping_count++] = (struct fixture_mapping){base, bytes, host};
}
/**
 * @brief Prepare the real topology/bootstrap and initialize production PMM.
 * @param entries Complete borrowed Multiboot RAM descriptors.
 * @param count Number of descriptors.
 * @param reserve Optional physical page to reserve as a boot module, or zero.
 */
static void boot_memory(struct multiboot2_mmap_entry *entries, uint32_t count, uint64_t reserve)
{
    borrowed_map = (uintptr_t)entries; borrowed_bytes = (size_t)count * sizeof(*entries);
    struct boot_info boot = {.mmap_addr=borrowed_map, .mmap_entry_count=count,
                             .mmap_entry_size=sizeof(*entries)};
    if (reserve) {
        boot.module_count = 1;
        boot.modules[0].start = reserve; boot.modules[0].end = reserve + PAGE;
    }
    mm_init(&boot);
    assert(mm_bootstrap_topology());
    assert(mm_free_memory_kib() > 0);
    assert(mm_runtime_init(mm_bootstrap_topology()) == -EBUSY);
    assert(!mm_bootstrap_alloc(1, UINT64_MAX));
}
/**
 * @brief Count final allocatable pages in the real bootstrap topology.
 * @return Page count after all metadata reservations; no ownership transfer.
 */
static uint64_t topology_free_pages(void)
{
    const struct mm_topology *t = mm_bootstrap_topology();
    uint64_t total = 0;
    for (size_t i = 0; i < t->range_count; ++i) {
        struct mm_topology_range r = t->ranges[i];
        if (!(r.flags & MM_TOPOLOGY_USABLE) || r.start >= map_limit) continue;
        if (r.end > map_limit) r.end = map_limit;
        total += (r.end - r.start) / PAGE;
    }
    return total;
}
/** @brief Kernel-hole and direct-map translation rejects user and module-like pointers. */
static void translation(void)
{
    uint64_t phys = 0x12345000ULL;
    assert(mm_virt_to_phys((const void *)(uintptr_t)RELIEFNT_KERNEL_HOLE_START) == RELIEFNT_KERNEL_HOLE_START);
    assert(mm_virt_to_phys((const void *)(uintptr_t)(RELIEFNT_KERNEL_HOLE_END - 1)) == RELIEFNT_KERNEL_HOLE_END - 1);
    assert(!mm_virt_to_phys((const void *)(uintptr_t)(RELIEFNT_KERNEL_HOLE_START - 1)));
    assert(!mm_virt_to_phys((const void *)(uintptr_t)RELIEFNT_KERNEL_HOLE_END));
    assert(!mm_virt_to_phys((const void *)(uintptr_t)(RELIEFNT_USER_TOP - 1)));
    assert(mm_virt_to_phys((const void *)(uintptr_t)(RELIEFNT_KERNEL_DIRECT_MAP_BASE + phys)) == phys);
    assert(!mm_virt_to_phys((const void *)(uintptr_t)(RELIEFNT_KERNEL_DIRECT_MAP_BASE + map_limit)));
    assert(!mm_virt_to_phys((const void *)(uintptr_t)(RELIEFNT_KERNEL_DIRECT_MAP_BASE + RELIEFNT_KERNEL_DIRECT_MAP_SIZE - 1)));
    assert(!mm_virt_to_phys((const void *)(uintptr_t)(RELIEFNT_KERNEL_DIRECT_MAP_BASE + RELIEFNT_KERNEL_DIRECT_MAP_SIZE)));
    assert(!mm_virt_to_phys((const void *)(uintptr_t)UINT64_MAX));
    puts("kernel pointer translation boundaries: PASS");
}
/** @brief A framebuffer described by a direct-map alias is never returned by the pool. */
static void framebuffer_reservation(void)
{
    map_ram(LOW, 64);
    uint64_t fb = LOW + 20 * PAGE;
    display = (struct framebuffer){.available=true, .pitch=PAGE, .height=1,
        .pixels=(uint32_t *)(uintptr_t)(RELIEFNT_KERNEL_DIRECT_MAP_BASE + fb)};
    struct multiboot2_mmap_entry e = {.addr=LOW, .len=64*PAGE, .type=1};
    boot_memory(&e, 1, 0);
    uint64_t before = mm_free_memory_kib();
    assert(mm_phys_is_ram(fb, PAGE) && !mm_page_refcount(fb));
    mm_free_page(fb);
    assert(mm_free_memory_kib() == before);
    for (uint64_t p; (p = mm_alloc_page()) != 0;) assert(p != fb);
    puts("framebuffer direct-map alias stays reserved: PASS");
}

/** @brief Small RAM must supply pages and reserve metadata before publishing the pool. */
static void small_ram(void)
{
    map_ram(LOW, 64);
    struct multiboot2_mmap_entry e = {.addr=LOW, .len=64*PAGE, .type=1};
    boot_memory(&e, 1, 0);
    uint64_t pages = mm_free_memory_kib() / 4;
    assert(pages == topology_free_pages() && pages >= 60 && pages < 64);
    uint64_t page = mm_alloc_page();
    assert(page >= LOW && page < LOW + 64 * PAGE);
    mm_free_page(page);
    assert(mm_free_memory_kib() == pages * 4);
    puts("small RAM uses proportional bootstrap-reserved metadata: PASS");
}
/** @brief Ordinary allocation uses high RAM; DMA requests honor the complete final byte. */
static void prefer_high(void)
{
    map_ram(LOW, 1024); map_ram(HIGH, 1024);
    struct multiboot2_mmap_entry e[] = {{.addr=LOW, .len=1024*PAGE, .type=1},
                                        {.addr=HIGH, .len=1024*PAGE, .type=1}};
    boot_memory(e, 2, 0);
    assert(!mm_alloc_pages_below(UINT32_MAX, UINT64_MAX));
    assert(!mm_alloc_pages_below(1, PAGE - 2));
    uint64_t high = mm_alloc_page();
    assert(high == HIGH + 1023 * PAGE);
    uint64_t low = mm_alloc_pages_below(2, 0xffffffffULL);
    assert(low >= LOW && low + 2 * PAGE - 1 <= 0xffffffffULL);
    mm_free_pages(low, 2);
    uint64_t constrained = mm_alloc_pages_below(1, LOW + 100 * PAGE + PAGE - 2);
    assert(constrained && constrained + PAGE - 1 <= LOW + 100 * PAGE + PAGE - 2);
    assert(constrained <= LOW + 99 * PAGE);
    mm_free_page(constrained); mm_free_page(high);
    assert(mm_free_memory_kib() == topology_free_pages() * 4);
    puts("high preference and strict inclusive DMA mask: PASS");
}
/** @brief Sparse holes, boundaries, reserved RAM, references and concurrent zeroing remain safe. */
static void ownership(void)
{
    map_ram(LOW, 2048); map_ram(HIGH, 66);
    uint64_t reserved = LOW + 1000 * PAGE;
    struct multiboot2_mmap_entry e[] = {{.addr=LOW, .len=2048*PAGE, .type=1},
                                        {.addr=HIGH, .len=66*PAGE, .type=1}};
    boot_memory(e, 2, reserved);
    assert(mm_phys_is_ram(reserved + 17, PAGE - 17));
    assert(mm_phys_is_ram(LOW, 2048 * PAGE));
    assert(!mm_phys_is_ram(LOW + 2048 * PAGE - 1, 2));
    assert(!mm_phys_is_ram(HIGH - PAGE, PAGE));
    assert(!mm_phys_is_ram(UINT64_MAX - 1, 3));
    assert(!mm_phys_is_ram(LOW, 0));
    uint64_t before = mm_free_memory_kib();
    uint64_t adjacent = mm_alloc_pages_below(1, reserved - 1);
    assert(adjacent == reserved - PAGE);
    mm_free_pages(adjacent, 2); /* A run containing reserved RAM is wholly rejected. */
    assert(mm_page_refcount(adjacent) == 1);
    mm_free_page(adjacent);
    mm_free_page(reserved); mm_retain_page(reserved);
    assert(mm_free_memory_kib() == before && mm_page_refcount(reserved) == 0);
    uint64_t page = mm_alloc_page();
    assert(page == HIGH + 65 * PAGE && mm_page_refcount(page) == 1);
    for (unsigned i = 0; i < 65536; ++i) mm_retain_page(page);
    assert(mm_page_refcount(page) == 65537);
    for (unsigned i = 0; i < 65536; ++i) mm_free_page(page);
    assert(mm_page_refcount(page) == 1);
    assert(mm_free_page_reclaimed);
    mm_retain_page(page);
    assert(!mm_free_page_reclaimed(page) && mm_page_refcount(page) == 1);
    assert(mm_free_memory_kib() == before - 4);
    assert(mm_free_page_reclaimed(page));
    assert(!mm_free_page_reclaimed(page));
    assert(!mm_free_page_reclaimed(reserved));
    assert(mm_page_refcount(page) == 0 && mm_free_memory_kib() == before);

    uint64_t block = mm_alloc_pages(8);
    assert(block == HIGH + 58 * PAGE);
    memset(paging_kernel_direct_map(block), 0xa5, 8 * PAGE);
    mm_free_pages(block, 8);
    clear_watch = block;
    assert(mm_alloc_pages(8) == block && clear_checks == 1 && !clear_watch);
    for (unsigned i = 0; i < 8 * PAGE; ++i) assert(!((unsigned char *)paging_kernel_direct_map(block))[i]);
    mm_free_pages(block, 8);
    assert(mm_free_memory_kib() == before);
    puts("holes, reservations, refcounts, double free and private zeroing: PASS");
}
/**
 * @brief Locate the reserved production metadata backing a currently owned page.
 * @param physical Aligned allocated page to locate.
 * @return Borrowed metadata section, or NULL if the bootstrap storage is missing.
 * Host-only overflow setup: real saturation would require over four billion retains.
 */
static struct mm_page_section *owned_section(uint64_t physical)
{
    const struct mm_topology *t = mm_bootstrap_topology();
    uint64_t pfn = physical / PAGE;
    for (size_t i = 0; i < t->range_count; ++i) {
        struct mm_topology_range r = t->ranges[i];
        if (!(r.flags & MM_TOPOLOGY_BOOTSTRAP)) continue;
        for (uint64_t addr = r.start; addr < r.end; addr += PAGE) {
            struct mm_page_section *s = paging_kernel_direct_map(addr);
            if (s->pages && s->pages <= MM_METADATA_PAGES_PER_SECTION &&
                pfn >= s->first_pfn && pfn - s->first_pfn < s->pages &&
                s->refs[pfn - s->first_pfn] == 1 &&
                (s->usable[(pfn - s->first_pfn) / 64] & (1ULL << ((pfn - s->first_pfn) % 64)))) return s;
        }
    }
    return NULL;
}
/** @brief Saturation pins a page even after later frees and cannot wrap its count. */
static void saturation(void)
{
    map_ram(LOW, 64);
    struct multiboot2_mmap_entry e = {.addr=LOW, .len=64*PAGE, .type=1};
    boot_memory(&e, 1, 0);
    uint64_t page = mm_alloc_page();
    struct mm_page_section *s = owned_section(page);
    assert(s);
    s->refs[page / PAGE - s->first_pfn] = UINT32_MAX - 1;
    mm_retain_page(page);
    assert(mm_page_refcount(page) == UINT32_MAX);
    mm_free_page(page); mm_retain_page(page);
    assert(mm_page_refcount(page) == UINT32_MAX);
    uint64_t other = mm_alloc_page();
    assert(other && other != page);
    puts("reference saturation remains pinned: PASS");
}
/** @brief Allocation crosses metadata sections and the DMA32 boundary only when contiguous. */
static void crossings(void)
{
    map_ram(LOW, 2048);
    uint64_t edge = HIGH - 4 * PAGE;
    map_ram(edge, 8);
    struct multiboot2_mmap_entry e[] = {{.addr=LOW, .len=2048*PAGE, .type=1},
                                        {.addr=edge, .len=8*PAGE, .type=1}};
    boot_memory(e, 2, 0);
    uint64_t bridge = mm_alloc_pages(8);
    assert(bridge == edge);
    mm_free_pages(bridge, 8);
    uint64_t block = mm_alloc_pages_below(1000, HIGH - 1);
    assert(block == LOW + 1048 * PAGE);
    assert(mm_page_refcount(block) == 1 && mm_page_refcount(block + 999 * PAGE) == 1);
    mm_free_pages(block, 1000);
    assert(mm_free_memory_kib() == topology_free_pages() * 4);
    puts("contiguous runs cross sections and DMA32 boundary: PASS");
}
/**
 * @brief Reuse pages partially freed from an allocation spanning the DMA32 boundary.
 * Uses explicit emergency admission to exhaust the pool, then exercises the
 * same unrestricted and masked scans below the ordinary reserve watermark.
 * Verifies high preference and cross-zone runs independently of reserve policy.
 */
static void crossing_partial_free(void)
{
    map_ram(LOW, 64);
    uint64_t edge = HIGH - 4 * PAGE;
    map_ram(edge, 8);
    struct multiboot2_mmap_entry entries[] = {{.addr=LOW, .len=64*PAGE, .type=1},
                                             {.addr=edge, .len=8*PAGE, .type=1}};
    boot_memory(entries, 2, 0);
    uint64_t low_count = mm_free_memory_kib() / 4 - 8;
    uint64_t low = mm_alloc_emergency_pages_below((uint32_t)low_count, LOW + 64*PAGE - 1);
    assert(low && mm_alloc_emergency_pages(8) == edge && !mm_free_memory_kib());
    uint64_t released = HIGH - PAGE;
    mm_free_page(released);
    assert(mm_free_memory_kib() == 4 && mm_page_refcount(released) == 0);
    uint64_t ordinary = mm_alloc_emergency_page();
    if (ordinary) mm_free_page(ordinary);
    uint64_t wide = mm_alloc_emergency_pages_below(1, UINT64_MAX);
    if (wide) mm_free_page(wide);
    uint64_t near_wide = mm_alloc_emergency_pages_below(1, HIGH + PAGE - 1);
    if (near_wide) mm_free_page(near_wide);
    uint64_t short_mask = mm_alloc_emergency_pages_below(1, HIGH - 2);
    uint64_t strict = mm_alloc_emergency_pages_below(1, HIGH - 1);
    if (strict) mm_free_page(strict);
    fprintf(stderr, "partial crossing ordinary=%#llx wide=%#llx near_wide=%#llx strict=%#llx short=%#llx\n",
            (unsigned long long)ordinary, (unsigned long long)wide,
            (unsigned long long)near_wide, (unsigned long long)strict,
            (unsigned long long)short_mask);
    assert(ordinary == released && wide == released && near_wide == released);
    assert(strict == released && !short_mask && mm_free_memory_kib() == 4);

    mm_free_page(HIGH);
    uint64_t preferred = mm_alloc_emergency_page();
    assert(preferred == HIGH);
    mm_free_page(preferred);
    uint64_t pair = mm_alloc_emergency_pages(2);
    assert(pair == released);
    mm_free_pages(pair, 2);
    pair = mm_alloc_emergency_pages_below(2, UINT64_MAX);
    assert(pair == released);
    mm_free_pages(pair, 2);
    assert(!mm_alloc_emergency_pages_below(2, HIGH - 1));
    puts("partially freed cross-zone run remains allocatable: PASS");
}

/** @brief Hundreds of disjoint frees do not disappear or combine across holes. */
static void fragments(void)
{
    map_ram(LOW, 1024);
    struct multiboot2_mmap_entry e = {.addr=LOW, .len=1024*PAGE, .type=1};
    boot_memory(&e, 1, 0);
    uint64_t pages[1024], count = 0;
    /* This scan model deliberately exhausts the pool, including reserve. */
    while (count < 1024 && (pages[count] = mm_alloc_emergency_page())) ++count;
    assert(count > 900 && !mm_free_memory_kib());
    for (uint64_t i = 0; i < count; i += 2) mm_free_page(pages[i]);
    assert(!mm_alloc_emergency_pages(2));
    for (uint64_t i = 1; i < count; i += 2) mm_free_page(pages[i]);
    assert(mm_free_memory_kib() == count * 4);
    puts("hundreds of fragments remain tracked and disjoint: PASS");
}

static pthread_mutex_t owners_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t thread_owners[4];
/**
 * @brief Repeatedly allocate exclusively owned pages from concurrent host threads.
 * @param argument Integer fixture slot encoded as a pointer.
 * @return NULL after releasing every acquired reference.
 */
static void *allocation_worker(void *argument)
{
    uintptr_t slot = (uintptr_t)argument;
    for (unsigned iteration = 0; iteration < 2000; ++iteration) {
        uint64_t phys = mm_alloc_page();
        assert(phys && mm_page_refcount(phys) == 1);
        pthread_mutex_lock(&owners_lock);
        for (unsigned i = 0; i < 4; ++i) assert(thread_owners[i] != phys);
        thread_owners[slot] = phys;
        pthread_mutex_unlock(&owners_lock);
        memset(paging_kernel_direct_map(phys), (int)(slot + 1), PAGE);
        pthread_mutex_lock(&owners_lock);
        assert(thread_owners[slot] == phys);
        thread_owners[slot] = 0;
        pthread_mutex_unlock(&owners_lock);
        mm_free_page(phys);
    }
    return NULL;
}
/** @brief Concurrent owners never receive the same page and leave no capacity leak. */
static void concurrency(void)
{
    map_ram(LOW, 512);
    struct multiboot2_mmap_entry e = {.addr=LOW, .len=512*PAGE, .type=1};
    boot_memory(&e, 1, 0);
    uint64_t before = mm_free_memory_kib();
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, allocation_worker, (void *)i));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    assert(mm_free_memory_kib() == before);
    puts("concurrent exclusive allocations and releases: PASS");
}

/** @brief One page in an independent allocator reference model. */
struct model_page {
    uint64_t phys; /**< Physical base. */
    bool free;     /**< Expected availability. */
};
/** @brief One run retained by the randomized reference-model test. */
struct model_run {
    uint32_t first; /**< First index in the model array. */
    uint32_t count; /**< Contiguous owned page count. */
};
static struct model_page model_pages[1300];
static struct model_run model_runs[1300];
static uint32_t model_random_state = 0x56a1137u;
/** @brief Produce deterministic fixture randomness. @return Next pseudo-random word. */
static uint32_t model_random(void)
{
    model_random_state = model_random_state * 1664525u + 1013904223u;
    return model_random_state;
}
/**
 * @brief Independently find the highest contiguous free run satisfying a byte mask.
 * @param count Number of requested pages. @param mask Inclusive byte maximum.
 * @param high_only Require the run to start above DMA32 when true.
 * @return First model index or -1 when no suitable run exists.
 */
static int model_find(uint32_t count, uint64_t mask, bool high_only)
{
    for (int first = (int)(sizeof(model_pages) / sizeof(model_pages[0])) - (int)count;
         first >= 0; --first) {
        uint64_t phys = model_pages[first].phys;
        if ((high_only && phys < HIGH) || phys > mask ||
            (uint64_t)count * PAGE - 1 > mask - phys) continue;
        uint32_t j = 0;
        while (j < count && model_pages[first + j].free &&
               model_pages[first + j].phys == phys + (uint64_t)j * PAGE) ++j;
        if (j == count) return first;
    }
    return -1;
}
/** @brief Compare randomized allocations and frees against an independent page model. */
static void reference_model(void)
{
    map_ram(LOW, 1200); map_ram(HIGH, 100);
    struct multiboot2_mmap_entry e[] = {{.addr=LOW, .len=1200*PAGE, .type=1},
                                        {.addr=HIGH, .len=100*PAGE, .type=1}};
    boot_memory(e, 2, LOW + 51 * PAGE);
    const struct mm_topology *t = mm_bootstrap_topology();
    uint64_t free_count = 0;
    for (unsigned i = 0; i < 1300; ++i) {
        model_pages[i].phys = i < 1200 ? LOW + (uint64_t)i * PAGE : HIGH + (uint64_t)(i - 1200) * PAGE;
        for (size_t r = 0; r < t->range_count; ++r) {
            if (model_pages[i].phys >= t->ranges[r].start && model_pages[i].phys < t->ranges[r].end) {
                model_pages[i].free = (t->ranges[r].flags & MM_TOPOLOGY_USABLE) != 0;
                break;
            }
        }
        free_count += model_pages[i].free;
    }
    uint32_t run_count = 0;
    for (unsigned step = 0; step < 6000; ++step) {
        uint32_t word = model_random();
        if (run_count && ((word >> 20) & 3u) == 0) {
            unsigned chosen = (model_random() >> 8) % run_count;
            struct model_run run = model_runs[chosen];
            mm_free_pages(model_pages[run.first].phys, run.count);
            for (unsigned j = 0; j < run.count; ++j) { assert(!model_pages[run.first + j].free); model_pages[run.first + j].free = true; }
            free_count += run.count;
            model_runs[chosen] = model_runs[--run_count];
        } else {
            uint32_t count = 1 + (model_random() >> 8) % 7;
            bool ordinary = ((model_random() >> 16) & 1u) != 0;
            uint64_t masks[] = {UINT64_MAX, 0xffffffffULL, LOW + ((model_random() >> 8) % 1200) * PAGE + PAGE - 1,
                                HIGH + ((model_random() >> 8) % 100) * PAGE + PAGE - 2};
            uint64_t mask = masks[(model_random() >> 16) % 4];
            int expected = ordinary ? model_find(count, UINT64_MAX, true) : -1;
            if (expected < 0) expected = model_find(count, ordinary ? UINT64_MAX : mask, false);
            /* Compare the underlying scan across the entire physical pool. */
            uint64_t actual = ordinary ? mm_alloc_emergency_pages(count) :
                                         mm_alloc_emergency_pages_below(count, mask);
            assert(actual == (expected < 0 ? 0 : model_pages[expected].phys));
            if (expected >= 0) {
                assert(run_count < 1300);
                model_runs[run_count++] = (struct model_run){(uint32_t)expected, count};
                for (unsigned j = 0; j < count; ++j) model_pages[expected + j].free = false;
                free_count -= count;
            }
        }
        assert(mm_free_memory_kib() == free_count * 4);
    }
    puts("randomized sparse allocation matches independent reference model: PASS");
}

/** @brief No unused tail bits may create pages beyond an odd-sized RAM extent. */
static void tail_bits(void)
{
    map_ram(LOW, 64); map_ram(HIGH, 66);
    struct multiboot2_mmap_entry e[] = {{.addr=LOW, .len=64*PAGE, .type=1},
                                        {.addr=HIGH, .len=66*PAGE, .type=1}};
    boot_memory(e, 2, 0);
    uint64_t all_high = mm_alloc_pages(66);
    assert(all_high == HIGH);
    uint64_t next = mm_alloc_page();
    assert(next >= LOW && next < LOW + 64 * PAGE);
    mm_free_page(next); mm_free_pages(all_high, 66);
    assert(mm_free_memory_kib() == topology_free_pages() * 4);
    puts("unused tail bits never yield fictitious RAM: PASS");
}
/** @brief Metadata exhaustion cannot publish partial runtime state or recycle reservations. */
static void initialization_failure(void)
{
    map_ram(LOW, 2);
    struct multiboot2_mmap_entry e = {.addr=LOW, .len=2*PAGE, .type=1};
    borrowed_map = (uintptr_t)&e; borrowed_bytes = sizeof(e);
    struct boot_info boot = {.mmap_addr=borrowed_map, .mmap_entry_count=1,
                             .mmap_entry_size=sizeof(e)};
    assert(mm_runtime_init(NULL) == -EINVAL);
    assert(!mm_bootstrap_prepare(&boot, 48));
    assert(mm_runtime_init(mm_bootstrap_topology()) == -ENOMEM);
    assert(!mm_free_memory_kib() && !mm_alloc_page());
    assert(!mm_bootstrap_alloc(1, UINT64_MAX));
    assert(!mm_phys_is_ram(LOW, PAGE));
    puts("metadata exhaustion publishes no runtime pool: PASS");
}

/** @brief The legacy boot coordinator halts rather than continuing after PMM failure. */
static void initialization_panic(void)
{
    map_ram(LOW, 2);
    struct multiboot2_mmap_entry e = {.addr=LOW, .len=2*PAGE, .type=1};
    borrowed_map = (uintptr_t)&e; borrowed_bytes = sizeof(e);
    struct boot_info boot = {.mmap_addr=borrowed_map, .mmap_entry_count=1,
                             .mmap_entry_size=sizeof(e)};
    panic_expected = true;
    if (!setjmp(panic_resume)) { mm_init(&boot); assert(!"failed PMM must stop boot"); }
    panic_expected = false;
    assert(!mm_free_memory_kib() && !mm_alloc_page());
    puts("mm_init stops boot on metadata exhaustion: PASS");
}

/** @brief The production heap acquires and returns pages through the sparse allocator. */
static void heap_lifetime(void)
{
    map_ram(LOW, 64);
    struct multiboot2_mmap_entry e = {.addr=LOW, .len=64*PAGE, .type=1};
    boot_memory(&e, 1, 0);
    uint64_t initial = mm_free_memory_kib();
    unsigned char *a = kernel_malloc(1);
    unsigned char *b = kernel_malloc(8193);
    assert(a && b && mm_free_memory_kib() == initial - 16);
    for (unsigned i = 0; i < 8193; ++i) assert(!b[i]);
    memset(b, 0x7a, 8193); *a = 0x31;
    kernel_free(a); kernel_free(b);
    assert(mm_free_memory_kib() == initial);
    puts("production heap allocation and page lifetime: PASS");
}

/** @brief PFNs beyond 32 bits work when a simulated larger map is actually installed. */
static void wide_pfn(void)
{
    map_ram(LOW, 64);
    map_ram(WIDE, 66); map_limit = 1ULL << 46;
    struct multiboot2_mmap_entry e[] = {{.addr=LOW, .len=64*PAGE, .type=1},
                                        {.addr=WIDE, .len=66*PAGE, .type=1}};
    boot_memory(e, 2, 0);
    assert(mm_phys_is_ram(WIDE, 66 * PAGE));
    assert(!mm_phys_is_ram(WIDE - 1, 2));
    uint64_t page = mm_alloc_page();
    assert(page == WIDE + 65 * PAGE && page / PAGE > UINT32_MAX);
    assert(mm_page_refcount(page) == 1);
    mm_free_page(page);
    assert(mm_page_refcount(page) == 0);
    assert(mm_free_memory_kib() == topology_free_pages() * 4);
    puts("sparse PFNs above UINT32_MAX with simulated mapped RAM: PASS");
}
/** @brief RAM outside the actually installed prefix must remain unavailable. */
static void clipped(void)
{
    map_ram(LOW, 64);
    struct multiboot2_mmap_entry e[] = {{.addr=LOW, .len=64*PAGE, .type=1},
                                        {.addr=WIDE, .len=66*PAGE, .type=1}};
    boot_memory(e, 2, 0);
    assert(mm_phys_is_ram(WIDE, 66 * PAGE));
    assert(!mm_phys_is_ram(WIDE + 66 * PAGE - 1, 2));
    assert(!paging_kernel_direct_map_range(WIDE, PAGE) && !mm_phys_to_virt(WIDE));
    assert(!mm_page_refcount(WIDE));
    uint64_t before = mm_free_memory_kib();
    mm_free_page(WIDE);
    assert(mm_free_memory_kib() == before);
    uint64_t page = mm_alloc_page();
    assert(page >= LOW && page < LOW + 64 * PAGE);
    puts("unmapped trusted RAM remains identifiable but excluded from allocation: PASS");
}

/** @brief Admit mapped sparse RAM around holes while preserving RAM type queries. */
static void mapped_sparse_runs(void)
{
    const uint64_t high = 0x500000000ULL;
    sparse_mapping = true;
    map_ram(LOW, 64);
    map_ram(high, 64);
    map_ram(high + 66 * PAGE, 64);
    map_ram(WIDE, 64);
    struct multiboot2_mmap_entry entries[] = {
        {.addr = LOW, .len = 64 * PAGE, .type = 1},
        {.addr = high, .len = 130 * PAGE, .type = 1},
        {.addr = WIDE, .len = 64 * PAGE, .type = 1},
    };
    borrowed_map = (uintptr_t)entries;
    borrowed_bytes = sizeof(entries);
    struct boot_info boot = {.mmap_addr = borrowed_map, .mmap_entry_count = 3,
                             .mmap_entry_size = sizeof(entries[0])};
    assert(!mm_bootstrap_prepare(&boot, 48));
    assert(!mm_runtime_init(mm_bootstrap_topology()));
    assert(mm_phys_is_ram(high + 64 * PAGE, 2 * PAGE));
    assert(!mm_page_refcount(high + 64 * PAGE));
    uint64_t wide = mm_alloc_page();
    assert(wide == WIDE + 63 * PAGE);
    mm_free_page(wide);
    uint64_t upper = mm_alloc_pages_below(64, WIDE - 1);
    assert(upper == high + 66 * PAGE);
    uint64_t lower = mm_alloc_pages_below(64, upper - 1);
    assert(lower == high);
    assert(!mm_alloc_pages_below(65, WIDE - 1));
    mm_free_pages(lower, 64);
    mm_free_pages(upper, 64);
    puts("mapped sparse RAM is allocatable across high PFNs without admitting holes: PASS");
}
/**
 * @brief Dispatch independent bootstrap scenarios in separate processes.
 * @param argc Argument count. @param argv Requested case name.
 * @return Zero on success, 2 for unknown case.
 */
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "translation")) translation();
    else if (!strcmp(argv[1], "framebuffer")) framebuffer_reservation();
    else if (!strcmp(argv[1], "small")) small_ram();
    else if (!strcmp(argv[1], "high")) prefer_high();
    else if (!strcmp(argv[1], "ownership")) ownership();
    else if (!strcmp(argv[1], "saturation")) saturation();
    else if (!strcmp(argv[1], "crossings")) crossings();
    else if (!strcmp(argv[1], "crossing_partial")) crossing_partial_free();
    else if (!strcmp(argv[1], "fragments")) fragments();
    else if (!strcmp(argv[1], "concurrency")) concurrency();
    else if (!strcmp(argv[1], "model")) reference_model();
    else if (!strcmp(argv[1], "tail")) tail_bits();
    else if (!strcmp(argv[1], "failure")) initialization_failure();
    else if (!strcmp(argv[1], "init_panic")) initialization_panic();
    else if (!strcmp(argv[1], "heap")) heap_lifetime();
    else if (!strcmp(argv[1], "wide")) wide_pfn();
    else if (!strcmp(argv[1], "clipped")) clipped();
    else if (!strcmp(argv[1], "mapped_sparse_runs")) mapped_sparse_runs();
    else return 2;
    return 0;
}
