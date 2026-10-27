#include <reliefnt/export.h>
#include "../core/storage_internal.h"

uint32_t storage_crc32(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (uint32_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/**
 * @brief Write a raw installer extent in bounded native transport batches.
 * @param disk Required present inventory record, stable for the complete transaction.
 * @param lba First physical 512-byte sector.
 * @param sector_count Complete sector count; zero performs no device write.
 * @param buffer Required readable kernel extent of sector_count times 512 bytes.
 * @return Zero after every batch succeeds, otherwise the first validation or transport errno; earlier batches may already be written.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call. Invalidates shared caches and latches mutation before I/O. The provider token is held for each batch; caller owns outer disk/format exclusion.
 */
int install_write_sectors(struct install_disk_state *disk, uint64_t lba,
                                 uint32_t sector_count, const void *buffer)
{
    const uint8_t *src = (const uint8_t *)buffer;
    if (!disk || !disk->present || !buffer) {
        return -22;
    }
    /* Installer formatting is also a filesystem mutation: do not make its
     * multi-sector GPT/FAT update replayable after an async yield. */
    storage_begin_mutation();
    storage_cache_invalidate();
    while (sector_count) {
        uint32_t chunk = min_u32(sector_count, STORAGE_WRITE_MAX_SECTORS);
        int ret = storage_write_install_disk(disk, lba, chunk, src);
        if (ret < 0) {
            return ret;
        }
        lba += chunk;
        sector_count -= chunk;
        src += chunk * SECTOR_SIZE;
    }
    return 0;
}

/**
 * @brief Reads sectors directly from a managed block disk.
 * @param disk Managed AHCI, IDE/PATA, or NVMe disk state.
 * @param lba First sector to read.
 * @param sector_count Number of sectors to read.
 * @param buffer Destination buffer.
 * @return Zero on success or a negative storage error.
 */
int install_read_sectors(struct install_disk_state *disk, uint64_t lba,
                                uint32_t sector_count, void *buffer)
{
    uint8_t *dst = (uint8_t *)buffer;
    if (!disk || !disk->present || !buffer) {
        return -22;
    }
    while (sector_count) {
        uint32_t chunk = min_u32(sector_count, STORAGE_WRITE_MAX_SECTORS);
        int ret = storage_read_install_disk(disk, lba, chunk, dst);
        if (ret < 0) {
            return ret;
        }
        lba += chunk;
        sector_count -= chunk;
        dst += chunk * SECTOR_SIZE;
    }
    return 0;
}

/**
 * @brief Zero a raw installer extent through the common write path.
 * @param disk Borrowed present disk inventory record, stable for the transaction.
 * @param lba First physical 512-byte sector.
 * @param sector_count Number of sectors; zero is a no-op.
 * @return Zero after all batches are cleared or the first write errno; partial completion is possible.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call. Uses the shared scratch buffer, advances mutation state and invalidates caches before native writes.
 */
int install_clear_sectors(struct install_disk_state *disk, uint64_t lba,
                                 uint64_t sector_count)
{
    storage_memzero(storage_scratch, sizeof(storage_scratch));
    while (sector_count) {
        uint32_t chunk = (uint32_t)min_u64(sector_count, STORAGE_SCRATCH_SECTORS);
        int ret = install_write_sectors(disk, lba, chunk, storage_scratch);
        if (ret < 0) {
            return ret;
        }
        lba += chunk;
        sector_count -= chunk;
    }
    return 0;
}

static void install_utf16_name(uint16_t dst[36], const char *name)
{
    uint32_t i = 0;
    for (; i < 36 && name && name[i]; ++i) {
        dst[i] = (uint16_t)(uint8_t)name[i];
    }
    for (; i < 36; ++i) {
        dst[i] = 0;
    }
}

int install_write_gpt(struct install_disk_state *disk, uint64_t sector_count,
                             uint64_t *out_esp_lba, uint64_t *out_esp_sectors,
                             uint64_t *out_root_lba, uint64_t *out_root_sectors)
{
    uint8_t disk_guid[16];
    uint8_t esp_part_guid[16];
    uint8_t root_part_guid[16];
    uint64_t last_lba;
    uint64_t backup_entries_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint32_t table_bytes = GPT_ENTRY_COUNT * GPT_ENTRY_SIZE;
    uint32_t table_sectors = table_bytes / SECTOR_SIZE;
    uint32_t table_crc;
    uint64_t esp_last;
    uint64_t root_first;
    int ret;
    if (!disk || !out_esp_lba || !out_esp_sectors || !out_root_lba || !out_root_sectors ||
        sector_count < 655360ULL) {
        return -28;
    }
    storage_make_guid(disk_guid, 0x4c3447494449534bULL, disk, sector_count);
    storage_make_guid(esp_part_guid, 0x4c34474944504152ULL, disk, sector_count);
    storage_make_guid(root_part_guid, 0x4c34474944525431ULL, disk, sector_count);
    last_lba = sector_count - 1u;
    backup_entries_lba = last_lba - table_sectors;
    first_usable = INSTALL_ESP_FIRST_LBA;
    last_usable = backup_entries_lba - 1u;
    esp_last = first_usable + INSTALL_ESP_SECTORS - 1u;
    root_first = (esp_last + 1u + 2047u) & ~2047ULL;
    if (esp_last >= last_usable || root_first >= last_usable ||
        last_usable - root_first + 1u < 262144ULL) {
        return -28;
    }

    ret = install_clear_sectors(disk, 0, first_usable);
    if (ret < 0) {
        return ret;
    }
    ret = install_clear_sectors(disk, backup_entries_lba, table_sectors + 1u);
    if (ret < 0) {
        return ret;
    }

    storage_memzero(storage_scratch, SECTOR_SIZE);
    storage_scratch[446 + 4] = 0xee;
    storage_put_u32(storage_scratch + 446 + 8, 1);
    storage_put_u32(storage_scratch + 446 + 12,
                    sector_count - 1u > 0xffffffffULL ? 0xffffffffu : (uint32_t)(sector_count - 1u));
    storage_scratch[510] = 0x55;
    storage_scratch[511] = 0xaa;
    ret = install_write_sectors(disk, 0, 1, storage_scratch);
    if (ret < 0) {
        return ret;
    }

    storage_memzero(storage_cluster_buf, table_bytes);
    struct gpt_entry *entry = (struct gpt_entry *)(void *)storage_cluster_buf;
    storage_memcpy(entry->type_guid, esp_guid, sizeof(entry->type_guid));
    storage_memcpy(entry->unique_guid, esp_part_guid, sizeof(entry->unique_guid));
    entry->first_lba = first_usable;
    entry->last_lba = esp_last;
    entry->attrs = 0;
    install_utf16_name(entry->name, "RELIEFOS_ESP");
    ++entry;
    storage_memcpy(entry->type_guid, linux_filesystem_guid, sizeof(entry->type_guid));
    storage_memcpy(entry->unique_guid, root_part_guid, sizeof(entry->unique_guid));
    entry->first_lba = root_first;
    entry->last_lba = last_usable;
    entry->attrs = 0;
    install_utf16_name(entry->name, "RELIEFOS_ROOT");
    table_crc = storage_crc32(storage_cluster_buf, table_bytes);
    ret = install_write_sectors(disk, 2, table_sectors, storage_cluster_buf);
    if (ret < 0) {
        return ret;
    }
    ret = install_write_sectors(disk, backup_entries_lba, table_sectors, storage_cluster_buf);
    if (ret < 0) {
        return ret;
    }

    storage_memzero(storage_scratch, SECTOR_SIZE);
    struct gpt_header *hdr = (struct gpt_header *)(void *)storage_scratch;
    hdr->signature = 0x5452415020494645ULL;
    hdr->revision = 0x00010000u;
    hdr->header_size = sizeof(struct gpt_header);
    hdr->current_lba = 1;
    hdr->backup_lba = last_lba;
    hdr->first_usable_lba = first_usable;
    hdr->last_usable_lba = last_usable;
    storage_memcpy(hdr->disk_guid, disk_guid, sizeof(hdr->disk_guid));
    hdr->partition_entries_lba = 2;
    hdr->partition_entry_count = GPT_ENTRY_COUNT;
    hdr->partition_entry_size = GPT_ENTRY_SIZE;
    hdr->partition_entries_crc32 = table_crc;
    hdr->header_crc32 = 0;
    hdr->header_crc32 = storage_crc32(hdr, hdr->header_size);
    ret = install_write_sectors(disk, 1, 1, storage_scratch);
    if (ret < 0) {
        return ret;
    }

    hdr->header_crc32 = 0;
    hdr->current_lba = last_lba;
    hdr->backup_lba = 1;
    hdr->partition_entries_lba = backup_entries_lba;
    hdr->header_crc32 = storage_crc32(hdr, hdr->header_size);
    ret = install_write_sectors(disk, last_lba, 1, storage_scratch);
    if (ret < 0) {
        return ret;
    }

    *out_esp_lba = first_usable;
    *out_esp_sectors = INSTALL_ESP_SECTORS;
    *out_root_lba = root_first;
    *out_root_sectors = last_usable - root_first + 1u;
    return 0;
}

/* Native filesystem modules share this core-owned interface; CRC/release
 * checks bind it to the exact kernel, without a stable cross-release API. */
EXPORT_SYMBOL(install_clear_sectors);
EXPORT_SYMBOL(install_write_sectors);
