/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_X86_FIRMWARE_INTERNAL_H
#define RELIEFNT_X86_FIRMWARE_INTERNAL_H
#include <reliefnt/multiboot2.h>
#include <reliefnt/paging.h>

/** @brief Prove an EFI amd64 identity-mapped firmware-data span.
 * @param boot Borrowed parsed handoff and valid EFI descriptor buffer.
 * @param start Physical first byte. @param bytes Nonzero complete extent.
 * @return True only for CPU/design-representable bytes entirely covered by
 * readable EFI descriptors and the entry-established current-root guarantee.
 * The BIOS entry proves only [0,4 GiB); active EFI amd64 proves EFI spaces.
 * BSP before the kernel replaces the loader root; our own EBS preserves proof.
 * The descriptor array itself is owned by the validated boot handoff or kernel.
 */
static inline bool firmware_identity_range(const struct boot_info *boot,
                                            uint64_t start, uint64_t bytes)
{
    if (!boot || !boot->efi_mmap_addr || !boot->efi_mmap_entry_count || !bytes ||
        boot->efi_mmap_entry_size < sizeof(struct efi_memory_descriptor) ||
        start >= RELIEFNT_KERNEL_DIRECT_MAP_SIZE ||
        bytes > RELIEFNT_KERNEL_DIRECT_MAP_SIZE - start ||
        !paging_phys_addressable(start, bytes)) return false;
    if (!boot->efi_identity_amd64 &&
        (!boot->bios_identity_4g || start >= 0x100000000ULL ||
         bytes > 0x100000000ULL - start)) return false;
    uint64_t count_bytes = (uint64_t)boot->efi_mmap_entry_count * boot->efi_mmap_entry_size;
    if (count_bytes > UINT64_MAX - boot->efi_mmap_addr) return false;
    uint64_t cursor = start, end = start + bytes;
    while (cursor < end) {
        uint64_t covered = cursor;
        for (uint32_t i = 0; i < boot->efi_mmap_entry_count; ++i) {
            struct efi_memory_descriptor d;
            const void *entry = (const void *)(uintptr_t)(boot->efi_mmap_addr +
                (uint64_t)i * boot->efi_mmap_entry_size);
            __builtin_memcpy(&d, entry, sizeof(d));
            if (!((d.type >= 1 && d.type <= 7) || d.type == 9 || d.type == 10 || d.type == 14) ||
                d.number_of_pages > UINT64_MAX / 4096) continue;
            uint64_t length = d.number_of_pages * 4096;
            if (length > UINT64_MAX - d.physical_start) continue;
            uint64_t last = d.physical_start + length;
            if (d.physical_start <= cursor && last > covered) covered = last;
        }
        if (covered == cursor) return false;
        cursor = covered < end ? covered : end;
    }
    return true;
}
#endif
