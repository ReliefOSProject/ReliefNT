/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/memory_topology.h>
#include <reliefnt/multiboot2.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/power.h>
#include <reliefnt/paging.h>
#include <linux/errno.h>

#define DIRECT_PAGE 4096ULL
#define DIRECT_LARGE (2ULL * 1024 * 1024)
#define DIRECT_LARGE_FLAG 0x80ULL
#define DIRECT_BOOT_LIMIT (16ULL * 1024 * 1024 * 1024)

/* Permanently shared PML4 branches; kernel image reservation owns these pages. */
static uint64_t direct_branches[128][512] __attribute__((aligned(4096)));
/** @brief BSP publication guard; an installed branch must never be cleared. */
static bool direct_initialized;

/** @brief Reserve and clear one table while the temporary low map is installed.
 * @param entry Writable parent entry to install only after allocation succeeds.
 * @return Borrowed table alias or NULL on exhaustion; bootstrap retains pages.
 */
static uint64_t *direct_new_table(uint64_t *entry)
{
    uint64_t physical = mm_bootstrap_alloc(1, DIRECT_BOOT_LIMIT - 1);
    if (!physical) return NULL;
    uint64_t *table = paging_trusted_table_alias(physical);
    if (!table) return NULL;
    for (unsigned i = 0; i < 512; ++i) table[i] = 0;
    *entry = physical | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE;
    return table;
}

/** @brief Ensure a staging PDE exists for one representable physical page.
 * @param physical Physical page in the requested direct-map window.
 * @return Borrowed staging PDE or NULL if bootstrap table allocation fails.
 */
static uint64_t *direct_stage_pde(uint64_t physical)
{
    uint64_t *pdpt = direct_branches[physical >> 39];
    uint64_t *pdpte = &pdpt[(physical >> 30) & 511];
    uint64_t *pd = (*pdpte & RELIEFNT_PAGE_PRESENT) ?
        paging_trusted_table_alias(*pdpte & RELIEFNT_PHYS_ADDR_MASK) :
        direct_new_table(pdpte);
    return pd ? &pd[(physical >> 21) & 511] : NULL;
}

/** @brief Split one staging large leaf while retaining its original attributes.
 * @param pde Writable present 2 MiB leaf in the staging map.
 * @return Borrowed 4 KiB page table or NULL; a failed allocation leaves the leaf.
 */
static uint64_t *direct_split(uint64_t *pde)
{
    uint64_t old = *pde, new_entry = 0;
    uint64_t *pt = direct_new_table(&new_entry);
    if (!pt) return NULL;
    uint64_t physical = old & RELIEFNT_PHYS_ADDR_MASK & ~(DIRECT_LARGE - 1);
    uint64_t flags = old & ~RELIEFNT_PHYS_ADDR_MASK & ~DIRECT_LARGE_FLAG;
    if (old & (1ULL << 12)) flags |= RELIEFNT_PAGE_PAT;
    for (uint32_t i = 0; i < 512; ++i) pt[i] = physical + i * DIRECT_PAGE | flags;
    *pde = new_entry;
    return pt;
}

/** @brief Map or reattribute a complete aligned physical staging extent.
 * @param start Inclusive page-aligned physical base.
 * @param end Exclusive page-aligned physical end.
 * @param flags Leaf flags, including present and the desired cache policy.
 * @return 0 or negative errno; staging allocations stay bootstrap-owned on error.
 */
static int direct_stage_range(uint64_t start, uint64_t end, uint64_t flags)
{
    if ((start | end) & (DIRECT_PAGE - 1) || start >= end ||
        end > RELIEFNT_KERNEL_DIRECT_MAP_SIZE ||
        !paging_phys_addressable(start, end - start)) return -EINVAL;
    while (start < end) {
        uint64_t *pde = direct_stage_pde(start);
        if (!pde) return -ENOMEM;
        if (!(*pde & RELIEFNT_PAGE_PRESENT) && !(start & (DIRECT_LARGE - 1)) &&
            end - start >= DIRECT_LARGE) {
            uint64_t large_flags=flags & ~RELIEFNT_PAGE_PAT;
            if(flags & RELIEFNT_PAGE_PAT)large_flags|=1ULL<<12;
            *pde = start | large_flags | DIRECT_LARGE_FLAG;
            start += DIRECT_LARGE;
            continue;
        }
        uint64_t *pt;
        if (*pde & DIRECT_LARGE_FLAG) {
            uint64_t old_flags = *pde & ~RELIEFNT_PHYS_ADDR_MASK & ~DIRECT_LARGE_FLAG;
            if(*pde & (1ULL<<12))old_flags|=RELIEFNT_PAGE_PAT;
            if (old_flags == flags) {
                uint64_t span = DIRECT_LARGE - (start & (DIRECT_LARGE - 1));
                start += span < end - start ? span : end - start;
                continue;
            }
            pt = direct_split(pde);
        } else if (*pde & RELIEFNT_PAGE_PRESENT)
            pt = paging_trusted_table_alias(*pde & RELIEFNT_PHYS_ADDR_MASK);
        else
            pt = direct_new_table(pde);
        if (!pt) return -ENOMEM;
        pt[(start >> 12) & 511] = start | flags;
        start += DIRECT_PAGE;
    }
    return 0;
}

/** @brief Round and map an explicit boot resource without overflow.
 * @param start Inclusive physical byte start.
 * @param bytes Nonzero byte length, or zero for an absent resource.
 * @param flags Complete staging leaf flags.
 * @return 0 or negative errno when the resource is unrepresentable.
 */
static int direct_stage_resource(uint64_t start, uint64_t bytes, uint64_t flags)
{
    if (!bytes) return 0;
    if (bytes > UINT64_MAX - start || start + bytes > UINT64_MAX - (DIRECT_PAGE - 1))
        return -EOVERFLOW;
    uint64_t end = (start + bytes + DIRECT_PAGE - 1) & ~(DIRECT_PAGE - 1);
    return direct_stage_range(start & ~(DIRECT_PAGE - 1), end, flags);
}

/** @brief Convert a retained temporary direct-map or identity pointer to physical.
 * @param address Borrowed boot buffer address.
 * @return Its physical byte address; does not inspect its contents.
 */
static uint64_t direct_boot_physical(uint64_t address)
{
    if (address >= RELIEFNT_KERNEL_DIRECT_MAP_BASE &&
        address - RELIEFNT_KERNEL_DIRECT_MAP_BASE < RELIEFNT_KERNEL_DIRECT_MAP_SIZE)
        return address - RELIEFNT_KERNEL_DIRECT_MAP_BASE;
    return address;
}

/** @brief Check whether an aligned overlap is inside a display reservation.
 * @param start Inclusive overlap start. @param end Exclusive overlap end.
 * @param base Raw display reservation base. @param bytes Reservation length.
 * @return True when the entire overlap lies in the rounded reservation.
 */
static bool direct_display_contains(uint64_t start, uint64_t end,
                                    uint64_t base, uint64_t bytes)
{
    if (!base || !bytes || bytes > UINT64_MAX - base ||
        base + bytes > UINT64_MAX - (DIRECT_PAGE - 1)) return false;
    uint64_t first = base & ~(DIRECT_PAGE - 1);
    uint64_t last = (base + bytes + DIRECT_PAGE - 1) & ~(DIRECT_PAGE - 1);
    return start >= first && end <= last;
}

/** @brief Reject early device claims overlapping RAM outside reserved display.
 * @param topology Current bootstrap view. @param display Retained display data.
 * @param start Aligned claim start. @param end Aligned exclusive claim end.
 * @param uncached True when the claim requires UC attributes.
 * @return True only when all RAM overlaps are explicit UC display reservations.
 */
static bool direct_claim_safe(const struct mm_topology *topology,
                              const struct framebuffer *display,
                              uint64_t start, uint64_t end, bool uncached)
{
    for (size_t i = 0; i < topology->range_count; ++i) {
        struct mm_topology_range r = topology->ranges[i];
        if (!(r.flags & MM_TOPOLOGY_RAM) || r.end <= start || r.start >= end) continue;
        uint64_t first = r.start > start ? r.start : start;
        uint64_t last = r.end < end ? r.end : end;
        if (!uncached || !display ||
            (!direct_display_contains(first, last, display->reservation_start,
                                      display->reservation_bytes) &&
             !direct_display_contains(first, last, display->auxiliary_reservation_start,
                                      display->auxiliary_reservation_bytes))) return false;
    }
    return true;
}

/** @brief Check a rounded device claim against one retained boot extent.
 * @param start Inclusive page-aligned claim start. @param end Exclusive claim end.
 * @param base Resource physical base. @param bytes Resource byte length.
 * @return True on overlap or malformed resource arithmetic.
 */
static bool direct_resource_overlap(uint64_t start, uint64_t end,
                                    uint64_t base, uint64_t bytes)
{
    if (!bytes) return false;
    if (bytes > UINT64_MAX - base) return true;
    return start < base + bytes && end > (base & ~(DIRECT_PAGE - 1));
}

/** @brief Reject device cache claims that cover image or borrowed boot data.
 * @param boot Borrowed handoff in the temporary map. @param base Device base.
 * @param bytes Device length, zero for an absent claim.
 * @return True only when the rounded claim is disjoint from retained data.
 */
static bool direct_boot_data_disjoint(const struct boot_info *boot,
                                      uint64_t base, uint64_t bytes)
{
    if (!bytes) return true;
    if (bytes > UINT64_MAX - base || base + bytes > UINT64_MAX - (DIRECT_PAGE - 1))
        return false;
    uint64_t start = base & ~(DIRECT_PAGE - 1);
    uint64_t end = (base + bytes + DIRECT_PAGE - 1) & ~(DIRECT_PAGE - 1);
    if (direct_resource_overlap(start, end, RELIEFNT_KERNEL_HOLE_START,
                                RELIEFNT_KERNEL_HOLE_END - RELIEFNT_KERNEL_HOLE_START))
        return false;
    if (boot->multiboot_info) {
        const struct multiboot2_info *info = (const void *)(uintptr_t)boot->multiboot_info;
        if (direct_resource_overlap(start, end, direct_boot_physical(boot->multiboot_info),
                                    info->total_size)) return false;
    }
    if (direct_resource_overlap(start, end, direct_boot_physical(boot->mmap_addr),
                                (uint64_t)boot->mmap_entry_count * boot->mmap_entry_size) ||
        direct_resource_overlap(start, end, direct_boot_physical(boot->efi_mmap_addr),
                                (uint64_t)boot->efi_mmap_entry_count * boot->efi_mmap_entry_size))
        return false;
    for (uint32_t i = 0; i < boot->module_count; ++i) {
        if (boot->modules[i].end < boot->modules[i].start ||
            direct_resource_overlap(start, end, boot->modules[i].start,
                                    boot->modules[i].end - boot->modules[i].start)) return false;
    }
    return true;
}

/**
 * @brief Construct sparse RAM and retained resource branches, then switch CR3.
 * @param topology Published bootstrap topology, which table reservations mutate.
 * @return 0 or negative errno. BSP only with temporary map and NXE active;
 * failed staging pages remain bootstrap-owned and no new branch is published;
 * a second call after publication returns -EBUSY without changing mappings.
 */
int paging_direct_map_init(const struct mm_topology *topology)
{
    if (direct_initialized) return -EBUSY;
    const struct boot_info *boot = mm_bootstrap_boot_info();
    if (!topology || topology != mm_bootstrap_topology() || !boot || !paging_nx_ready())
        return -EINVAL;
    for (unsigned i = 0; i < 128; ++i)
        for (unsigned j = 0; j < 512; ++j) direct_branches[i][j] = 0;
    const uint64_t wb = RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC;
    uint64_t cursor = 0;
    for (;;) {
        struct mm_topology_range range = {0};
        for (size_t i = 0; i < topology->range_count; ++i) {
            struct mm_topology_range candidate = topology->ranges[i];
            if ((candidate.flags & MM_TOPOLOGY_RAM) && candidate.end > cursor) {
                range = candidate;
                break;
            }
        }
        if (!range.end) break;
        uint64_t first = range.start > cursor ? range.start : cursor;
        int rc = direct_stage_range(first, range.end, wb);
        if (rc) return rc;
        cursor = range.end; /* Allocations may mutate the topology; rescan. */
    }
    int rc = direct_stage_resource(0, 0x100000, wb);
    if (!rc) rc = direct_stage_resource(RELIEFNT_KERNEL_HOLE_START,
        RELIEFNT_KERNEL_HOLE_END - RELIEFNT_KERNEL_HOLE_START, wb);
    if (!rc && boot->multiboot_info) {
        const struct multiboot2_info *info = (const void *)(uintptr_t)boot->multiboot_info;
        rc = direct_stage_resource(direct_boot_physical(boot->multiboot_info), info->total_size, wb);
    }
    if (!rc && boot->mmap_entry_count)
        rc = direct_stage_resource(direct_boot_physical(boot->mmap_addr),
            (uint64_t)boot->mmap_entry_count * boot->mmap_entry_size, wb);
    if (!rc && boot->efi_mmap_entry_count)
        rc = direct_stage_resource(direct_boot_physical(boot->efi_mmap_addr),
            (uint64_t)boot->efi_mmap_entry_count * boot->efi_mmap_entry_size, wb);
    for (uint32_t i = 0; !rc && i < boot->module_count; ++i)
        rc = direct_stage_resource(boot->modules[i].start,
            boot->modules[i].end - boot->modules[i].start, wb);
    for (uint32_t i = 0; !rc && i < power_acpi_resource_count(); ++i) {
        uint64_t start, bytes;
        if (!power_acpi_resource(i, &start, &bytes)) return -EINVAL;
        rc = direct_stage_resource(start, bytes, wb);
    }
    if (rc) return rc;
    const struct framebuffer *display = framebuffer_get();
    uint64_t uc = wb | RELIEFNT_PAGE_PWT | RELIEFNT_PAGE_PCD;
    if (display) {
        if (!direct_boot_data_disjoint(boot, display->reservation_start, display->reservation_bytes) ||
            !direct_boot_data_disjoint(boot, display->auxiliary_reservation_start,
                                       display->auxiliary_reservation_bytes)) return -EINVAL;
        for (uint32_t i = 0; i < power_acpi_resource_count(); ++i) {
            uint64_t base, bytes;
            if (!power_acpi_resource(i, &base, &bytes) || bytes > UINT64_MAX - base ||
                base + bytes > UINT64_MAX - (DIRECT_PAGE - 1) ||
                display->reservation_bytes > UINT64_MAX - display->reservation_start ||
                display->auxiliary_reservation_bytes > UINT64_MAX - display->auxiliary_reservation_start)
                return -EINVAL;
            uint64_t first = base & ~(DIRECT_PAGE - 1);
            uint64_t last = (base + bytes + DIRECT_PAGE - 1) & ~(DIRECT_PAGE - 1);
            if (direct_display_contains(first, last, display->reservation_start,
                                        display->reservation_bytes) ||
                direct_display_contains(first, last, display->auxiliary_reservation_start,
                                        display->auxiliary_reservation_bytes)) return -EINVAL;
            /* Partial intersections also conflict; page-rounded claims must be disjoint. */
            if (display->reservation_start && display->reservation_bytes &&
                first < display->reservation_start + display->reservation_bytes &&
                last > (display->reservation_start & ~(DIRECT_PAGE - 1))) return -EINVAL;
            if (display->auxiliary_reservation_start && display->auxiliary_reservation_bytes &&
                first < display->auxiliary_reservation_start + display->auxiliary_reservation_bytes &&
                last > (display->auxiliary_reservation_start & ~(DIRECT_PAGE - 1))) return -EINVAL;
        }
        rc = direct_stage_resource(display->reservation_start, display->reservation_bytes, uc);
        if (!rc) rc = direct_stage_resource(display->auxiliary_reservation_start,
                                             display->auxiliary_reservation_bytes, uc);
        if (rc) return rc;
    }
    for (uint32_t i = 0;; ++i) {
        uint64_t start, end;
        uint64_t cache_flags;
        if (!paging_boot_mmio_run(i, &start, &end, &cache_flags)) break;
        if (end < start || !direct_boot_data_disjoint(boot, start, end - start)) return -EINVAL;
        for (uint32_t j = 0; j < power_acpi_resource_count(); ++j) {
            uint64_t base, bytes;
            if (!power_acpi_resource(j, &base, &bytes) || bytes > UINT64_MAX - base)
                return -EINVAL;
            if (start < base + bytes && end > base) return -EINVAL;
        }
        if (!direct_claim_safe(topology, display, start, end,
                cache_flags==(RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD))) return -EINVAL;
        uint64_t flags = wb | cache_flags;
        rc = direct_stage_range(start, end, flags);
        if (rc) return rc;
    }
    paging_publish_direct_map(direct_branches);
    direct_initialized = true;
    return 0;
}
