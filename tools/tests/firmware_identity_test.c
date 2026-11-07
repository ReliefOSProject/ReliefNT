/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../../arch/x86_64/platform/platform.c"

#define FIXTURE_BASE 0x130000000ULL
/** @brief Model the fixture CPU's 46-bit physical limit.
 * @param physical First byte. @param bytes Extent length. @return Representability.
 */
bool paging_phys_addressable(uint64_t physical, uint64_t bytes)
{ return physical < (1ULL << 46) && bytes <= (1ULL << 46) - physical; }
/** @brief Suppress console output from the fixture.
 * @param level Log level. @param format Borrowed format string.
 */
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }

/** @brief Verify valid and unproved already-ended EFI identity pointers.
 * @return Zero after all fixture assertions succeed.
 */
int main(void)
{
    struct efi_memory_descriptor missing_descriptor = {.type = 6, .physical_start = 0x230000000ULL,
                                                        .number_of_pages = 1};
    struct boot_info ended = {.efi_system_table = 0x230000000ULL,
                              .efi_mmap_addr = (uintptr_t)&missing_descriptor,
                              .efi_mmap_entry_count = 1, .efi_mmap_entry_size = sizeof(missing_descriptor),
                              .bios_identity_4g = true};
    platform_identity_init(&ended);
    assert(!platform_dmi()->values[7][0]);
    struct efi_memory_descriptor low = {.type = 6, .physical_start = 0xfffff000ULL,
                                        .number_of_pages = 2};
    ended.efi_mmap_addr = (uintptr_t)&low;
    assert(firmware_identity_range(&ended, 0xfffff000ULL, 4096));
    assert(!firmware_identity_range(&ended, 0xfffff000ULL, 4097));
    ended.bios_identity_4g = false;
    assert(!firmware_identity_range(&ended, 0xfffff000ULL, 1));
    uint8_t *memory = mmap((void *)FIXTURE_BASE, 8192, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(memory != MAP_FAILED);
    struct efi_system_table *st = (void *)memory;
    struct efi_configuration_table *config = (void *)(memory + 0x100);
    struct smbios3_entry *entry = (void *)(memory + 0x200);
    uint8_t *table = memory + 0x300;
    uint16_t *vendor = (void *)(memory + 0x800);
    vendor[0] = 'E'; vendor[1] = 'F'; vendor[2] = 'I';
    st->firmware_vendor = vendor;
    st->firmware_revision = 42;
    st->number_of_table_entries = 1;
    st->configuration_table = config;
    config->vendor_guid = (struct efi_guid){0xf2fd1544U, 0x9794U, 0x4a2cU,
                                            {0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94}};
    config->vendor_table = entry;
    memcpy(entry->anchor, "_SM3_", 5);
    entry->length = sizeof(*entry);
    entry->major = 3; entry->minor = 2;
    entry->table_address = (uintptr_t)table;
    entry->table_max_size = 64;
    table[0] = 1; table[1] = 27;
    for (unsigned i = 0; i < 16; ++i) table[8 + i] = i + 1;
    table[4] = 1; table[5] = 2;
    const char strings[] = "Test Vendor\0Test Machine\0";
    memcpy(table + 27, strings, sizeof(strings));
    uint8_t sum = 0;
    for (unsigned i = 0; i < sizeof(*entry); ++i) sum += ((uint8_t *)entry)[i];
    entry->checksum = (uint8_t)(0 - sum);
    struct efi_memory_descriptor descriptor = {.type = 6, .physical_start = FIXTURE_BASE,
                                                .number_of_pages = 2};
    struct boot_info boot = {.efi_system_table = FIXTURE_BASE,
                             .efi_mmap_addr = (uintptr_t)&descriptor,
                             .efi_mmap_entry_size = sizeof(descriptor), .efi_mmap_entry_count = 1,
                             .efi_identity_amd64 = true};
    platform_identity_init(&boot);
    assert(!strcmp(platform_dmi()->values[7], "04030201-0605-0807-090a-0b0c0d0e0f10"));
    assert(!strcmp(platform_dmi()->values[3], "Test Vendor"));
    assert(!strcmp(platform_dmi()->values[4], "Test Machine"));
    boot.efi_system_table = FIXTURE_BASE + 0x100000;
    platform_identity_init(&boot);
    assert(!platform_dmi()->values[7][0]);
    boot.efi_system_table = FIXTURE_BASE;
    descriptor.number_of_pages = 1;
    st->configuration_table = config;
    descriptor.type = 11;
    platform_identity_init(&boot);
    assert(!platform_dmi()->values[7][0]);
    descriptor.type = 6;
    entry->table_address = FIXTURE_BASE + 4096;
    platform_identity_init(&boot);
    assert(!platform_dmi()->values[7][0]);
    entry->table_address = (uintptr_t)table;
    st->firmware_vendor = (void *)(FIXTURE_BASE + 4096);
    platform_identity_init(&boot);
    assert(!strcmp(platform_dmi()->values[7], "04030201-0605-0807-090a-0b0c0d0e0f10"));
    boot.efi_system_table = FIXTURE_BASE;
    descriptor.number_of_pages = 1;
    st->configuration_table = (void *)(FIXTURE_BASE + 4096);
    platform_identity_init(&boot);
    assert(!platform_dmi()->values[7][0]);
    assert(!munmap(memory, 8192));
    puts("PASS DMI sysfs inventory preserves UUID/vendor/model and rejects unproved EFI spans");
    return 0;
}
