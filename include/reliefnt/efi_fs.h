/*
 * ReliefOS EFI firmware interface: filesystem helpers plus the boot-services
 * memory-map capture used during early boot.
 */
#ifndef RELIEFNT_EFI_FS_H
#define RELIEFNT_EFI_FS_H

#include <reliefos/fs_abi.h>
#include <reliefnt/types.h>

/**
 * @brief Bring up the EFI firmware filesystem using the system table at system_table_addr.
 * @param system_table_addr Firmware-owned system table address, readable while
 *        the firmware page tables and boot services remain active.
 * @return None. BSP boot context before the kernel replaces firmware mappings.
 */
void efi_fs_init(uint64_t system_table_addr);
/**
 * @brief Close the EFI root volume and seal filesystem entry points.
 * @return None. Must be called before replacing the firmware page tables.
 *         The caller may still capture the final EFI memory map through
 *         efi_boot_memory_map() using its explicit system-table address.
 */
void efi_fs_shutdown(void);
/**
 * @brief Return true once the EFI filesystem is usable.
 */
bool efi_fs_ready(void);
/**
 * @brief Capture the live EFI memory map into buffer while boot services are active.
 * Reports descriptor geometry through out_entry_size/out_entry_count.
 * @param system_table_addr Firmware-owned system table address, valid before
 *        replacement of the firmware page tables.
 * @param buffer Writable memory-map destination.
 * @param buffer_bytes Destination capacity.
 * @param out_entry_size Output descriptor stride.
 * @param out_entry_count Output descriptor count.
 * @return 0 on success or a negative errno-style value. May be called after
 *         efi_fs_shutdown() to include memory released when the root volume closed.
 */
int efi_boot_memory_map(uint64_t system_table_addr, void *buffer,
                        uint64_t buffer_bytes, uint64_t *out_entry_size,
                        uint32_t *out_entry_count);
/**
 * @brief Read a bounded file into caller-owned storage without allocating pages.
 * @param path Absolute EFI path, including a leading slash.
 * @param buffer Writable destination of capacity bytes, valid across firmware calls.
 * @param capacity Destination capacity; files larger than it fail without reading.
 * @param out_len Required output set to bytes read only on success.
 * @return 0 on a complete read, or a negative errno-style value. Requires a
 *         live EFI filesystem and firmware services; use only before replacing
 *         the firmware page tables.
 */
int efi_fs_read_file_into(const char *path, void *buffer, size_t capacity,
                          size_t *out_len);
/**
 * @brief List up to capacity directory entries from path into entries, setting out_count.
 */
int efi_fs_list_dir(const char *path, struct reliefos_dir_entry *entries,
                    uint32_t capacity, uint32_t *out_count);

#endif
