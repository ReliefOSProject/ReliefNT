#include "internal.h"



int fat32_mount(void)
{
    struct fat32_bpb *bpb = (struct fat32_bpb *)(void *)storage_scratch;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint32_t total_sectors;
    uint32_t data_sectors;
    int ret = storage_read_sectors(g_storage.esp_start_lba, 1, storage_scratch);
    if (ret < 0) {
        return storage_read_failure(ret);
    }
    if (bpb->bytes_per_sector != SECTOR_SIZE || bpb->sectors_per_cluster == 0 ||
        bpb->fat_count == 0 || bpb->fat_size32 == 0 || bpb->root_cluster < 2) {
        return -2;
    }
    fs_info_sector = bpb->fs_info;
    backup_boot_sector = bpb->backup_boot_sector;
    g_storage.bytes_per_sector = bpb->bytes_per_sector;
    g_storage.sectors_per_cluster = bpb->sectors_per_cluster;
    g_storage.cluster_bytes = bpb->bytes_per_sector * bpb->sectors_per_cluster;
    g_storage.fat_count = bpb->fat_count;
    g_storage.fat_start_sector = bpb->reserved_sector_count;
    g_storage.fat_sector_count = bpb->fat_size32;
    g_storage.data_start_sector = bpb->reserved_sector_count + (uint32_t)bpb->fat_count * bpb->fat_size32;
    g_storage.root_cluster = bpb->root_cluster;
    g_storage.next_free_cluster = bpb->root_cluster + 1u;
    g_storage.fat_fs_info_sector = 0;
    g_storage.fat_backup_boot_sector = 0;
    g_storage.fat_free_clusters = 0;
    g_storage.fat_fsinfo_valid = 0;
    total_sectors = bpb->total_sectors32 ? bpb->total_sectors32 : bpb->total_sectors16;
    g_storage.total_sectors = total_sectors;
    if (total_sectors <= g_storage.data_start_sector ||
        (g_storage.esp_sector_count && total_sectors > g_storage.esp_sector_count)) {
        return -2;
    }
    data_sectors = total_sectors - g_storage.data_start_sector;
    g_storage.data_cluster_count = data_sectors / g_storage.sectors_per_cluster;
    if (g_storage.cluster_bytes > sizeof(storage_cluster_buf)) {
        console_error("[reliefnt] storage FAT32 cluster too large=%u\n", g_storage.cluster_bytes);
        return -2;
    }
    if (fs_info_sector > 0 && fs_info_sector < bpb->reserved_sector_count &&
        storage_read_sectors(g_storage.esp_start_lba + fs_info_sector, 1u,
                             storage_scratch) == 0 &&
        storage_get_u32(storage_scratch) == 0x41615252u &&
        storage_get_u32(storage_scratch + 484u) == 0x61417272u &&
        storage_get_u32(storage_scratch + 508u) == 0xaa550000u) {
        uint32_t free_clusters = storage_get_u32(storage_scratch + 488u);
        uint32_t next_free = storage_get_u32(storage_scratch + 492u);
        if (free_clusters <= g_storage.data_cluster_count) {
            g_storage.fat_fs_info_sector = fs_info_sector;
            g_storage.fat_backup_boot_sector = backup_boot_sector;
            g_storage.fat_free_clusters = free_clusters;
            g_storage.fat_fsinfo_valid = 1;
            if (next_free >= 2u && next_free <= g_storage.data_cluster_count + 1u) {
                g_storage.next_free_cluster = next_free;
            }
        }
    }
    /* Installer targets clone the exFAT/ext2 root volume before mounting its
     * ESP as the target ESP. Always set the filesystem kind here so later path
     * lookups do not accidentally dispatch FAT32 paths through another backend. */
    g_storage.filesystem = STORAGE_FILESYSTEM_FAT32;
    return 0;
}
