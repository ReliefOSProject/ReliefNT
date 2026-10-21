/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_STORAGE_PARTITION_H
#define RELIEFNT_STORAGE_PARTITION_H

#include <stdint.h>

#include "../core/storage_types.h"

/**
 * @file storage_partition.h
 * @brief Private GPT layouts and block-partition interface.
 */

#define GPT_ENTRY_COUNT 128u
#define GPT_ENTRY_SIZE 128u
struct __attribute__((packed)) gpt_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t partition_entries_lba;
    uint32_t partition_entry_count;
    uint32_t partition_entry_size;
    uint32_t partition_entries_crc32;
};

struct __attribute__((packed)) gpt_entry {
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name[36];
};

/* GPT partition-type GUIDs in little-endian on-disk byte order; defined once
 * by block/core/storage_state_data.c. */
extern const uint8_t esp_guid[16];
extern const uint8_t linux_filesystem_guid[16];
/* Microsoft Basic Data GUID. */
extern const uint8_t basic_data_guid[16];
/* Public block-device spelling used by devfs, mount tables, and GPT links.
 * The storage core continues to address disks by their probe-time disk_id. */
/**
 * @brief Formats the public /dev name for one disk or partition.
 *
 * A partition index of -1 identifies the whole disk; nonnegative values are
 * zero-based partition indexes.
 * @return Zero on success, or a negative storage error.
 */
int storage_disk_device_name(uint32_t disk_id, int32_t partition_index,
                             char *out, uint32_t capacity);
/** @brief Resolves a public block-device name to disk and partition indexes. */
int storage_parse_block_name(const char *name, uint32_t *disk_id,
                             int32_t *partition_index);

/** @brief Invalidates the cached public block-device namespace. */
void storage_disk_block_cache_reset(void);
/** @brief Returns the LBA range represented by a disk or partition. */
int storage_disk_block_info(uint32_t disk_id, int32_t partition_index,
                            uint64_t *out_first_lba, uint64_t *out_sector_count);


#endif /* RELIEFNT_STORAGE_PARTITION_H */
