#include "internal.h"

/** @brief Locate one FAT32 entry inside the selected native sector.
 * @param cluster Unsigned native cluster number; modulo arithmetic preserves the
 * on-disk entry offset. Selected parsed volume has nonzero bytes_per_sector.
 * @return Byte offset within that sector; caller holds storage execution ownership.
 */
static uint32_t fat_offset_for_cluster(uint32_t cluster)
{
    return (cluster * 4u) % g_storage.bytes_per_sector;
}

static int fat32_write_fsinfo(void);


int fat32_read_fat_entry(uint32_t cluster, uint32_t *out_next)
{
    uint32_t sector = fat_sector_for_cluster(cluster);
    uint32_t offset = fat_offset_for_cluster(cluster);
    uint64_t fat_start = g_storage.esp_start_lba + g_storage.fat_start_sector;
    uint64_t fat_end = fat_start + g_storage.fat_sector_count;
    uint64_t cache_start;
    uint32_t cache_sectors;
    if (!out_next || sector < fat_start || sector >= fat_end) {
        return -22;
    }
    if (!storage_fat_cache.valid || storage_fat_cache.volume != g_active_volume ||
        sector < storage_fat_cache.first_lba ||
        sector >= storage_fat_cache.first_lba + storage_fat_cache.sector_count) {
        cache_start = fat_start +
                      ((sector - fat_start) / STORAGE_FAT_CACHE_SECTORS) *
                          STORAGE_FAT_CACHE_SECTORS;
        cache_sectors = (uint32_t)min_u64(STORAGE_FAT_CACHE_SECTORS,
                                          fat_end - cache_start);
        storage_fat_cache.valid = 0;
        int ret = storage_read_sectors(cache_start, cache_sectors, storage_fat_cache_data);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        storage_fat_cache.volume = g_active_volume;
        storage_fat_cache.first_lba = cache_start;
        storage_fat_cache.sector_count = cache_sectors;
        storage_fat_cache.valid = 1;
    }
    offset += (uint32_t)(sector - storage_fat_cache.first_lba) * SECTOR_SIZE;
    if (offset + sizeof(uint32_t) > storage_fat_cache.sector_count * SECTOR_SIZE) {
        console_error("[storage] fat entry range cluster=%u sector=%llu offset=%u cache_sectors=%u\n",
                       cluster, (unsigned long long)sector, offset,
                       storage_fat_cache.sector_count);
        return -5;
    }
    uint32_t value = *(const uint32_t *)(const void *)(storage_fat_cache_data + offset);
    *out_next = value & 0x0fffffffu;
    return 0;
}

int storage_read_cache_lookup(uint32_t cluster, uint32_t *out_offset,
                                     uint32_t *out_clusters)
{
    uint64_t lba;
    uint32_t sector_offset;
    uint32_t sectors_left;
    if (!storage_read_cache.valid || storage_read_cache.volume != g_active_volume ||
        !out_offset || !out_clusters || cluster < 2) {
        return 0;
    }
    lba = cluster_to_lba(cluster);
    if (lba < storage_read_cache.first_lba ||
        lba >= storage_read_cache.first_lba + storage_read_cache.sector_count) {
        return 0;
    }
    sector_offset = (uint32_t)(lba - storage_read_cache.first_lba);
    sectors_left = storage_read_cache.sector_count - sector_offset;
    if (sector_offset % g_storage.sectors_per_cluster ||
        sectors_left < g_storage.sectors_per_cluster) {
        return 0;
    }
    *out_offset = sector_offset * SECTOR_SIZE;
    *out_clusters = sectors_left / g_storage.sectors_per_cluster;
    return 1;
}

int storage_read_contiguous_clusters(uint32_t first_cluster,
                                            uint32_t max_clusters,
                                            uint32_t *out_clusters,
                                            uint32_t *out_next)
{
    uint32_t cluster = first_cluster;
    uint32_t count = 1;
    uint32_t next = FAT32_EOC;
    if (!out_clusters || !out_next || first_cluster < 2 || max_clusters == 0) {
        return -22;
    }
    for (;;) {
        int ret = fat32_read_fat_entry(cluster, &next);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        if (count >= max_clusters || next >= FAT32_EOC || next != cluster + 1u) {
            *out_clusters = count;
            *out_next = next;
            return 0;
        }
        cluster = next;
        ++count;
    }
}

int storage_read_cache_fill(uint32_t first_cluster, uint32_t clusters)
{
    uint32_t sectors;
    uint64_t lba;
    if (first_cluster < 2 || clusters == 0 ||
        clusters > STORAGE_READAHEAD_SECTORS / g_storage.sectors_per_cluster) {
        return -22;
    }
    sectors = clusters * g_storage.sectors_per_cluster;
    lba = cluster_to_lba(first_cluster);
    storage_read_cache.valid = 0;
    int ret = storage_read_sectors(lba, sectors, storage_read_cache_data);
    if (ret < 0) {
        return storage_read_failure(ret);
    }
    storage_read_cache.volume = g_active_volume;
    storage_read_cache.first_lba = lba;
    storage_read_cache.sector_count = sectors;
    storage_read_cache.valid = 1;
    return 0;
}

int fat32_read_lookup_cluster(uint32_t cluster, void *buffer)
{
    if (cluster < 2 || !buffer || g_storage.cluster_bytes == 0 ||
        g_storage.cluster_bytes > sizeof(storage_dir_lookup_cache_data)) {
        return -22;
    }
    if (storage_dir_lookup_cache.valid &&
        storage_dir_lookup_cache.volume == g_active_volume &&
        storage_dir_lookup_cache.cluster == cluster) {
        storage_memcpy(buffer, storage_dir_lookup_cache_data, g_storage.cluster_bytes);
        return 0;
    }
    int ret = storage_read_sectors(cluster_to_lba(cluster), g_storage.sectors_per_cluster,
                                   storage_dir_lookup_cache_data);
    if (ret < 0) {
        storage_dir_lookup_cache.valid = 0;
        return storage_read_failure(ret);
    }
    storage_dir_lookup_cache.volume = g_active_volume;
    storage_dir_lookup_cache.cluster = cluster;
    storage_dir_lookup_cache.valid = 1;
    storage_memcpy(buffer, storage_dir_lookup_cache_data, g_storage.cluster_bytes);
    return 0;
}

int fat32_write_fat_entry(uint32_t cluster, uint32_t value)
{
    uint32_t sector_offset = (cluster * 4u) / g_storage.bytes_per_sector;
    uint32_t offset = fat_offset_for_cluster(cluster);
    uint32_t masked = value & 0x0fffffffu;
    uint64_t first_fat_lba = g_storage.esp_start_lba + g_storage.fat_start_sector +
                             sector_offset;
    /* The sector read below supplies data for a persistent FAT update. Once
     * this point is reached the operation must finish synchronously: replaying
     * a partially updated FAT chain after EAGAIN is not safe. */
    storage_begin_mutation();
    for (uint32_t fat = 0; fat < g_storage.fat_count; ++fat) {
        uint64_t lba = g_storage.esp_start_lba + g_storage.fat_start_sector +
                       (uint64_t)fat * g_storage.fat_sector_count + sector_offset;
        int ret = storage_read_sectors(lba, 1, storage_scratch);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        *(uint32_t *)(void *)(storage_scratch + offset) =
            (*(uint32_t *)(void *)(storage_scratch + offset) & 0xf0000000u) | masked;
        ret = storage_write_sectors(lba, 1, storage_scratch);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        /* Keep the cache associated with the primary FAT, and only publish
         * the new contents after the media write succeeded.  Publishing the
         * pre-write buffer makes the allocator see an entry as free again;
         * publishing the backup FAT would associate primary-FAT metadata with
         * the wrong sector when the two copies differ. */
        if (fat == 0u) {
            storage_memcpy(storage_fat_cache_data, storage_scratch, SECTOR_SIZE);
        }
    }
    /* storage_write_sectors() invalidates the read cache. Re-seed the FAT
     * cache with the just-written sector so freeing/appending a long chain
     * does not issue a disk read for every adjacent FAT entry. */
    if (g_storage.fat_count == 0u) {
        return -22;
    }
    storage_fat_cache.volume = g_active_volume;
    storage_fat_cache.first_lba = first_fat_lba;
    storage_fat_cache.sector_count = 1;
    storage_fat_cache.valid = 1;
    return 0;
}

static int fat32_write_fsinfo(void)
{
    uint64_t primary_lba;
    if (!g_storage.fat_fsinfo_valid) {
        return 0;
    }
    storage_memzero(storage_scratch, SECTOR_SIZE);
    storage_put_u32(storage_scratch, 0x41615252u);
    storage_put_u32(storage_scratch + 484u, 0x61417272u);
    storage_put_u32(storage_scratch + 488u, g_storage.fat_free_clusters);
    storage_put_u32(storage_scratch + 492u, g_storage.next_free_cluster);
    storage_put_u32(storage_scratch + 508u, 0xaa550000u);
    primary_lba = g_storage.esp_start_lba + g_storage.fat_fs_info_sector;
    int ret = storage_write_sectors(primary_lba, 1u, storage_scratch);
    if (ret < 0) {
        return ret;
    }
    if (g_storage.fat_backup_boot_sector &&
        (uint32_t)g_storage.fat_backup_boot_sector + g_storage.fat_fs_info_sector <
            g_storage.fat_start_sector &&
        (ret = storage_write_sectors(g_storage.esp_start_lba + g_storage.fat_backup_boot_sector +
                                     g_storage.fat_fs_info_sector,
                                     1u, storage_scratch)) < 0) {
        return ret;
    }
    return 0;
}

int fat32_note_allocated(void)
{
    if (g_storage.fat_fsinfo_valid && g_storage.fat_free_clusters > 0) {
        --g_storage.fat_free_clusters;
    }
    return fat32_write_fsinfo();
}

int fat32_note_freed(void)
{
    if (g_storage.fat_fsinfo_valid &&
        g_storage.fat_free_clusters < g_storage.data_cluster_count) {
        ++g_storage.fat_free_clusters;
    }
    return fat32_write_fsinfo();
}

int fat32_read_cluster(uint32_t cluster, void *buffer)
{
    uint32_t cache_offset;
    uint32_t cache_clusters;
    if (cluster < 2) {
        return -2;
    }
    if (!buffer) {
        return -22;
    }
    if (storage_read_cache_lookup(cluster, &cache_offset, &cache_clusters)) {
        storage_memcpy(buffer, storage_read_cache_data + cache_offset,
                       g_storage.cluster_bytes);
        return 0;
    }
    return storage_read_sectors(cluster_to_lba(cluster), g_storage.sectors_per_cluster, buffer);
}

int fat32_write_cluster(uint32_t cluster, const void *buffer)
{
    if (cluster < 2 || !buffer) {
        return -2;
    }
    return storage_write_sectors(cluster_to_lba(cluster), g_storage.sectors_per_cluster, buffer);
}
