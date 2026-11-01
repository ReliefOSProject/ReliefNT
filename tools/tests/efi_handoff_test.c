/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Exercise the production boot coordinator and firmware implementations. */
#include "../../firmware/efi_fs.c"
#include "../../kernel/init/kernel.c"

static unsigned map_calls, exit_calls;
static unsigned acpi_capture_calls;
static unsigned identity_capture_calls;
static bool reject_first_key;
static bool reject_every_exit;
static efi_handle_t expected_handle = (void *)(uintptr_t)0x1234;
static unsigned display_mapped, display_claimed, display_fail;

/** @brief Model current coverage of the two retained display extents.
 * @param physical Physical base. @param bytes Exact fixture length.
 * @return True when that extent is available in the current root.
 */
bool paging_kernel_direct_map_range(uint64_t physical, uint64_t bytes)
{
    unsigned bit = physical == 0x70000000 ? 1 : physical == 0x30000000 ? 2 : 0;
    assert(bit && bytes == (bit == 1 ? 65536u : 4096u));
    return (display_mapped & bit) != 0;
}

/** @brief Record an attempted display UC claim in the fixture root.
 * @param physical Physical base. @param bytes Exact fixture length.
 * @return True unless this fixture claim is configured to fail.
 */
bool paging_mmio_uncached(uint64_t physical, uint64_t bytes)
{
    unsigned bit = physical == 0x70000000 ? 1 : physical == 0x30000000 ? 2 : 0;
    assert(bit && bytes == (bit == 1 ? 65536u : 4096u));
    display_claimed |= bit;
    return !(display_fail & bit);
}

/** @brief Save an inert interrupt sentinel for the hosted firmware fixture.
 * @return A fake saved interrupt state.
 */
uint64_t kernel_irq_save(void) { return 0; }

/** @brief Restore the hosted interrupt sentinel.
 * @param flags Saved fixture state returned by kernel_irq_save().
 */
void kernel_irq_restore(uint64_t flags) { (void)flags; }

/** @brief Discard early-console output during a host test.
 * @param level Log severity.
 * @param format Borrowed format string and variadic arguments.
 */
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }

/** @brief Verify ACPI is captured against a map before the first EBS attempt.
 * @param boot Borrowed active-EFI handoff and fresh snapshot.
 */
void power_acpi_capture_efi_rsdp(struct boot_info *boot)
{
    assert(boot->efi_boot_services_active && boot->efi_mmap_entry_count && map_calls && !exit_calls);
    ++acpi_capture_calls;
}
/** @brief Verify SMBIOS capture occurs with a map and before firmware exit.
 * @param boot Borrowed active-EFI handoff with a fresh snapshot.
 */
void platform_identity_init(const struct boot_info *boot)
{
    assert(boot->efi_boot_services_active && boot->efi_mmap_entry_count && !exit_calls);
    ++identity_capture_calls;
}

/** @brief Supply a single complete descriptor and a changing firmware map key.
 * @param bytes Input capacity and returned byte count.
 * @param map Writable caller-owned descriptor storage.
 * @param key Output map key that must be passed directly to ExitBootServices.
 * @param stride Output EFI descriptor size.
 * @param version Output descriptor version.
 * @return EFI success after writing one descriptor.
 */
static efi_status_t __attribute__((ms_abi)) fixture_get_map(
    uint64_t *bytes, void *map, uint64_t *key, uint64_t *stride, uint32_t *version)
{
    assert(*bytes >= sizeof(struct efi_memory_descriptor));
    struct efi_memory_descriptor descriptor = {
        .type = 7, .physical_start = 0x200000, .number_of_pages = 128,
    };
    memcpy(map, &descriptor, sizeof(descriptor));
    *bytes = sizeof(descriptor);
    *key = 100 + ++map_calls;
    *stride = sizeof(descriptor);
    *version = 1;
    return EFI_SUCCESS;
}

/** @brief Verify the exact firmware handle and current map key on exit.
 * @param handle Image handle supplied by Multiboot tag 20.
 * @param key Most recently returned firmware memory-map key.
 * @return EFI_INVALID_PARAMETER for a requested retry, otherwise EFI success.
 */
static efi_status_t __attribute__((ms_abi)) fixture_exit(efi_handle_t handle, uint64_t key)
{
    assert(handle == expected_handle && key == 100 + map_calls);
    ++exit_calls;
    if (reject_every_exit || (reject_first_key && exit_calls == 1))
        return 0x8000000000000002ULL;
    return EFI_SUCCESS;
}

/** @brief Exercise active firmware exit, retries and retained display UC claims.
 * @return Zero if the handoff and pre/post-publication claim contracts hold.
 */
int main(void)
{
    struct efi_boot_services services = {0};
    struct efi_system_table system_table = {0};
    struct multiboot2_mmap_entry old_map = {0x201000, 0x1000, 2, 0};
    struct boot_info boot = {
        .efi_system_table = (uintptr_t)&system_table,
        .efi_boot_services_active = true,
        .efi_identity_amd64 = true,
        .efi_image_handle = (uintptr_t)expected_handle,
        .mmap_addr = (uintptr_t)&old_map,
        .mmap_entry_size = sizeof(old_map),
        .mmap_entry_count = 1,
    };
    services.get_memory_map = fixture_get_map;
    services.exit_boot_services = (void *)fixture_exit;
    system_table.boot_services = &services;
    reject_first_key = true;

    assert(boot_capture_efi_acpi(&boot) == 0 && acpi_capture_calls == 1 &&
           identity_capture_calls == 1 && map_calls == 1);
    assert(boot_capture_efi_memory_map(&boot) == 0);
    assert(exit_calls == 2 && map_calls == 3);
    assert(boot.mmap_addr == (uintptr_t)&old_map && boot.mmap_entry_count == 1);
    assert(boot.efi_mmap_addr && boot.efi_mmap_entry_size == sizeof(struct efi_memory_descriptor));
    assert(boot.efi_mmap_entry_count == 1);
    assert(!boot.efi_boot_services_active && boot.efi_identity_amd64);

    /* Already-ended handoffs must never call potentially invalid firmware. */
    unsigned previous_maps = map_calls, previous_exits = exit_calls;
    assert(boot_capture_efi_memory_map(&boot) == 0);
    assert(map_calls == previous_maps && exit_calls == previous_exits);

    /* An unidentifiable image cannot transfer firmware-owned RAM. */
    boot.efi_boot_services_active = true;
    boot.efi_image_handle = 0;
    assert(boot_capture_efi_memory_map(&boot) < 0);
    assert(map_calls == previous_maps && exit_calls == previous_exits);

    /* Repeated invalid keys fail closed and preserve the preexisting maps. */
    boot.efi_image_handle = (uintptr_t)expected_handle;
    uint64_t prior_efi_map = boot.efi_mmap_addr;
    uint32_t prior_efi_count = boot.efi_mmap_entry_count;
    reject_first_key = false;
    reject_every_exit = true;
    assert(boot_capture_efi_memory_map(&boot) < 0);
    assert(map_calls == previous_maps + 4 && exit_calls == previous_exits + 4);
    assert(boot.efi_boot_services_active && boot.efi_mmap_addr == prior_efi_map &&
           boot.efi_mmap_entry_count == prior_efi_count &&
           boot.mmap_addr == (uintptr_t)&old_map && boot.mmap_entry_count == 1);
    struct framebuffer display = {.available = true,
        .reservation_start = 0x70000000, .reservation_bytes = 65536,
        .auxiliary_reservation_start = 0x30000000, .auxiliary_reservation_bytes = 4096};
    display_mapped = 1;
    assert(boot_claim_framebuffer(&display, false) && display_claimed == 1);
    assert(!boot_claim_framebuffer(&display, true));
    display_mapped = 3; display_claimed = 0;
    assert(boot_claim_framebuffer(&display, true) && display_claimed == 3);
    display_fail = 2; display_claimed = 0;
    assert(!boot_claim_framebuffer(&display, true) && display_claimed == 3);
    puts("PASS EFI handoff retries the map key and preserves prior reservations");
    return 0;
}
