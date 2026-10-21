/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_STORAGE_TYPES_H
#define RELIEFNT_STORAGE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <reliefnt/lock.h>
#include <reliefnt/storage.h>
#include <reliefnt/tmpfs.h>

#include "storage_hardware.h"
#include "../../fs/ext4/storage_ext4_types.h"

/** @brief Common sector size for all supported block transports. */
#define SECTOR_SIZE 512u
#define STORAGE_SCRATCH_SECTORS 8u
#define STORAGE_FAT_CACHE_SECTORS 8u
/* Keep the FAT cache fill to a 4 KiB DMA transfer.  This is the proven
 * compatibility limit for resource loads on the current virtual AHCI path;
 * the short completion poll below removes the common full-tick delay without
 * changing the transfer shape used by TTF and BMP loading. */
#define STORAGE_READAHEAD_SECTORS 8u
/* Keep the chain scratch space bounded even for legacy 512-byte-cluster
 * FAT32 volumes. New VMDK images and installer-created volumes use larger
 * clusters, but this remains the safe compatibility limit. */
#define FAT32_MAX_FILE_CLUSTERS 65536u
#define ISO9660_BLOCK_SIZE 2048u
#define STORAGE_MAX_VOLUMES 10u
#define STORAGE_VOLUME_ROOT 0u
#define STORAGE_VOLUME_TARGET_ROOT 1u
#define STORAGE_VOLUME_BOOT 2u
#define STORAGE_VOLUME_DYNAMIC_FIRST 3u
#define STORAGE_MAX_INSTALL_DISKS RELIEFOS_INSTALL_MAX_DISKS
#define STORAGE_PATH_CACHE_ENTRIES 128u
#define STORAGE_DIR_INDEX_ENTRIES 512u
#define INSTALL_ESP_FIRST_LBA 2048ULL
#define INSTALL_ESP_SECTORS 262144ULL
#define INSTALL_EXT2_BLOCK_SIZE 4096u
#define INSTALL_EXT2_BLOCKS_PER_GROUP 32768u
#define INSTALL_EXT2_INODES_PER_GROUP 8192u
enum storage_volume_kind {
    STORAGE_VOLUME_NONE = 0,
    STORAGE_VOLUME_AHCI = 1,
    STORAGE_VOLUME_RAM = 2,
    STORAGE_VOLUME_IDE = 3,
    STORAGE_VOLUME_NVME = 4,
};

enum storage_transport {
    STORAGE_TRANSPORT_AHCI = 1,
    STORAGE_TRANSPORT_IDE_PIO = 2,
    STORAGE_TRANSPORT_NVME = 3,
};

enum storage_filesystem_kind {
    STORAGE_FILESYSTEM_NONE = 0,
    STORAGE_FILESYSTEM_FAT32 = 1,
    STORAGE_FILESYSTEM_ISO9660 = 2,
    STORAGE_FILESYSTEM_EXT2 = 3,
    STORAGE_FILESYSTEM_EXFAT = 4,
    STORAGE_FILESYSTEM_TMPFS = 5,
    STORAGE_FILESYSTEM_EXT4 = 6,
};


struct nvme_controller;

/**
 * @brief Shared volume state used by block routing and filesystem backends.
 *
 * The transport identity and filesystem-specific geometry live together
 * because path routing selects a volume before dispatching to a backend.
 */
struct storage_volume {
    bool ready;
    struct tmpfs_super *tmpfs;
    char tmpfs_source[RELIEFOS_FS_PATH_LEN];
    uint64_t mount_flags;
    uint8_t volume_id;
    uint8_t kind;
    uint8_t filesystem;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t port;
    uint8_t transport;
    uint8_t ide_channel;
    uint8_t ide_drive;
    uint8_t ide_atapi;
    uint16_t ide_command_base;
    uint16_t ide_control_base;
    uint8_t ide_lba48;
    char device_model[41];
    struct ahci_hba_mem *abar;
    struct ahci_hba_port *hba_port;
    struct nvme_controller *nvme;
    uint16_t nvme_controller_index;
    uint32_t nvme_nsid;
    uint8_t *ram_base;
    uint64_t ram_bytes;
    uint64_t esp_start_lba;
    uint64_t esp_sector_count;
    /* Ext-family partition range (ext2/3/4 share one parser and one GPT
     * partition); the legacy ext2_* spellings live on as access macros. */
    uint64_t ext_start_lba;
    uint64_t ext_sector_count;
    uint64_t exfat_start_lba;
    uint64_t exfat_sector_count;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_bytes;
    uint32_t fat_count;
    uint32_t fat_start_sector;
    uint32_t fat_sector_count;
    uint32_t data_start_sector;
    uint32_t total_sectors;
    uint32_t data_cluster_count;
    uint32_t root_cluster;
    uint16_t fat_fs_info_sector;
    uint16_t fat_backup_boot_sector;
    uint32_t fat_free_clusters;
    uint8_t fat_fsinfo_valid;
    uint32_t ext2_block_size;
    uint32_t ext2_blocks_count;
    uint32_t ext2_blocks_per_group;
    uint32_t ext2_inodes_per_group;
    uint32_t ext2_inode_size;
    uint32_t ext2_first_data_block;
    uint32_t ext2_group_count;
    uint32_t ext2_feature_incompat;
    uint32_t ext2_next_block;
    uint32_t ext2_next_inode;
    /* ext4 backend state (geometry parsed at mount, bounded caches held by
     * fs/ext4/cache.c) plus mount bookkeeping: mount_generation is
     * stamped into cache entries and read_only_reason is one of the
     * STORAGE_EXT4_READ_ONLY_* values (0 = writable). */
    struct storage_ext4_state ext4;
    uint32_t mount_generation;
    uint32_t read_only_reason;
    uint32_t exfat_fat_offset;
    uint32_t exfat_fat_length;
    uint32_t exfat_cluster_heap_offset;
    uint32_t exfat_cluster_count;
    uint32_t exfat_root_cluster;
    uint32_t exfat_bitmap_cluster;
    uint32_t exfat_upcase_cluster;
    uint64_t exfat_bitmap_length;
    uint64_t exfat_upcase_length;
    uint32_t exfat_upcase_checksum;
    uint32_t exfat_next_free_cluster;
    uint8_t exfat_sectors_per_cluster_shift;
    uint8_t exfat_bitmap_nofat;
    uint8_t exfat_upcase_nofat;
    uint32_t iso_block_size;
    uint32_t iso_root_extent;
    uint32_t iso_root_size;
    uint64_t iso_sector_count;
    uint32_t next_free_cluster;
    uint8_t gpt_disk_guid[16];
    uint8_t esp_unique_guid[16];
    uint8_t ext2_unique_guid[16];
    uint8_t exfat_unique_guid[16];
    uint64_t esp_device;
    uint64_t ext_device;
    uint64_t exfat_device;
    uint8_t has_gpt_identity;
    uint32_t source_disk_id;
    uint32_t source_partition_index;
    uint8_t data_partition_mount;
    char mount_path[RELIEFOS_FS_PATH_LEN];
};

/* Spec section 8: the ext2 partition range was renamed to the ext-family
 * spelling.  Keep the old field names working as access macros so existing
 * callers and host-test fixtures compile unchanged. */
#define ext2_start_lba ext_start_lba
#define ext2_sector_count ext_sector_count


struct install_disk_state {
    bool present;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t port;
    uint8_t transport;
    uint8_t ide_channel;
    uint8_t ide_drive;
    uint8_t ide_atapi;
    uint8_t ide_lba48;
    uint16_t ide_command_base;
    uint16_t ide_control_base;
    char device_model[41];
    uint8_t boot_root;
    uint8_t target_mounted;
    struct ahci_hba_mem *abar;
    struct ahci_hba_port *hba_port;
    struct nvme_controller *nvme;
    uint16_t nvme_controller_index;
    uint32_t nvme_nsid;
    uint64_t sector_count;
};


/* State definitions are owned by block/core/storage_state_data.c. */
extern struct storage_volume g_volumes[STORAGE_MAX_VOLUMES];
extern struct storage_volume *g_active_volume;
#define g_storage (*g_active_volume)

/*
 * A filesystem syscall is allowed to park while it is only observing disk
 * state.  Once it has begun a mutation, however, replaying the syscall after
 * an asynchronous yield could allocate a second FAT chain or apply part of a
 * directory update twice.  Keep that small critical tail synchronous; normal
 * application loads, path walks and compiler header reads remain preemptible.
 */
extern bool storage_io_async_context;
extern bool storage_io_write_started;
/* The AHCI command table, DMA staging areas and FAT32 caches are shared
 * globally.  A syscall that yielded for disk completion therefore owns the
 * storage state until its instruction is retried and completes. */
extern uint32_t storage_task_io_owner;

extern struct install_disk_state g_install_disks[STORAGE_MAX_INSTALL_DISKS];
extern uint32_t g_install_disk_count;
extern uint8_t g_devfs_enabled;
extern uint8_t g_installer_root_active;

/* AHCI uses shared command RAM and IDE PIO owns channel registers while an
 * operation is in flight.  NVMe has a queue per controller, but it shares a
 * staging page with the transport.  Serialize these synchronous operations
 * without disabling interrupts, so SMP callers cannot corrupt a command. */
extern struct kernel_spinlock storage_transport_lock;

extern uint8_t ahci_received_fis[256] __attribute__((aligned(256)));
extern uint8_t ahci_cmd_table_buf[256] __attribute__((aligned(128)));
extern struct ahci_cmd_header ahci_cmd_headers[32] __attribute__((aligned(1024)));
extern uint8_t storage_scratch[STORAGE_SCRATCH_SECTORS * SECTOR_SIZE] __attribute__((aligned(4096)));
extern uint8_t storage_cluster_buf[64 * SECTOR_SIZE] __attribute__((aligned(4096)));

/* The public /dev block namespace is rebuilt whenever storage is probed. */
extern uint8_t storage_fat_cache_data[STORAGE_FAT_CACHE_SECTORS * SECTOR_SIZE]
    __attribute__((aligned(4096)));
extern uint8_t storage_read_cache_data[STORAGE_READAHEAD_SECTORS * SECTOR_SIZE]
    __attribute__((aligned(4096)));
/* Directory lookups are much more frequent than file-data reads during a
 * compiler build. Keep a separate cluster cache so opening the next header
 * does not evict the directory that is being scanned from the data cache. */
extern uint8_t storage_dir_lookup_cache_data[64 * SECTOR_SIZE]
    __attribute__((aligned(4096)));
extern uint32_t storage_old_chain[FAT32_MAX_FILE_CLUSTERS];
extern uint32_t storage_new_chain[FAT32_MAX_FILE_CLUSTERS];
struct storage_sector_cache {
    struct storage_volume *volume;
    uint64_t first_lba;
    uint32_t sector_count;
    uint8_t valid;
};

struct storage_cluster_cache {
    struct storage_volume *volume;
    uint32_t cluster;
    uint8_t valid;
};

struct storage_path_cache_entry {
    struct storage_volume *volume;
    struct storage_node node;
    char path[RELIEFOS_FS_PATH_LEN];
    uint8_t valid;
};

struct storage_dir_index_entry {
    struct storage_volume *volume;
    uint32_t directory_cluster;
    struct storage_node node;
    char name[RELIEFOS_FS_NAME_LEN];
    uint8_t valid;
};

extern struct storage_sector_cache storage_fat_cache;
extern struct storage_sector_cache storage_read_cache;
extern struct storage_cluster_cache storage_dir_lookup_cache;
extern struct storage_path_cache_entry storage_path_cache[STORAGE_PATH_CACHE_ENTRIES];
extern uint32_t storage_path_cache_next;
extern struct storage_dir_index_entry storage_dir_index[STORAGE_DIR_INDEX_ENTRIES];
extern uint32_t storage_dir_index_next;

/* Keep the last file chain tail available for repeated append writes. The
 * scheduler limits each syscall to a small payload, so without this hint a
 * large file would be walked from its first cluster on every syscall. */
struct storage_write_chain_cache {
    struct storage_volume *volume;
    uint32_t first_cluster;
    uint32_t size;
    uint32_t count;
    uint32_t tail;
    char path[RELIEFOS_FS_PATH_LEN];
    uint8_t valid;
};

extern struct storage_write_chain_cache storage_write_chain_cache;

/* Directory reads are issued one entry at a time. Cache the current cluster
 * and decoded entry ordinal so sequential readdir never restarts at root. */
struct storage_dir_iter_cache {
    struct storage_volume *volume;
    uint32_t first_cluster;
    uint32_t cluster;
    uint32_t entry_offset;
    uint64_t next_index;
    uint8_t valid;
};

extern struct storage_dir_iter_cache storage_dir_iter_cache;
static inline uint32_t storage_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void storage_put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

#endif /* RELIEFNT_STORAGE_TYPES_H */
