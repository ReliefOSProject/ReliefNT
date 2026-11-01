/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/errno.h>
#include <reliefnt/memory_topology.h>
#include <reliefnt/multiboot2.h>
#include <reliefnt/framebuffer.h>

#include "../../arch/x86_64/mm/paging.c"

static uint64_t pdpt0[512] __attribute__((aligned(4096)));
static uint64_t pdpt1[512] __attribute__((aligned(4096)));
static uint64_t pdpt32[512] __attribute__((aligned(4096)));
static uint64_t low_pd[512] __attribute__((aligned(4096)));
static uint64_t low_pt[512] __attribute__((aligned(4096)));
static uint64_t high_pd[512] __attribute__((aligned(4096)));
static uint64_t fixture_tables[128][512] __attribute__((aligned(4096)));
static unsigned table_count, table_budget, cr3_loads;
static struct mm_topology fixture_topology;
static struct boot_info fixture_boot;
static struct framebuffer fixture_display;
static uint64_t fixture_acpi_base, fixture_acpi_size;

/** @brief Count the fixture's optional ACPI resource.
 * @return One when configured, otherwise zero.
 */
uint32_t power_acpi_resource_count(void) { return fixture_acpi_size ? 1 : 0; }
/** @brief Return the fixture's optional ACPI resource.
 * @param index Resource index. @param start Required physical output.
 * @param bytes Required length output.
 * @return True for the configured resource, false otherwise.
 */
bool power_acpi_resource(uint32_t index, uint64_t *start, uint64_t *bytes)
{
    if (index || !fixture_acpi_size || !start || !bytes) return false;
    *start = fixture_acpi_base; *bytes = fixture_acpi_size; return true;
}

/** @brief Translate a page-table address from this host fixture's static tables.
 * @param physical Fixture physical address, represented by its host pointer.
 * @return Borrowed host table pointer; the fixture owns all static tables.
 */
uint64_t *paging_trusted_table_alias(uint64_t physical)
{
    if (physical >= 0x20000000ULL && physical < 0x20000000ULL + sizeof(fixture_tables))
        return (uint64_t *)((uint8_t *)fixture_tables + (physical - 0x20000000ULL));
    return (uint64_t *)(uintptr_t)physical;
}

/** @brief Borrow the hosted bootstrap topology.
 * @return Fixture-owned topology throughout construction.
 */
const struct mm_topology *mm_bootstrap_topology(void) { return &fixture_topology; }

/** @brief Borrow retained hosted boot resources.
 * @return Fixture-owned handoff throughout construction.
 */
const struct boot_info *mm_bootstrap_boot_info(void) { return &fixture_boot; }

/** @brief Borrow the fixture's retained display description.
 * @return Stable fixture-owned framebuffer descriptor.
 */
const struct framebuffer *framebuffer_get(void) { return &fixture_display; }

/** @brief Supply one owned low staging table with controlled allocation failure.
 * @param pages Number of pages; the builder requests one at a time.
 * @param inclusive_max Inclusive physical byte limit.
 * @return Fake physical table address or zero on budget exhaustion.
 */
uint64_t mm_bootstrap_alloc(uint32_t pages, uint64_t inclusive_max)
{
    assert(pages == 1 && inclusive_max >= 0x20000000ULL + sizeof(fixture_tables) - 1);
    if (table_count >= table_budget || table_count >= 128) return 0;
    return 0x20000000ULL + (uint64_t)table_count++ * 4096;
}

/** @brief Record rather than execute a privileged root switch.
 * @param physical Physical address of the fixture kernel root.
 */
void x86_64_load_cr3(uint64_t physical)
{ assert(physical == (uintptr_t)kernel_pml4); ++cr3_loads; }

/** @brief Construct sparse branches, publish a high GOP alias and fail atomically. */
static void staged_map(void)
{
    const uint64_t high = 20ULL << 30, wide = (16ULL << 40) + 0x10000,
                   high_gop = 32ULL << 30;
    struct mm_topology_range ranges[] = {
        {0x100000, 0x180000, MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE},
        {high, high + 0x400000, MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE},
        {wide, wide + 0x8000, MM_TOPOLOGY_RAM | MM_TOPOLOGY_USABLE},
    };
    fixture_topology = (struct mm_topology){.ranges = ranges, .range_count = 3,
                                            .range_capacity = 3, .physical_limit = 1ULL << 46};
    fixture_boot = (struct boot_info){0};
    fixture_boot.module_count = 1;
    fixture_boot.modules[0].start = 24ULL << 30;
    fixture_boot.modules[0].end = (24ULL << 30) + 8192;
    fixture_acpi_base = (28ULL << 30) + 8;
    fixture_acpi_size = 6000;
    nx_enabled = true;
    mmio_run_count = 1;
    mmio_runs[0] = (struct mmio_run){.start = high + 4096, .end = high + 8192,
                                     .permanent = 1, .policy = MMIO_CACHE_POLICY_UC};
    fixture_display.reservation_start = high + 4096;
    fixture_display.reservation_bytes = 4096;
    fixture_display.auxiliary_reservation_start = high_gop;
    fixture_display.auxiliary_reservation_bytes = 4096;
    fixture_display.pixels = (void *)(uintptr_t)(RELIEFNT_KERNEL_DIRECT_MAP_BASE + high_gop);
    uint64_t previous = (uintptr_t)pdpt0 | 3;
    kernel_pml4[256] = previous;
    kernel_pml4[511] = (uintptr_t)kernel_shared_pdpt | 3;
    assert(paging_phys_addressable(high_gop, 4096));
    assert(!paging_kernel_direct_map_range(high_gop, 4096));
    assert(!paging_kernel_direct_map(high_gop));
    fixture_acpi_base = high + 4096;
    fixture_acpi_size = 4096;
    table_count = 0;
    table_budget = 128;
    assert(paging_direct_map_init(&fixture_topology) < 0);
    assert(kernel_pml4[256] == previous && !cr3_loads);
    fixture_acpi_base = (28ULL << 30) + 8;
    fixture_acpi_size = 6000;
    fixture_display.reservation_start = fixture_boot.modules[0].start;
    mmio_run_count = 0;
    table_count = 0;
    assert(paging_direct_map_init(&fixture_topology) < 0);
    assert(kernel_pml4[256] == previous && !cr3_loads);
    fixture_display.reservation_start = high + 4096;
    mmio_run_count = 1;
    unsigned budget;
    for (budget = 0; budget < 128; ++budget) {
        table_count = 0;
        table_budget = budget;
        memset(fixture_tables, 0, sizeof(fixture_tables));
        int rc = paging_direct_map_init(&fixture_topology);
        if (!rc) break;
        assert(kernel_pml4[256] == previous && !cr3_loads);
    }
    assert(budget < 128 && cr3_loads == 1);
    for (unsigned i = 256; i < 384; ++i) assert(kernel_pml4[i] & RELIEFNT_PAGE_PRESENT);
    assert(kernel_pml4[511] == ((uintptr_t)kernel_shared_pdpt | 3));
    assert(paging_kernel_direct_map_range(high, 0x400000));
    assert(!paging_kernel_direct_map_range(high + 0x400000, 4096));
    assert(paging_kernel_direct_map_range(wide, 0x8000));
    assert(paging_kernel_direct_map_range(24ULL << 30, 8192));
    assert(paging_kernel_direct_map_range(fixture_acpi_base, fixture_acpi_size));
    assert(paging_kernel_direct_map_range(high_gop, 4096));
    assert(paging_kernel_direct_map(high_gop) == fixture_display.pixels);
    assert(paging_direct_map_init(&fixture_topology) == -EBUSY);
    assert(cr3_loads == 1 && paging_kernel_direct_map_range(high, 0x400000));
    uint64_t *leaf = mmio_existing_leaf(high + 4096);
    assert(leaf && (*leaf & (RELIEFNT_PAGE_PWT | RELIEFNT_PAGE_PCD | RELIEFNT_PAGE_NOEXEC)) ==
        (RELIEFNT_PAGE_PWT | RELIEFNT_PAGE_PCD | RELIEFNT_PAGE_NOEXEC));
    leaf = mmio_existing_leaf(high_gop);
    assert(leaf && (*leaf & (RELIEFNT_PAGE_PWT | RELIEFNT_PAGE_PCD | RELIEFNT_PAGE_NOEXEC)) ==
        (RELIEFNT_PAGE_PWT | RELIEFNT_PAGE_PCD | RELIEFNT_PAGE_NOEXEC));
    puts("PASS staged sparse map, high GOP alias, boot resource and UC replay, allocation rollback");
}

/** @brief Verify sparse coverage and exact physical identity across page levels.
 * @return Zero after successful assertions.
 */
int main(void)
{
    const uint64_t flags = RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE;
    const uint64_t huge = flags | PAGE_SIZE_FLAG;
    const uint64_t high = 20ULL << 30;
    const uint64_t wide = 16ULL << 40;
    const uint64_t edge = 1ULL << 39;
    assert(RELIEFNT_KERNEL_DIRECT_MAP_BASE == 0xffff800000000000ULL);
    assert(RELIEFNT_KERNEL_DIRECT_MAP_SIZE == 1ULL << 46);
    uint8_t bits = paging_physical_bits();
    assert(bits >= 32 && bits <= 52);
    uint64_t physical_limit = 1ULL << bits;
    assert(paging_phys_addressable(physical_limit - 1, 1));
    assert(!paging_phys_addressable(physical_limit - 1, 2));
    assert(!paging_phys_addressable(physical_limit, 1));

    kernel_pml4[256] = (uintptr_t)pdpt0 | flags;
    kernel_pml4[257] = (uintptr_t)pdpt1 | flags;
    kernel_pml4[288] = (uintptr_t)pdpt32 | flags;
    pdpt0[0] = (uintptr_t)low_pd | flags;
    low_pd[0] = (uintptr_t)low_pt | flags;
    low_pt[0] = flags;
    low_pt[2] = 0x3000 | flags; /* Present, but maps the wrong physical page. */
    pdpt0[20] = (uintptr_t)high_pd | flags;
    high_pd[0] = high | huge;
    pdpt0[511] = (edge - (1ULL << 30)) | huge;
    pdpt1[0] = edge | huge;
    pdpt32[0] = wide | huge;

    assert(paging_kernel_direct_map_range(0, 4096));
    assert(!paging_kernel_direct_map_range(4096, 1));
    assert(!paging_kernel_direct_map_range(8192, 1));
    assert(!paging_kernel_direct_map(8192));
    assert(paging_kernel_direct_map_range(high + 4095, 0x1ff001));
    assert(!paging_kernel_direct_map_range(high + 0x1ff000, 8192));
    assert(paging_kernel_direct_map_range(edge - 4096, 8192));
    assert(paging_kernel_direct_map_range(wide + 0x1234, 4096));
    assert(!paging_kernel_direct_map_range(RELIEFNT_KERNEL_DIRECT_MAP_SIZE - 1, 2));
    puts("PASS sparse direct-map coverage, identity, large pages and PML4 crossing");
    staged_map();
    return 0;
}
