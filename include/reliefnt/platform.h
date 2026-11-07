/*
 * ReliefOS platform interface: declares firmware and machine identification.
 * Provides platform setup and system-information hooks to kernel services.
 */
#ifndef RELIEFNT_PLATFORM_H
#define RELIEFNT_PLATFORM_H

#include <reliefnt/multiboot2.h>

/**
 * @brief Capture SMBIOS inventory for DMI sysfs using proven EFI handoff mappings.
 * @param boot Borrowed parsed handoff and EFI memory map, or NULL.
 * BSP before the kernel root switch; unavailable spans remain absent.
 */
void platform_identity_init(const struct boot_info *boot);
#define PLATFORM_DMI_FIELDS 15u
struct platform_dmi_info {
    /* bios vendor/version/date; system vendor/name/version/serial/uuid/sku/family;
     * board vendor/name/version/serial; chassis vendor. Empty means unavailable. */
    char values[PLATFORM_DMI_FIELDS][128];
};
/** @brief Return cached, validated SMBIOS strings; unavailable fields are empty. */
const struct platform_dmi_info *platform_dmi(void);

#endif
