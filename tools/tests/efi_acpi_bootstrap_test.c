/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/memory_topology.h>
#include "../../arch/x86_64/platform/power.c"

#define TEST_BASE 0x30000000ULL
#define TEST_BYTES (256ULL * 4096)
/** @brief Discard hosted diagnostics. @param level Log level. @param format Borrowed format. */
void console_log(enum console_log_level level, const char *format, ...) { (void)level; (void)format; }
/** @brief Model CPU physical range. @param p First byte. @param n Length. @return Representability. */
bool paging_phys_addressable(uint64_t p, uint64_t n)
{ return p < (1ULL << 46) && n <= (1ULL << 46) - p; }
/** @brief Check host-backed temporary map coverage. @param p First byte. @param n Length. @return Coverage. */
bool paging_kernel_direct_map_range(uint64_t p, uint64_t n)
{ return p >= TEST_BASE && p - TEST_BASE < TEST_BYTES && n <= TEST_BYTES - (p - TEST_BASE); }
/** @brief Resolve a host-backed physical byte. @param p Physical byte. @return Borrowed alias or NULL. */
void *paging_kernel_direct_map(uint64_t p)
{ return paging_kernel_direct_map_range(p, 1) ? (void *)(uintptr_t)p : NULL; }
/** @brief Find mapped host-backed pages. @param start First page. @param end Exclusive end.
 * @param first Writable first page. @param last Writable exclusive end. @return True for overlap.
 */
bool paging_kernel_direct_map_next(uint64_t start, uint64_t end, uint64_t *first, uint64_t *last)
{
    *first = start > TEST_BASE ? start : TEST_BASE;
    *last = end < TEST_BASE + TEST_BYTES ? end : TEST_BASE + TEST_BYTES;
    return *first < *last;
}
/** @brief Supply an absent display. @return Borrowed stable fixture. */
const struct framebuffer *framebuffer_get(void) { static struct framebuffer fb; return &fb; }

/** @brief Exercise EFI configuration RSDP capture through real bootstrap reservation.
 * @return Zero when a valid external RSDP does not invalidate the handoff.
 */
int main(void)
{
    uint8_t *memory = mmap((void *)TEST_BASE, TEST_BYTES, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(memory != MAP_FAILED);
    uint8_t *system = memory + 0x1000, *config = memory + 0x1800, *rsdp = memory + 0x2000;
    struct efi_memory_descriptor *descriptor = (void *)(memory + 0x3000);
    struct multiboot2_info *mb = (void *)(memory + 0x4000);
    const uint8_t guid[] = {0x71,0xe8,0x68,0x88,0xf1,0xe4,0xd3,0x11,
                            0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81};
    uint64_t count = 1, config_phys = (uintptr_t)config, rsdp_phys = (uintptr_t)rsdp;
    memcpy(system + 104, &count, 8); memcpy(system + 112, &config_phys, 8);
    memcpy(config, guid, 16); memcpy(config + 16, &rsdp_phys, 8);
    memcpy(rsdp, "RSD PTR ", 8);
    uint8_t sum = 0; for (unsigned i = 0; i < 20; ++i) sum += rsdp[i]; rsdp[8] = (uint8_t)(0 - sum);
    *descriptor = (struct efi_memory_descriptor){.type = 7, .physical_start = TEST_BASE,
                                                  .number_of_pages = TEST_BYTES / 4096};
    mb->total_size = 16;
    struct boot_info boot = {.multiboot_info = (uintptr_t)mb, .efi_system_table = (uintptr_t)system,
                             .efi_mmap_addr = (uintptr_t)descriptor, .efi_mmap_entry_count = 1,
                             .efi_mmap_entry_size = sizeof(*descriptor), .efi_boot_services_active = true,
                             .efi_identity_amd64 = true};
    power_acpi_capture_efi_rsdp(&boot);
    power_init(&boot);
    assert(power_acpi_capture_resources() == 0 && power_acpi_resource_count() == 1);
    size_t workspace_bytes = 0;
    boot.rsdp_addr = rsdp_phys;
    assert(mm_topology_workspace_size(&boot, &workspace_bytes) < 0);
    boot.rsdp_addr = 0;
    assert(!mm_topology_workspace_size(&boot, &workspace_bytes) && workspace_bytes);
    assert(mm_bootstrap_prepare(&boot, 46) == 0);
    const struct mm_topology *topology = mm_bootstrap_topology();
    bool reserved = false;
    for (size_t i = 0; i < topology->range_count; ++i)
        if (topology->ranges[i].start <= rsdp_phys && topology->ranges[i].end > rsdp_phys)
            reserved = (topology->ranges[i].flags & MM_TOPOLOGY_RESERVED) != 0;
    assert(reserved);
    puts("PASS external EFI RSDP passes bootstrap and remains reserved");
    return 0;
}
