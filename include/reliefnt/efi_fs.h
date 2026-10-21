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
 */
void efi_fs_init(uint64_t system_table_addr);
/**
 * @brief Return true once the EFI filesystem is usable.
 */
bool efi_fs_ready(void);
/**
 * @brief Capture the live EFI memory map into buffer while boot services are active.
 * Reports descriptor geometry through out_entry_size/out_entry_count.
 */
int efi_boot_memory_map(uint64_t system_table_addr, void *buffer,
                        uint64_t buffer_bytes, uint64_t *out_entry_size,
                        uint32_t *out_entry_count);
/**
 * @brief Load the file at path into memory, returning a pointer in out_data and its size in out_len.
 */
int efi_fs_read_file(const char *path, const void **out_data, size_t *out_len);
/**
 * @brief List up to capacity directory entries from path into entries, setting out_count.
 */
int efi_fs_list_dir(const char *path, struct reliefos_dir_entry *entries,
                    uint32_t capacity, uint32_t *out_count);

#endif
