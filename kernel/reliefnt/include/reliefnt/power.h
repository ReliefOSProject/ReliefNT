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
 * @brief Locate ACPI tables and record the S5 shutdown control state for later use.
 */
void power_init(const struct boot_info *boot);
/* Return a validated ACPI table discovered during power_init, or NULL. */
const void *power_acpi_find_table(const char signature[4]);
/** Read the optional ACPI PM timer through its validated System-I/O GAS or
 * legacy port. Returns false when unavailable/unsupported; count is masked
 * to the FADT's 24/32-bit width. The read changes no timer or power state. */
bool power_acpi_pm_timer_read(uint32_t *count, uint32_t *mask);
void power_reboot(void) __attribute__((noreturn));
void power_shutdown(void) __attribute__((noreturn));

#endif
