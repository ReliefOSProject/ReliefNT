/*
 * ReliefOS power interface: declares shutdown and reboot operations.
 * Allows platform code and system calls to request controlled power changes.
 */
#ifndef RELIEFNT_POWER_H
#define RELIEFNT_POWER_H

#include <reliefnt/types.h>

#define ACPI_PM_TIMER_HZ 3579545u

struct boot_info;

/**
 * @brief Locate ACPI tables and record S5 state using proven boot mappings.
 * @param boot Borrowed handoff; any EFI RSDP must already have been captured.
 */
void power_init(const struct boot_info *boot);
/** @brief Capture an EFI configuration-table RSDP before ExitBootServices.
 * @param boot Writable active-EFI handoff with a fresh memory-map snapshot.
 * Retains a valid Multiboot RSDP, otherwise records a proven EFI RSDP physical
 * address separately from the Multiboot-owned block. Requires descriptor and
 * current-root reachability proof; performs no firmware call or allocation.
 */
void power_acpi_capture_efi_rsdp(struct boot_info *boot);
/** @brief Find a validated ACPI table through current reachable mappings.
 * @param signature Four-byte signature. @return Borrowed table alias or NULL.
 */
const void *power_acpi_find_table(const char signature[4]);
/**
 * @brief Capture readable ACPI table spans while firmware identity mappings live.
 * @return 0 or negative errno if the inventory is exhausted or a span is invalid.
 * BSP only after power_init and before bootstrap allocation; no allocation.
 */
int power_acpi_capture_resources(void);
/** @brief Count captured ACPI physical spans.
 * @return Number of stable spans until the next capture; no ownership transfer.
 */
uint32_t power_acpi_resource_count(void);
/** @brief Borrow one captured ACPI physical span.
 * @param index Zero-based resource index. @param start Required physical output.
 * @param bytes Required byte-length output.
 * @return True for an existing span, false for invalid arguments or index.
 */
bool power_acpi_resource(uint32_t index, uint64_t *start, uint64_t *bytes);
/** Read the optional ACPI PM timer through its validated System-I/O GAS or
 * legacy port. Returns false when unavailable/unsupported; count is masked
 * to the FADT's 24/32-bit width. The read changes no timer or power state. */
bool power_acpi_pm_timer_read(uint32_t *count, uint32_t *mask);
void power_reboot(void) __attribute__((noreturn));
void power_shutdown(void) __attribute__((noreturn));

#endif
