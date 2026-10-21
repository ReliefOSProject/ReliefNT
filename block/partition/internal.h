#ifndef RELIEFNT_BLOCK_PARTITION_STORAGE_DISK_C_INTERNAL_H
#define RELIEFNT_BLOCK_PARTITION_STORAGE_DISK_C_INTERNAL_H

#include "../core/storage_internal.h"
struct disk_gpt_table {
    struct gpt_header primary;
    struct gpt_header backup;
    uint32_t table_bytes;
    uint32_t table_sectors;
};






struct disk_block_partition_range {
    uint64_t first_lba;
    uint64_t sector_count;
    uint8_t present;
    uint8_t unique_guid[16];
};

struct disk_block_partition_cache {
    uint64_t disk_sectors;
    struct disk_block_partition_range entries[RELIEFOS_DISK_MAX_PARTITIONS];
    int32_t status;
    uint8_t valid;
};





/* Disk IDs are assigned during every storage probe.  Never carry partition
 * ranges (or a previous probe's error) across that reassignment. */




void disk_block_cache_invalidate(uint32_t disk_id);

void storage_disk_block_cache_reset(void);

void disk_block_cache_invalidate_disk(const struct install_disk_state *disk);

/**
 * @brief Checks whether a GPT GUID is all zeroes.
 * @param guid GPT type or unique GUID.
 * @return Nonzero when the GUID denotes an unused entry.
 */
int disk_gpt_guid_empty(const uint8_t guid[16]);

/**
 * @brief Tests whether a GPT entry is allocated.
 * @param entry GPT partition entry to inspect.
 * @return Nonzero if the entry has a type GUID.
 */
int disk_gpt_entry_used(const struct gpt_entry *entry);

/**
 * @brief Prepares a block disk for a synchronous disk-management operation.
 * @param disk_id Detected AHCI, IDE/PATA, or NVMe disk index.
 * @param out_disk Receives the selected block-disk state.
 * @param out_sector_count Receives the total disk sectors.
 * @return Zero on success or a negative errno-style storage error.
 */
int disk_manage_prepare(uint32_t disk_id, struct install_disk_state **out_disk,
                               uint64_t *out_sector_count);

/**
 * @brief Reads and validates both GPT headers and the primary partition table.
 * @param disk Prepared block-disk state.
 * @param sector_count Total number of disk sectors.
 * @param out_table Receives validated GPT header metadata.
 * @return Zero on success or a negative errno-style storage error.
 */
int disk_gpt_load(struct install_disk_state *disk, uint64_t sector_count,
                         struct disk_gpt_table *out_table);

int storage_disk_block_info(uint32_t disk_id, int32_t partition_index,
                            uint64_t *out_first_lba, uint64_t *out_sector_count);

int storage_sync_disk(uint32_t disk_id);

/** @brief Return a validated GPT partition UUID; caller holds the storage lock.
 * @param disk_id Published disk index.
 * @param partition_index Zero-based GPT slot.
 * @param uuid Output buffer of at least 37 bytes, including NUL.
 * @return Zero or negative errno; no UUID is fabricated for non-GPT disks.
 */
int storage_disk_partition_uuid(uint32_t disk_id, uint32_t partition_index, char uuid[37]);

int storage_disk_block_read(uint32_t disk_id, int32_t partition_index,
                            uint64_t offset, void *buffer, uint32_t length,
                            uint32_t *out_read);

int storage_disk_block_write(uint32_t disk_id, int32_t partition_index,
                             uint64_t offset, const void *buffer, uint32_t length,
                             uint32_t *out_written);

int storage_disk_block_reread(uint32_t disk_id);

/**
 * @brief Validates all populated GPT entries and rejects overlapping ranges.
 * @param table Loaded GPT table metadata.
 * @return Zero on success or a negative errno-style storage error.
 */
int disk_gpt_entries_valid(const struct disk_gpt_table *table);

/**
 * @brief Detects the filesystem superblock at a GPT partition start.
 * @param disk Prepared block-disk state.
 * @param entry Validated GPT partition entry.
 * @param out_filesystem Receives a RELIEFOS_DISK_FILESYSTEM value.
 * @return Zero when probing completed, including an unknown filesystem; a
 *         negative storage error when the partition could not be read.
 */
int disk_partition_filesystem(struct install_disk_state *disk,
                                     const struct gpt_entry *entry,
                                     uint32_t *out_filesystem);

/**
 * @brief Checks whether an operation may mutate a disk-management partition.
 * @param disk Block-disk state that owns the partition.
 * @return Zero if the disk is not a startup or mounted installer target.
 */
int disk_partition_mutable(const struct install_disk_state *disk);

/**
 * @brief Checks whether a runtime data mount belongs to a block disk.
 * @param disk_id Block-disk identifier to test.
 * @return Nonzero when a dynamic data mount references @p disk_id.
 */
int storage_disk_has_data_mount(uint32_t disk_id);

/**
 * @brief Checks whether installer-reserved mount slots are occupied.
 * @return Nonzero when /target or /target/boot cannot be reused by the installer.
 */
int storage_installer_mounts_busy(void);

/**
 * @brief Tests a user-visible filesystem selection accepted by the formatter.
 * @param filesystem RELIEFOS_DISK_FILESYSTEM value.
 * @return Nonzero when the selection is FAT32, exFAT, ext4, or legacy ext2.
 */
int disk_filesystem_format_supported(uint32_t filesystem);

/**
 * @brief Lists GPT partitions for an administrator-facing disk-management client.
 * @param disk_id Detected block-disk identifier.
 * @param partitions Caller buffer receiving public partition records.
 * @param capacity Number of records available in @p partitions.
 * @param out_count Receives the total number of used GPT entries.
 * @return Zero on success or a negative errno-style storage error.
 */
int storage_disk_list_partitions(uint32_t disk_id,
                                 struct reliefos_disk_partition *partitions,
                                 uint32_t capacity, uint32_t *out_count);

/**
 * @brief Initializes a protective MBR and empty primary/backup GPT pair.
 *
 * This is intentionally separate from the normal GPT editor.  The editor
 * requires a valid table, while this operation is the explicit destructive
 * entry point used by the installer ISO's gptinit utility.
 */
int storage_disk_initialize_gpt(const struct reliefos_disk_gpt_initialize *request);

/**
 * @brief Formats an existing unprotected GPT partition as FAT32, exFAT, ext4,
 *        or legacy ext2.
 * @param request Validated disk-management format request.
 * @return Zero on success or a negative errno-style storage error.
 */
int storage_disk_format_partition(const struct reliefos_disk_partition_format *request);

/**
 * @brief Deletes an unprotected GPT partition entry while leaving its data intact.
 * @param request Validated disk-management delete request.
 * @return Zero on success or a negative errno-style storage error.
 */
int storage_disk_delete_partition(const struct reliefos_disk_partition_delete *request);

/** Edit standard GPT type and/or printable partition name in both GPT copies. */
int storage_disk_edit_partition(const struct reliefos_disk_partition_edit *request);

/**
 * @brief Creates and formats a data partition in the first suitable free GPT range.
 * @param request Validated disk-management create request.
 * @return Zero on success or a negative errno-style storage error.
 */
int storage_disk_create_partition(const struct reliefos_disk_partition_create *request);

int storage_disk_partition_volume_id(uint32_t disk_id, uint32_t partition_index,
                                     uint32_t *out_volume_id);

int storage_disk_partition_mount_path(uint32_t disk_id, uint32_t partition_index,
                                      char *out_path, uint32_t capacity);

/** Mount a GPT partition through the standard block-device and mount ABI. */
int storage_mount_block_partition(uint32_t disk_id, uint32_t partition_index,
                                  const char *target, const char *filesystem_name,
                                  uint64_t flags, uint32_t *out_volume_id);

/** Legacy ABI adapter retained until all callers have moved to mount(2). */
int storage_disk_mount_partition(struct reliefos_disk_partition_mount *request);

int storage_mount_path_volume_id(const char *target, uint32_t *out_volume_id);

int storage_unmount_path(const char *target, uint32_t *out_volume_id);

/**
 * @brief Unmounts a runtime data partition after the kernel has checked task usage.
 * @param request Disk and GPT-entry selector.
 * @return Zero on success or a negative errno-style storage error.
 */
int storage_disk_unmount_partition(const struct reliefos_disk_partition_unmount *request);

int storage_install_list_disks(struct reliefos_install_disk *disks,
                               uint32_t capacity, uint32_t *out_count);

int storage_install_format_target(uint32_t disk_id);

/**
 * @brief Preserves the historical installer formatting ABI.
 * @param disk_id Installer disk identifier.
 * @return Zero after formatting the ESP plus exFAT root, or a negative error.
 */
int storage_install_format_esp(uint32_t disk_id);

int storage_install_mount_target(uint32_t disk_id);

#endif
