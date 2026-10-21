/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_STORAGE_CORE_API_H
#define RELIEFNT_STORAGE_CORE_API_H

#include <stddef.h>
#include <stdint.h>
#include <reliefos/fs_abi.h>

#include "storage_types.h"

/**
 * @file storage_core_api.h
 * @brief Private block-core and storage-state interface shared by modules.
 *
 * Filesystem families and transports use this contract for volume selection,
 * bounded sector I/O, cache invalidation, installer access and shared path
 * helpers. Hardware register layouts and filesystem-specific APIs are declared
 * in their owning headers.
 */

struct ide_device_info;

bool storage_async_can_yield(void);
bool storage_mount_path_matches(const char *path, const char *mount_path);
const char *storage_transport_name(uint8_t transport);
int gpt_find_esp(void);
int storage_devlink_next(uint64_t *cursor, char uuid[37], char target[48]);
int storage_devlink_target(const char *path, char target[48]);
int install_clear_sectors(struct install_disk_state *disk, uint64_t lba,
                          uint64_t sector_count);
int install_format_exfat(struct install_disk_state *disk, uint64_t start_lba,
                         uint64_t sector_count);
int install_format_ext2(struct install_disk_state *disk, uint64_t start_lba,
                        uint64_t sector_count);
int install_format_ext4(struct install_disk_state *disk, uint64_t start_lba,
                        uint64_t sector_count);
int install_format_fat32(struct install_disk_state *disk, uint64_t start_lba,
                         uint64_t sector_count);
int install_read_sectors(struct install_disk_state *disk, uint64_t lba,
                         uint32_t sector_count, void *buffer);
int install_write_gpt(struct install_disk_state *disk, uint64_t sector_count,
                      uint64_t *out_esp_lba, uint64_t *out_esp_sectors,
                      uint64_t *out_root_lba, uint64_t *out_root_sectors);
int install_write_sectors(struct install_disk_state *disk, uint64_t lba,
                          uint32_t sector_count, const void *buffer);
/**
 * @brief Queries capacity and identity for a probed installer disk.
 * @param disk Transport-specific disk state to identify.
 * @param out_sectors Receives the disk capacity in sectors.
 * @return Zero on success, or a negative storage error.
 */
int storage_install_identify(struct install_disk_state *disk,
                             uint64_t *out_sectors);
int storage_acquire_task_io(void);
int storage_backend_path(const char *path, char *backend_path,
                         uint32_t backend_capacity);
int storage_dir_index_lookup(uint32_t directory_cluster, const char *name,
                             struct storage_node *out);
int storage_flush_volume(const struct storage_volume *volume);
int storage_guid_valid(const uint8_t guid[16]);
int storage_is_acl_metadata_name(const char *name);
int storage_memcmp(const void *a, const void *b, size_t len);
int storage_parent_path(const char *path, char *parent, uint32_t parent_cap,
                        char *name, uint32_t name_cap);
int storage_path_cache_lookup(const char *path, struct storage_node *out);
int storage_prepare_install_disk(struct install_disk_state *disk);
int storage_read_install_disk(const struct install_disk_state *disk,
                              uint64_t lba, uint32_t sector_count,
                              void *buffer);
int storage_read_iso_blocks(uint64_t lba, uint32_t block_count, void *buffer);
int storage_read_sectors(uint64_t lba, uint32_t sector_count, void *buffer);
int storage_route_path(const char *path, struct storage_volume **out_volume,
                       char *backend_path, uint32_t backend_capacity);
int storage_select_node_volume(const struct storage_node *node,
                               struct storage_volume **old_volume);
int storage_select_volume(uint32_t volume_id);
int storage_text_eq(const char *a, const char *b);
int storage_text_eq_ci(const char *a, const char *b);
int storage_write_install_disk(const struct install_disk_state *disk,
                               uint64_t lba, uint32_t sector_count,
                               const void *buffer);
int storage_write_sectors(uint64_t lba, uint32_t sector_count,
                          const void *buffer);
size_t storage_strlen(const char *s);
uint32_t min_u32(uint32_t a, uint32_t b);
uint32_t storage_crc32(const void *data, uint32_t len);
uint64_t cluster_to_lba(uint32_t cluster);
uint64_t fat_sector_for_cluster(uint32_t cluster);
uint64_t min_u64(uint64_t a, uint64_t b);
uint64_t storage_volume_device_number(const struct storage_volume *volume);
void storage_begin_mutation(void);
void storage_cache_invalidate(void);
void storage_copy_text(char *dst, uint32_t cap, const char *src);
void storage_dir_index_store(uint32_t directory_cluster, const char *name,
                             const struct storage_node *node);
void storage_disk_ide_device(const struct install_disk_state *disk,
                             struct ide_device_info *device);
void storage_format_guid(const uint8_t guid[16], char *out, uint32_t cap);
void storage_make_guid(uint8_t guid[16], uint64_t tag,
                       const struct install_disk_state *disk,
                       uint64_t sector_count);
void storage_partition_guid_text(const uint8_t guid[16], char uuid[37]);
void storage_path_cache_store(const char *path, const struct storage_node *node);
void storage_restore_volume(struct storage_volume *old_volume);
void storage_sector_cache_invalidate(void);
void storage_volume_from_install_disk(struct storage_volume *volume,
                                      const struct install_disk_state *disk);
void storage_volume_ide_device(const struct storage_volume *volume,
                               struct ide_device_info *device);

/** @brief Freestanding zero operation supplied by the block core. */
void storage_memzero(void *dst, size_t len);

/** @brief Freestanding copy operation supplied by the block core. */
void storage_memcpy(void *dst, const void *src, size_t len);

/** @brief Reads a bounded sector range from the selected volume. */
int storage_read_device(const struct storage_volume *volume, uint64_t lba,
                        uint32_t sector_count, void *buffer);

/** @brief Writes a bounded sector range to the selected volume. */
int storage_write_device(const struct storage_volume *volume, uint64_t lba,
                         uint32_t sector_count, const void *buffer);

#endif /* RELIEFNT_STORAGE_CORE_API_H */
