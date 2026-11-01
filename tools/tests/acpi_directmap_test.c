/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <reliefnt/memory_topology.h>
#include "../../arch/x86_64/platform/power.c"

#define ACPI_TEST_BASE 0x130000000ULL
#define ACPI_TEST_BYTES 0x4000ULL
static uint8_t direct_alias[ACPI_TEST_BYTES];
static bool published;
static uint64_t unmapped_page;
static struct mm_topology topology;

/** @brief Discard fixture diagnostics.
 * @param level Log severity. @param format Borrowed format and arguments.
 */
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }

/** @brief Report whether the fixture has switched to its sparse topology.
 * @return Borrowed topology after publication, otherwise NULL.
 */
const struct mm_topology *mm_bootstrap_topology(void) { return published ? &topology : NULL; }
/** @brief Model the fixture CPU's 46-bit physical address width.
 * @param physical First physical byte. @param bytes Requested length.
 * @return True when the complete extent is representable.
 */
bool paging_phys_addressable(uint64_t physical, uint64_t bytes)
{ return physical < (1ULL << 46) && bytes <= (1ULL << 46) - physical; }
/** @brief Validate complete fixture-owned physical coverage.
 * @param physical First physical byte. @param bytes Requested length.
 * @return True for a complete extent inside the fixture ACPI backing.
 */
bool paging_kernel_direct_map_range(uint64_t physical, uint64_t bytes)
{
    if (unmapped_page && physical < unmapped_page + 4096 && physical + bytes > unmapped_page)
        return false;
    return published && physical >= ACPI_TEST_BASE && physical - ACPI_TEST_BASE < ACPI_TEST_BYTES &&
           bytes <= ACPI_TEST_BYTES - (physical - ACPI_TEST_BASE);
}
/** @brief Resolve the fixture's direct-map alias without returning identity.
 * @param physical First physical byte.
 * @return Borrowed alias or NULL when the byte is unmapped.
 */
void *paging_kernel_direct_map(uint64_t physical)
{
    return paging_kernel_direct_map_range(physical, 1) ? direct_alias + physical - ACPI_TEST_BASE : NULL;
}
/** @brief Set an ACPI table checksum after initializing its contents.
 * @param bytes Writable table bytes. @param count Complete table length.
 * @param checksum_offset Offset of its checksum byte.
 */
static void set_checksum(uint8_t *bytes, size_t count, size_t checksum_offset)
{
    bytes[checksum_offset] = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < count; ++i) sum += bytes[i];
    bytes[checksum_offset] = (uint8_t)(0 - sum);
}
/** @brief Preserve the legacy low-identity RSDT discovery path. */
static void test_legacy_bios_rsdt(void)
{
    const uint64_t base = 0x30000000ULL;
    uint8_t *memory = mmap((void *)base, ACPI_TEST_BYTES, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(memory != MAP_FAILED);
    struct acpi_rsdp *rsdp = (void *)memory;
    memcpy(rsdp->signature, "RSD PTR ", 8);
    rsdp->rsdt_address = (uint32_t)base + 0x1000;
    set_checksum(memory, 20, 8);
    struct acpi_sdt_header *root = (void *)(memory + 0x1000);
    memcpy(root->signature, "RSDT", 4);
    root->length = sizeof(*root) + 4;
    uint32_t child = (uint32_t)base + 0x2000;
    memcpy((uint8_t *)root + sizeof(*root), &child, 4);
    set_checksum((uint8_t *)root, root->length, 9);
    struct acpi_sdt_header *table = (void *)(memory + 0x2000);
    memcpy(table->signature, "APIC", 4);
    table->length = sizeof(*table);
    set_checksum((uint8_t *)table, table->length, 9);
    struct boot_info boot = {.rsdp_addr = base, .bios_identity_4g = true};
    published = false;
    power_init(&boot);
    assert(power_acpi_capture_resources() == 0 && power_acpi_resource_count() == 3);
    assert(power_acpi_find_table("APIC") == table);
    assert(!munmap(memory, ACPI_TEST_BYTES));
}
/** @brief Verify retained ACPI lookups follow actual direct-map aliases.
 * @return Zero on success; all identity mappings are fixture-owned.
 */
int main(void)
{
    struct efi_memory_descriptor missing_descriptor = {.type = 9, .physical_start = 0x230000000ULL,
                                                        .number_of_pages = 1};
    struct boot_info ended = {.efi_system_table = 0x230000000ULL, .rsdp_addr = 0x230000000ULL,
                              .efi_mmap_addr = (uintptr_t)&missing_descriptor,
                              .efi_mmap_entry_count = 1, .efi_mmap_entry_size = sizeof(missing_descriptor),
                              .bios_identity_4g = true};
    power_init(&ended);
    assert(!acpi_root_rsdp);
    uint8_t *memory = mmap((void *)ACPI_TEST_BASE, ACPI_TEST_BYTES, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(memory != MAP_FAILED && (uintptr_t)memory == ACPI_TEST_BASE);
    struct acpi_rsdp *rsdp = (void *)memory;
    memcpy(rsdp->signature, "RSD PTR ", 8);
    rsdp->revision = 2;
    rsdp->length = sizeof(*rsdp);
    rsdp->xsdt_address = ACPI_TEST_BASE + 0x1000;
    set_checksum(memory, 20, 8);
    set_checksum(memory, sizeof(*rsdp), 32);
    struct acpi_sdt_header *root = (void *)(memory + 0x1000);
    memcpy(root->signature, "XSDT", 4);
    root->length = sizeof(*root) + 8;
    uint64_t child = ACPI_TEST_BASE + 0x2000;
    memcpy((uint8_t *)root + sizeof(*root), &child, sizeof(child));
    set_checksum((uint8_t *)root, root->length, 9);
    struct acpi_sdt_header *table = (void *)(memory + 0x2000);
    memcpy(table->signature, "APIC", 4);
    table->length = sizeof(*table);
    set_checksum((uint8_t *)table, table->length, 9);
    uint8_t *system_table = memory + 0x3000, *config = memory + 0x3100;
    const uint8_t guid[16] = {0x71, 0xe8, 0x68, 0x88, 0xf1, 0xe4, 0xd3, 0x11,
                              0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81};
    uint64_t one = 1, config_phys = ACPI_TEST_BASE + 0x3100, rsdp_phys = ACPI_TEST_BASE;
    memcpy(system_table + 104, &one, 8);
    memcpy(system_table + 112, &config_phys, 8);
    memcpy(config, guid, sizeof(guid));
    memcpy(config + 16, &rsdp_phys, 8);
    struct efi_memory_descriptor descriptor = {.type = 9, .physical_start = ACPI_TEST_BASE,
                                                .number_of_pages = ACPI_TEST_BYTES / 4096};
    struct boot_info boot = {.efi_system_table = ACPI_TEST_BASE + 0x3000,
                             .efi_mmap_addr = (uintptr_t)&descriptor,
                             .efi_mmap_entry_size = sizeof(descriptor), .efi_mmap_entry_count = 1,
                             .efi_identity_amd64 = true};
    power_acpi_capture_efi_rsdp(&boot);
    assert(!boot.rsdp_addr); /* An ended handoff performs no EFI lookup. */
    boot.efi_boot_services_active = true;
    boot.efi_mmap_entry_count = 0;
    power_acpi_capture_efi_rsdp(&boot);
    assert(!boot.rsdp_addr);
    boot.efi_mmap_entry_count = 1;
    descriptor.type = 11; /* Memory-mapped device space is never ACPI RAM. */
    power_acpi_capture_efi_rsdp(&boot);
    assert(!boot.rsdp_addr);
    descriptor.type = 9;
    boot.efi_mmap_entry_size = sizeof(descriptor) - 1;
    power_acpi_capture_efi_rsdp(&boot);
    assert(!boot.rsdp_addr);
    boot.efi_mmap_entry_size = sizeof(descriptor);
    descriptor.number_of_pages = UINT64_MAX;
    power_acpi_capture_efi_rsdp(&boot);
    assert(!boot.rsdp_addr);
    descriptor.number_of_pages = 3;
    power_acpi_capture_efi_rsdp(&boot);
    assert(!boot.rsdp_addr); /* The system table has no proven mapped span. */
    descriptor.number_of_pages = ACPI_TEST_BYTES / 4096;
    power_acpi_capture_efi_rsdp(&boot);
    assert(boot.efi_rsdp_addr == ACPI_TEST_BASE && !boot.rsdp_addr);
    struct efi_memory_descriptor split[] = {
        {.type = 9, .physical_start = ACPI_TEST_BASE, .number_of_pages = 2},
        {.type = 10, .physical_start = ACPI_TEST_BASE + 0x2000, .number_of_pages = 2},
    };
    boot.efi_mmap_addr = (uintptr_t)split;
    boot.efi_mmap_entry_count = 2;
    assert(acpi_physical_span(ACPI_TEST_BASE + 0x1fff, 2));
    split[1].physical_start += 4096;
    assert(!acpi_physical_span(ACPI_TEST_BASE + 0x1fff, 2));
    boot.efi_mmap_addr = (uintptr_t)&descriptor;
    boot.efi_mmap_entry_count = 1;
    assert(!acpi_physical_span(ACPI_TEST_BASE + ACPI_TEST_BYTES - 1, 2));
    assert(!acpi_physical_span(UINT64_MAX - 7, 16));
    boot.efi_boot_services_active = false;
    boot.efi_system_table = ACPI_TEST_BASE + 0x100000;
    boot.efi_mmap_entry_count = 0;
    boot.rsdp_addr = 0;
    boot.efi_rsdp_addr = 0;
    power_init(&boot); /* No EFI pointer is followed after an ended handoff. */
    assert(!acpi_root_rsdp);
    struct multiboot2_info *mb = (void *)(memory + 0x3500);
    mb->total_size = 128;
    memcpy((uint8_t *)mb + sizeof(*mb), rsdp, sizeof(*rsdp));
    boot.multiboot_info = (uintptr_t)mb;
    boot.rsdp_addr = (uintptr_t)mb + sizeof(*mb);
    acpi_early_boot = &boot;
    assert(acpi_resolve_rsdp(boot.rsdp_addr)); /* Parsed handoff owns this span. */
    boot.multiboot_info = 0;
    boot.efi_system_table = ACPI_TEST_BASE + 0x3000;
    boot.efi_mmap_entry_count = 1;
    boot.rsdp_addr = 0;
    boot.efi_rsdp_addr = ACPI_TEST_BASE;
    power_init(&boot);
    assert(acpi_root_rsdp == rsdp);
    memcpy(direct_alias, memory, ACPI_TEST_BYTES);
    assert(power_acpi_capture_resources() == 0 && power_acpi_resource_count() == 3);
    uint64_t start, bytes;
    assert(power_acpi_resource(0, &start, &bytes) && start == ACPI_TEST_BASE && bytes == sizeof(*rsdp));
    assert(power_acpi_resource(1, &start, &bytes) && start == ACPI_TEST_BASE + 0x1000);
    assert(power_acpi_resource(2, &start, &bytes) && start == ACPI_TEST_BASE + 0x2000);
    assert(power_acpi_find_table("APIC") == table);
    published = true;
    assert(power_acpi_find_table("APIC") == (void *)(direct_alias + 0x2000));
    unmapped_page = ACPI_TEST_BASE + 0x2000;
    assert(!power_acpi_find_table("APIC"));
    unmapped_page = 0;
    assert(!munmap(memory, ACPI_TEST_BYTES));
    assert(power_acpi_find_table("APIC") == (void *)(direct_alias + 0x2000));
    test_legacy_bios_rsdt();
    puts("PASS retained ACPI lookups switch from identity to direct aliases");
    return 0;
}
