/* Shared storage state, defined exactly once.  block/core/storage_types.h
 * declares these extern so every storage module can be an independent
 * translation unit instead of sharing one copy per file.  This file carries
 * no functions: host fixtures link it to get the shared state without also
 * pulling in the block-core primitives they replace with fakes. */
#include "storage_types.h"

struct storage_volume g_volumes[STORAGE_MAX_VOLUMES];
struct storage_volume *g_active_volume = &g_volumes[0];

bool storage_io_async_context;
bool storage_io_write_started;
uint32_t storage_task_io_owner;

struct install_disk_state g_install_disks[STORAGE_MAX_INSTALL_DISKS];
uint32_t g_install_disk_count;
uint8_t g_devfs_enabled = 1;
uint8_t g_installer_root_active;

struct kernel_spinlock storage_transport_lock = KERNEL_SPINLOCK_INIT;

/* Shared AHCI command RAM; storage_fat32.c issues its own commands through
 * the same tables. */
uint8_t ahci_received_fis[256] __attribute__((aligned(256)));
uint8_t ahci_cmd_table_buf[256] __attribute__((aligned(128)));
struct ahci_cmd_header ahci_cmd_headers[32] __attribute__((aligned(1024)));

uint8_t storage_scratch[STORAGE_SCRATCH_SECTORS * SECTOR_SIZE]
    __attribute__((aligned(4096)));
uint8_t storage_cluster_buf[64 * SECTOR_SIZE] __attribute__((aligned(4096)));

uint8_t storage_fat_cache_data[STORAGE_FAT_CACHE_SECTORS * SECTOR_SIZE]
    __attribute__((aligned(4096)));
uint8_t storage_read_cache_data[STORAGE_READAHEAD_SECTORS * SECTOR_SIZE]
    __attribute__((aligned(4096)));
uint8_t storage_dir_lookup_cache_data[64 * SECTOR_SIZE]
    __attribute__((aligned(4096)));
uint32_t storage_old_chain[FAT32_MAX_FILE_CLUSTERS];
uint32_t storage_new_chain[FAT32_MAX_FILE_CLUSTERS];

struct storage_sector_cache storage_fat_cache;
struct storage_sector_cache storage_read_cache;
struct storage_cluster_cache storage_dir_lookup_cache;
struct storage_path_cache_entry storage_path_cache[STORAGE_PATH_CACHE_ENTRIES];
uint32_t storage_path_cache_next;
struct storage_dir_index_entry storage_dir_index[STORAGE_DIR_INDEX_ENTRIES];
uint32_t storage_dir_index_next;
struct storage_write_chain_cache storage_write_chain_cache;
struct storage_dir_iter_cache storage_dir_iter_cache;

const uint8_t esp_guid[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};

const uint8_t linux_filesystem_guid[16] = {
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4,
};

/* Microsoft Basic Data GUID in GPT's little-endian on-disk byte order. */
const uint8_t basic_data_guid[16] = {
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
    0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7,
};
