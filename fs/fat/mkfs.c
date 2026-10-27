/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include "internal.h"

/** @brief Choose a FAT32 cluster size preserving the ESP minimum cluster count.
 * @param total_sectors Validated extent size in 512-byte sectors.
 * @return Sectors per cluster. Pure layout choice, no allocation or I/O.
 */
static uint32_t install_choose_fat32_spc(uint64_t total_sectors)
{
    uint64_t bytes = total_sectors * SECTOR_SIZE;
    /* The ESP is intentionally small but must still meet the FAT32 minimum
     * cluster-count rule. One KiB clusters keep a 128 MiB ESP standards-sized;
     * the normal high-volume data path uses the separate exFAT root. */
    if (bytes <= 128ULL * 1024ULL * 1024ULL) return 2u;
    if (bytes <= 512ULL * 1024ULL * 1024ULL) return 4u;
    return 8u;
}

/** @brief Write native FAT32 metadata into an authorized unmounted extent.
 * @param disk Borrowed managed target disk. @param start_lba First validated sector.
 * @param sector_count Validated extent length in 512-byte sectors.
 * @return Zero or layout/I/O errno. Execution ownership and admitted filesystem
 * provider pin required across all writes; no graph/module lock or queued work.
 */
int install_format_fat32(struct install_disk_state *disk, uint64_t start_lba,
                                uint64_t sector_count)
{
    const uint32_t reserved = 32;
    const uint32_t fat_count = 2;
    uint32_t total_sectors;
    uint32_t sectors_per_cluster;
    uint32_t fat_size = 1;
    uint32_t data_start;
    uint32_t data_sectors;
    uint32_t clusters = 0;
    if (!disk || sector_count > 0xffffffffULL || sector_count < 65536ULL) {
        return -28;
    }
    total_sectors = (uint32_t)sector_count;
    sectors_per_cluster = install_choose_fat32_spc(sector_count);
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t next_fat_size;
        if (total_sectors <= reserved + fat_count * fat_size) {
            return -28;
        }
        data_sectors = total_sectors - reserved - fat_count * fat_size;
        clusters = data_sectors / sectors_per_cluster;
        next_fat_size = ((clusters + 2u) * 4u + SECTOR_SIZE - 1u) / SECTOR_SIZE;
        if (next_fat_size == fat_size) {
            break;
        }
        fat_size = next_fat_size;
    }
    data_start = reserved + fat_count * fat_size;
    if (clusters < 2 || data_start >= total_sectors) {
        return -28;
    }

    int ret = install_clear_sectors(disk, start_lba, (uint64_t)data_start + sectors_per_cluster);
    if (ret < 0) {
        return ret;
    }

    storage_memzero(storage_scratch, SECTOR_SIZE);
    struct fat32_bpb *bpb = (struct fat32_bpb *)(void *)storage_scratch;
    bpb->jump[0] = 0xeb;
    bpb->jump[1] = 0x58;
    bpb->jump[2] = 0x90;
    storage_memcpy(bpb->oem, "RELIEFOS", 8);
    bpb->bytes_per_sector = SECTOR_SIZE;
    bpb->sectors_per_cluster = (uint8_t)sectors_per_cluster;
    bpb->reserved_sector_count = reserved;
    bpb->fat_count = fat_count;
    bpb->root_entry_count = 0;
    bpb->total_sectors16 = 0;
    bpb->media = 0xf8;
    bpb->fat_size16 = 0;
    bpb->sectors_per_track = 63;
    bpb->head_count = 255;
    bpb->hidden_sectors = (uint32_t)start_lba;
    bpb->total_sectors32 = total_sectors;
    bpb->fat_size32 = fat_size;
    bpb->root_cluster = 2;
    bpb->fs_info = 1;
    bpb->backup_boot_sector = 6;
    bpb->drive_number = 0x80;
    bpb->boot_signature = 0x29;
    bpb->volume_id = 0x4c454f34u;
    storage_memcpy(bpb->volume_label, "RELIEFOS   ", 11);
    storage_memcpy(bpb->fs_type, "FAT32   ", 8);
    storage_scratch[510] = 0x55;
    storage_scratch[511] = 0xaa;
    ret = install_write_sectors(disk, start_lba, 1, storage_scratch);
    if (ret < 0) {
        return ret;
    }
    ret = install_write_sectors(disk, start_lba + 6, 1, storage_scratch);
    if (ret < 0) {
        return ret;
    }

    storage_memzero(storage_scratch, SECTOR_SIZE);
    storage_put_u32(storage_scratch + 0, 0x41615252u);
    storage_put_u32(storage_scratch + 484, 0x61417272u);
    storage_put_u32(storage_scratch + 488, clusters - 1u);
    storage_put_u32(storage_scratch + 492, 3u);
    storage_put_u32(storage_scratch + 508, 0xaa550000u);
    ret = install_write_sectors(disk, start_lba + 1, 1, storage_scratch);
    if (ret < 0) {
        return ret;
    }
    ret = install_write_sectors(disk, start_lba + 7, 1, storage_scratch);
    if (ret < 0) {
        return ret;
    }

    for (uint32_t fat = 0; fat < fat_count; ++fat) {
        uint64_t fat_lba = start_lba + reserved + (uint64_t)fat * fat_size;
        storage_memzero(storage_scratch, SECTOR_SIZE);
        storage_put_u32(storage_scratch + 0, 0x0ffffff8u);
        storage_put_u32(storage_scratch + 4, 0xffffffffu);
        storage_put_u32(storage_scratch + 8, FAT32_EOC);
        ret = install_write_sectors(disk, fat_lba, 1, storage_scratch);
        if (ret < 0) {
            return ret;
        }
    }

    /* FAT32 stores the volume label both in the BPB and as a root-directory
     * entry.  Writing both prevents standard repair tools from discarding the
     * label during their consistency pass. */
    storage_memzero(storage_scratch, SECTOR_SIZE);
    {
        struct fat32_dirent *label = (struct fat32_dirent *)(void *)storage_scratch;
        storage_memcpy(label->name, "RELIEFOS   ", sizeof(label->name));
        label->attr = FAT32_ATTR_VOLUME;
    }
    return install_write_sectors(disk, start_lba + data_start, 1u, storage_scratch);
}
