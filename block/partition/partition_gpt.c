#include "internal.h"

/**
 * @brief Writes one updated GPT header to a managed disk.
 * @param disk Prepared block-disk state.
 * @param header Header to serialize.
 * @return Zero on success or a negative errno-style storage error.
 */
static int disk_gpt_write_header(struct install_disk_state *disk,
                                 const struct gpt_header *header);

/**
 * @brief Commits the GPT table in the crash-tolerant backup-before-primary order.
 * @param disk Prepared block-disk state.
 * @param table Validated table headers for the managed disk.
 * @return Zero on success or a negative errno-style storage error.
 */
static int disk_gpt_write(struct install_disk_state *disk, struct disk_gpt_table *table);

/**
 * @brief Converts a GPT UTF-16 name into the ASCII-safe public ABI field.
 * @param source GPT UTF-16 name field.
 * @param target Destination text buffer.
 * @param capacity Destination capacity in bytes.
 */
static void disk_gpt_name_to_text(const uint16_t source[36], char *target, uint32_t capacity);

/**
 * @brief Builds public metadata for one validated GPT partition.
 * @param disk_id Block-disk identifier.
 * @param disk Prepared block-disk state.
 * @param entry_index GPT entry index.
 * @param entry GPT partition entry.
 * @param out Destination public partition record.
 */
static int disk_partition_export(uint32_t disk_id, struct install_disk_state *disk,
                                 uint32_t entry_index, const struct gpt_entry *entry,
                                 struct reliefos_disk_partition *out);

/**
 * @brief Formats a validated data extent using the selected ReliefOS filesystem.
 * @param disk Prepared block-disk state.
 * @param entry GPT partition entry defining the data extent.
 * @param filesystem Selected RELIEFOS_DISK_FILESYSTEM value.
 * @return Zero on success or a negative errno-style storage error.
 */
static int disk_format_entry(struct install_disk_state *disk, const struct gpt_entry *entry,
                             uint32_t filesystem);

/**
 * @brief Assigns a standard GPT type GUID for a selected data filesystem.
 * @param entry Writable GPT entry.
 * @param filesystem Selected RELIEFOS_DISK_FILESYSTEM value.
 */
static void disk_gpt_set_filesystem_type(struct gpt_entry *entry, uint32_t filesystem);

static int disk_gpt_set_partition_type(struct gpt_entry *entry, uint32_t type);

/**
 * @brief Aligns a logical block address upward to the standard GPT 1 MiB boundary.
 * @param lba Logical block address to align.
 * @return Aligned LBA, or zero when alignment would overflow.
 */
static uint64_t disk_gpt_align_lba(uint64_t lba);

/**
 * @brief Finds the first aligned free GPT range large enough for a request.
 * @param table Loaded and validated GPT metadata.
 * @param required_sectors Number of aligned sectors requested.
 * @param out_first Receives the first sector in the free range.
 * @return Zero on success or a negative errno-style storage error.
 */
static int disk_gpt_find_free_range(const struct disk_gpt_table *table,
                                    uint64_t required_sectors, uint64_t *out_first);

/**
 * @brief Writes an ASCII-safe GPT name from a disk-management create request.
 * @param destination Writable GPT UTF-16 name field.
 * @param source User-selected partition label.
 */
static void disk_gpt_set_name(uint16_t destination[36], const char *source);


/**
 * @brief Writes one updated GPT header to a managed disk.
 * @param disk Prepared block-disk state.
 * @param header Header to serialize.
 * @return Zero on success or a negative errno-style storage error.
 */
static int disk_gpt_write_header(struct install_disk_state *disk,
                                 const struct gpt_header *header)
{
    if (!disk || !header) {
        return -22;
    }
    storage_memzero(storage_scratch, SECTOR_SIZE);
    storage_memcpy(storage_scratch, header, sizeof(*header));
    return install_write_sectors(disk, header->current_lba, 1u, storage_scratch);
}

/**
 * @brief Commits the GPT table in the crash-tolerant backup-before-primary order.
 * @param disk Prepared block-disk state.
 * @param table Validated table headers for the managed disk.
 * @return Zero on success or a negative errno-style storage error.
 */
static int disk_gpt_write(struct install_disk_state *disk, struct disk_gpt_table *table)
{
    uint32_t table_crc;
    int ret;
    if (!disk || !table || table->table_bytes == 0 ||
        table->table_bytes > sizeof(storage_cluster_buf)) {
        return -22;
    }
    /* A failed or partial metadata update must never leave a stale extent
     * cache available to a later /dev/sdXN or /dev/nvmeXnYpZ lookup.  Invalidate before the
     * first sector and keep it invalid on every error path; successful writes
     * are invalidated again below for clarity. */
    disk_block_cache_invalidate_disk(disk);
    table_crc = storage_crc32(storage_cluster_buf, table->table_bytes);
    table->backup.partition_entries_crc32 = table_crc;
    table->backup.header_crc32 = 0;
    table->backup.header_crc32 = storage_crc32(&table->backup, sizeof(table->backup));
    table->primary.partition_entries_crc32 = table_crc;
    table->primary.header_crc32 = 0;
    table->primary.header_crc32 = storage_crc32(&table->primary, sizeof(table->primary));
    ret = install_write_sectors(disk, table->backup.partition_entries_lba,
                                table->table_sectors, storage_cluster_buf);
    if (ret < 0) {
        return ret;
    }
    ret = disk_gpt_write_header(disk, &table->backup);
    if (ret < 0) {
        return ret;
    }
    ret = install_write_sectors(disk, table->primary.partition_entries_lba,
                                table->table_sectors, storage_cluster_buf);
    if (ret < 0) {
        return ret;
    }
    ret = disk_gpt_write_header(disk, &table->primary);
    disk_block_cache_invalidate_disk(disk);
    return ret;
}

/**
 * @brief Validates all populated GPT entries and rejects overlapping ranges.
 * @param table Loaded GPT table metadata.
 * @return Zero on success or a negative errno-style storage error.
 */
int disk_gpt_entries_valid(const struct disk_gpt_table *table)
{
    const struct gpt_entry *entries = (const struct gpt_entry *)(const void *)storage_cluster_buf;
    if (!table || !entries) {
        return -22;
    }
    for (uint32_t i = 0; i < table->primary.partition_entry_count; ++i) {
        const struct gpt_entry *entry = &entries[i];
        if (!disk_gpt_entry_used(entry)) {
            continue;
        }
        if (entry->first_lba < table->primary.first_usable_lba ||
            disk_gpt_guid_empty(entry->unique_guid) ||
            entry->last_lba < entry->first_lba ||
            entry->last_lba > table->primary.last_usable_lba) {
            return -22;
        }
        for (uint32_t j = 0; j < i; ++j) {
            const struct gpt_entry *other = &entries[j];
            if (disk_gpt_entry_used(other) &&
                !(entry->last_lba < other->first_lba || entry->first_lba > other->last_lba)) {
                return -22;
            }
        }
    }
    return 0;
}

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
                                     uint32_t *out_filesystem)
{
    uint64_t sectors;
    int ret;
    if (!disk || !entry || !out_filesystem || entry->last_lba < entry->first_lba) {
        return -22;
    }
    *out_filesystem = RELIEFOS_DISK_FILESYSTEM_UNKNOWN;
    sectors = entry->last_lba - entry->first_lba + 1u;
    ret = install_read_sectors(disk, entry->first_lba, 1u, storage_scratch);
    if (ret < 0) {
        return ret;
    }
    if (storage_scratch[510] == 0x55 && storage_scratch[511] == 0xaa &&
        storage_memcmp(storage_scratch + 82u, "FAT32   ", 8u) == 0) {
        *out_filesystem = RELIEFOS_DISK_FILESYSTEM_FAT32;
        return 0;
    }
    if (storage_scratch[510] == 0x55 && storage_scratch[511] == 0xaa &&
        storage_memcmp(storage_scratch + 3u, "EXFAT   ", 8u) == 0 &&
        storage_scratch[108u] == 9u && storage_scratch[110u] == 1u) {
        *out_filesystem = RELIEFOS_DISK_FILESYSTEM_EXFAT;
        return 0;
    }
    if (sectors > 2u) {
        ret = install_read_sectors(disk, entry->first_lba + 2u, 1u, storage_scratch);
        if (ret < 0) {
            return ret;
        }
        const struct ext2_superblock *super =
            (const struct ext2_superblock *)(const void *)storage_scratch;
        if (super->magic == EXT2_SUPER_MAGIC) {
            *out_filesystem = (super->feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) != 0
                                  ? RELIEFOS_DISK_FILESYSTEM_EXT4
                                  : RELIEFOS_DISK_FILESYSTEM_EXT2;
        }
    }
    return 0;
}

/**
 * @brief Converts a GPT UTF-16 name into the ASCII-safe public ABI field.
 * @param source GPT UTF-16 name field.
 * @param target Destination text buffer.
 * @param capacity Destination capacity in bytes.
 */
static void disk_gpt_name_to_text(const uint16_t source[36], char *target, uint32_t capacity)
{
    uint32_t out = 0;
    if (!target || capacity == 0) {
        return;
    }
    if (source) {
        for (uint32_t i = 0; i < 36u && source[i] != 0 && out + 1u < capacity; ++i) {
            uint16_t ch = source[i];
            target[out++] = ch >= 32u && ch <= 126u ? (char)ch : '?';
        }
    }
    target[out] = 0;
}

/**
 * @brief Builds public metadata for one validated GPT partition.
 * @param disk_id Block-disk identifier.
 * @param disk Prepared block-disk state.
 * @param entry_index GPT entry index.
 * @param entry GPT partition entry.
 * @param out Destination public partition record.
 */
static int disk_partition_export(uint32_t disk_id, struct install_disk_state *disk,
                                 uint32_t entry_index, const struct gpt_entry *entry,
                                 struct reliefos_disk_partition *out)
{
    if (!disk || !entry || !out) {
        return -22;
    }
    storage_memzero(out, sizeof(*out));
    out->disk_id = disk_id;
    out->index = entry_index;
    out->sector_count = entry->last_lba - entry->first_lba + 1u;
    out->first_lba = entry->first_lba;
    storage_memcpy(out->type_guid, entry->type_guid, sizeof(out->type_guid));
    disk_gpt_name_to_text(entry->name, out->name, sizeof(out->name));
    if (storage_memcmp(entry->type_guid, esp_guid, sizeof(entry->type_guid)) == 0) {
        out->flags |= RELIEFOS_DISK_PARTITION_FLAG_ESP;
    }
    if (disk->boot_root) {
        out->flags |= RELIEFOS_DISK_PARTITION_FLAG_BOOT_ROOT |
                      RELIEFOS_DISK_PARTITION_FLAG_PROTECTED;
    }
    if (disk->target_mounted) {
        out->flags |= RELIEFOS_DISK_PARTITION_FLAG_TARGET_MOUNTED |
                      RELIEFOS_DISK_PARTITION_FLAG_PROTECTED;
    }
    if (storage_disk_partition_mount_path(disk_id, entry_index, out->mount_path,
                                          sizeof(out->mount_path)) == 0) {
        out->flags |= RELIEFOS_DISK_PARTITION_FLAG_MOUNTED;
    }
    return disk_partition_filesystem(disk, entry, &out->filesystem);
}

/**
 * @brief Checks whether an operation may mutate a disk-management partition.
 * @param disk Block-disk state that owns the partition.
 * @return Zero if the disk is not a startup or mounted installer target.
 */
int disk_partition_mutable(const struct install_disk_state *disk)
{
    if (!disk) {
        return -22;
    }
    /* When booted from the installer ISO, / is the writable RAM root and the
     * physical disks are installation targets, even if probing them first
     * found an existing ReliefOS root. Keep the boot-disk guard for normal disk
     * boots while allowing the ISO's advanced environment to manage targets. */
    return ((disk->boot_root && !storage_installer_root_active()) ||
            disk->target_mounted) ? -16 : 0;
}

/**
 * @brief Tests a user-visible filesystem selection accepted by the formatter.
 * @param filesystem RELIEFOS_DISK_FILESYSTEM value.
 * @return Nonzero when the selection is FAT32, exFAT, ext4, or legacy ext2.
 */
int disk_filesystem_format_supported(uint32_t filesystem)
{
    return filesystem == RELIEFOS_DISK_FILESYSTEM_FAT32 ||
           filesystem == RELIEFOS_DISK_FILESYSTEM_EXT2 ||
           filesystem == RELIEFOS_DISK_FILESYSTEM_EXT4 ||
           filesystem == RELIEFOS_DISK_FILESYSTEM_EXFAT;
}

/**
 * @brief Formats a validated data extent using the selected ReliefOS filesystem.
 * @param disk Prepared block-disk state.
 * @param entry GPT partition entry defining the data extent.
 * @param filesystem Selected RELIEFOS_DISK_FILESYSTEM value.
 * @return Zero on success or a negative errno-style storage error.
 */
static int disk_format_entry(struct install_disk_state *disk, const struct gpt_entry *entry,
                             uint32_t filesystem)
{
    uint64_t sector_count;
    if (!disk || !entry || entry->last_lba < entry->first_lba ||
        !disk_filesystem_format_supported(filesystem)) {
        return -22;
    }
    sector_count = entry->last_lba - entry->first_lba + 1u;
    if (filesystem == RELIEFOS_DISK_FILESYSTEM_FAT32) {
        return install_format_fat32(disk, entry->first_lba, sector_count);
    }
    if (filesystem == RELIEFOS_DISK_FILESYSTEM_EXFAT) {
        return install_format_exfat(disk, entry->first_lba, sector_count);
    }
    if (filesystem == RELIEFOS_DISK_FILESYSTEM_EXT4) {
        return install_format_ext4(disk, entry->first_lba, sector_count);
    }
    return install_format_ext2(disk, entry->first_lba, sector_count);
}

/**
 * @brief Assigns a standard GPT type GUID for a selected data filesystem.
 * @param entry Writable GPT entry.
 * @param filesystem Selected RELIEFOS_DISK_FILESYSTEM value.
 */
static void disk_gpt_set_filesystem_type(struct gpt_entry *entry, uint32_t filesystem)
{
    if (!entry) {
        return;
    }
    if (filesystem == RELIEFOS_DISK_FILESYSTEM_UNKNOWN ||
        filesystem == RELIEFOS_DISK_FILESYSTEM_FAT32 ||
        filesystem == RELIEFOS_DISK_FILESYSTEM_EXFAT) {
        storage_memcpy(entry->type_guid, basic_data_guid, sizeof(entry->type_guid));
    } else if (filesystem == RELIEFOS_DISK_FILESYSTEM_EXT2 ||
               filesystem == RELIEFOS_DISK_FILESYSTEM_EXT4) {
        storage_memcpy(entry->type_guid, linux_filesystem_guid, sizeof(entry->type_guid));
    }
}

static int disk_gpt_set_partition_type(struct gpt_entry *entry, uint32_t type)
{
    if (!entry) {
        return -22;
    }
    if (type == RELIEFOS_DISK_PARTITION_TYPE_BASIC_DATA) {
        storage_memcpy(entry->type_guid, basic_data_guid, sizeof(entry->type_guid));
    } else if (type == RELIEFOS_DISK_PARTITION_TYPE_ESP) {
        storage_memcpy(entry->type_guid, esp_guid, sizeof(entry->type_guid));
    } else if (type == RELIEFOS_DISK_PARTITION_TYPE_LINUX) {
        storage_memcpy(entry->type_guid, linux_filesystem_guid, sizeof(entry->type_guid));
    } else {
        return -22;
    }
    return 0;
}

/**
 * @brief Aligns a logical block address upward to the standard GPT 1 MiB boundary.
 * @param lba Logical block address to align.
 * @return Aligned LBA, or zero when alignment would overflow.
 */
static uint64_t disk_gpt_align_lba(uint64_t lba)
{
    if (lba > UINT64_MAX - 2047u) {
        return 0;
    }
    return (lba + 2047u) & ~2047ULL;
}

/**
 * @brief Finds the first aligned free GPT range large enough for a request.
 * @param table Loaded and validated GPT metadata.
 * @param required_sectors Number of aligned sectors requested.
 * @param out_first Receives the first sector in the free range.
 * @return Zero on success or a negative errno-style storage error.
 */
static int disk_gpt_find_free_range(const struct disk_gpt_table *table,
                                    uint64_t required_sectors, uint64_t *out_first)
{
    const struct gpt_entry *entries = (const struct gpt_entry *)(const void *)storage_cluster_buf;
    uint64_t cursor;
    if (!table || !entries || !out_first || required_sectors == 0) {
        return -22;
    }
    cursor = table->primary.first_usable_lba;
    while (cursor <= table->primary.last_usable_lba) {
        uint64_t candidate = disk_gpt_align_lba(cursor);
        uint64_t next = table->primary.last_usable_lba + 1u;
        uint64_t next_end = 0;
        if (candidate == 0 || candidate > table->primary.last_usable_lba) {
            return -28;
        }
        for (uint32_t i = 0; i < table->primary.partition_entry_count; ++i) {
            const struct gpt_entry *entry = &entries[i];
            if (disk_gpt_entry_used(entry) && entry->first_lba >= cursor &&
                entry->first_lba < next) {
                next = entry->first_lba;
                next_end = entry->last_lba;
            }
        }
        if (candidate <= next && required_sectors <= next - candidate) {
            *out_first = candidate;
            return 0;
        }
        if (next > table->primary.last_usable_lba || next_end == UINT64_MAX) {
            return -28;
        }
        cursor = next_end + 1u;
    }
    return -28;
}

/**
 * @brief Writes an ASCII-safe GPT name from a disk-management create request.
 * @param destination Writable GPT UTF-16 name field.
 * @param source User-selected partition label.
 */
static void disk_gpt_set_name(uint16_t destination[36], const char *source)
{
    uint32_t i = 0;
    if (!destination) {
        return;
    }
    for (; i < 36u && source && source[i]; ++i) {
        uint8_t ch = (uint8_t)source[i];
        destination[i] = ch >= 32u && ch <= 126u ? ch : '?';
    }
    for (; i < 36u; ++i) {
        destination[i] = 0;
    }
}

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
                                 uint32_t capacity, uint32_t *out_count)
{
    struct install_disk_state *disk;
    struct disk_gpt_table table;
    const struct gpt_entry *entries;
    uint64_t sector_count;
    uint32_t count = 0;
    if (!out_count || (capacity && !partitions)) {
        return -22;
    }
    *out_count = 0;
    int ret = storage_acquire_task_io();
    if (ret < 0) {
        return ret;
    }
    if (capacity > RELIEFOS_DISK_MAX_PARTITIONS) {
        capacity = RELIEFOS_DISK_MAX_PARTITIONS;
    }
    ret = disk_manage_prepare(disk_id, &disk, &sector_count);
    if (ret < 0) {
        return ret;
    }
    ret = disk_gpt_load(disk, sector_count, &table);
    if (ret < 0) {
        if (ret == -2) {
            *out_count = 0;
            return 0;
        }
        return ret;
    }
    ret = disk_gpt_entries_valid(&table);
    if (ret < 0) {
        return ret;
    }
    entries = (const struct gpt_entry *)(const void *)storage_cluster_buf;
    for (uint32_t i = 0; i < table.primary.partition_entry_count; ++i) {
        if (!disk_gpt_entry_used(&entries[i])) {
            continue;
        }
        if (count < capacity) {
            ret = disk_partition_export(disk_id, disk, i, &entries[i], &partitions[count]);
            if (ret < 0) {
                return ret;
            }
        }
        ++count;
    }
    *out_count = count;
    return 0;
}

/**
 * @brief Initializes a protective MBR and empty primary/backup GPT pair.
 *
 * This is intentionally separate from the normal GPT editor.  The editor
 * requires a valid table, while this operation is the explicit destructive
 * entry point used by the installer ISO's gptinit utility.
 */
int storage_disk_initialize_gpt(const struct reliefos_disk_gpt_initialize *request)
{
    struct install_disk_state *disk;
    struct gpt_header *header;
    uint8_t disk_guid[16];
    uint64_t sector_count;
    uint64_t last_lba;
    uint64_t backup_entries_lba;
    uint64_t first_usable = 2048u;
    uint64_t last_usable;
    uint32_t table_bytes = GPT_ENTRY_COUNT * GPT_ENTRY_SIZE;
    uint32_t table_sectors = table_bytes / SECTOR_SIZE;
    uint32_t table_crc;
    int ret;

    ret = storage_acquire_task_io();
    if (ret < 0) {
        return ret;
    }
    if (!request || (request->flags & ~RELIEFOS_DISK_GPT_INITIALIZE_FORCE) != 0) {
        return -22;
    }
    ret = disk_manage_prepare(request->disk_id, &disk, &sector_count);
    if (ret < 0) {
        return ret;
    }
    ret = disk_partition_mutable(disk);
    if (ret < 0) {
        return ret;
    }
    /* This operation intentionally replaces all partition metadata.  Drop
     * any extents learned from a previous table before clearing sector 0 and
     * the GPT copies, including when a later write fails. */
    disk_block_cache_invalidate_disk(disk);
    if (sector_count < 655360ULL) {
        return -28;
    }

    /* Refuse to replace a valid table unless the caller explicitly opted in. */
    {
        struct disk_gpt_table existing;
        ret = disk_gpt_load(disk, sector_count, &existing);
        if (ret == 0 && !(request->flags & RELIEFOS_DISK_GPT_INITIALIZE_FORCE)) {
            return -17;
        }
    }

    last_lba = sector_count - 1u;
    backup_entries_lba = last_lba - table_sectors;
    last_usable = backup_entries_lba - 1u;
    if (last_usable <= first_usable) {
        return -28;
    }
    storage_make_guid(disk_guid, 0x4c3447494e495447ULL, disk, sector_count);

    /* Clear old metadata, including stale backup headers and partition rows. */
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
    storage_put_u32(storage_scratch + 446 + 8, 1u);
    storage_put_u32(storage_scratch + 446 + 12,
                    sector_count - 1u > 0xffffffffULL
                        ? 0xffffffffu : (uint32_t)(sector_count - 1u));
    storage_scratch[510] = 0x55;
    storage_scratch[511] = 0xaa;
    ret = install_write_sectors(disk, 0, 1u, storage_scratch);
    if (ret < 0) {
        return ret;
    }

    storage_memzero(storage_cluster_buf, table_bytes);
    table_crc = storage_crc32(storage_cluster_buf, table_bytes);
    ret = install_write_sectors(disk, 2u, table_sectors, storage_cluster_buf);
    if (ret < 0) {
        return ret;
    }
    ret = install_write_sectors(disk, backup_entries_lba, table_sectors,
                                storage_cluster_buf);
    if (ret < 0) {
        return ret;
    }

    storage_memzero(storage_scratch, SECTOR_SIZE);
    header = (struct gpt_header *)(void *)storage_scratch;
    header->signature = 0x5452415020494645ULL;
    header->revision = 0x00010000u;
    header->header_size = sizeof(struct gpt_header);
    header->current_lba = 1u;
    header->backup_lba = last_lba;
    header->first_usable_lba = first_usable;
    header->last_usable_lba = last_usable;
    storage_memcpy(header->disk_guid, disk_guid, sizeof(header->disk_guid));
    header->partition_entries_lba = 2u;
    header->partition_entry_count = GPT_ENTRY_COUNT;
    header->partition_entry_size = GPT_ENTRY_SIZE;
    header->partition_entries_crc32 = table_crc;
    header->header_crc32 = storage_crc32(header, header->header_size);
    ret = install_write_sectors(disk, 1u, 1u, storage_scratch);
    if (ret < 0) {
        return ret;
    }

    header->header_crc32 = 0;
    header->current_lba = last_lba;
    header->backup_lba = 1u;
    header->partition_entries_lba = backup_entries_lba;
    header->header_crc32 = storage_crc32(header, header->header_size);
    ret = install_write_sectors(disk, last_lba, 1u, storage_scratch);
    if (ret == 0) disk_block_cache_invalidate_disk(disk);
    return ret;
}

/**
 * @brief Formats an existing unprotected GPT partition as FAT32, exFAT, ext4,
 *        or legacy ext2.
 * @param request Validated disk-management format request.
 * @return Zero on success or a negative errno-style storage error.
 */
int storage_disk_format_partition(const struct reliefos_disk_partition_format *request)
{
    struct install_disk_state *disk;
    struct disk_gpt_table table;
    struct gpt_entry entry;
    struct gpt_entry *entries;
    uint64_t sector_count;
    uint32_t mounted_volume;
    int ret = storage_acquire_task_io();
    if (ret < 0) {
        return ret;
    }
    if (!request || !disk_filesystem_format_supported(request->filesystem)) {
        return -22;
    }
    ret = disk_manage_prepare(request->disk_id, &disk, &sector_count);
    if (ret < 0) return ret;
    ret = disk_partition_mutable(disk);
    if (ret < 0) return ret;
    ret = disk_gpt_load(disk, sector_count, &table);
    if (ret < 0 || request->partition_index >= table.primary.partition_entry_count) {
        return ret < 0 ? ret : -22;
    }
    ret = disk_gpt_entries_valid(&table);
    if (ret < 0) {
        return ret;
    }
    entries = (struct gpt_entry *)(void *)storage_cluster_buf;
    if (!disk_gpt_entry_used(&entries[request->partition_index])) {
        return -2;
    }
    if (storage_disk_partition_volume_id(request->disk_id, request->partition_index,
                                         &mounted_volume) == 0) {
        return -RELIEFOS_EBUSY;
    }
    entry = entries[request->partition_index];
    ret = disk_format_entry(disk, &entry, request->filesystem);
    if (ret < 0) {
        return ret;
    }
    ret = disk_gpt_load(disk, sector_count, &table);
    if (ret < 0) {
        return ret;
    }
    entries = (struct gpt_entry *)(void *)storage_cluster_buf;
    if (request->partition_index >= table.primary.partition_entry_count ||
        !disk_gpt_entry_used(&entries[request->partition_index])) {
        return -5;
    }
    disk_gpt_set_filesystem_type(&entries[request->partition_index], request->filesystem);
    return disk_gpt_write(disk, &table);
}

/**
 * @brief Deletes an unprotected GPT partition entry while leaving its data intact.
 * @param request Validated disk-management delete request.
 * @return Zero on success or a negative errno-style storage error.
 */
int storage_disk_delete_partition(const struct reliefos_disk_partition_delete *request)
{
    struct install_disk_state *disk;
    struct disk_gpt_table table;
    struct gpt_entry *entries;
    uint64_t sector_count;
    uint32_t mounted_volume;
    int ret = storage_acquire_task_io();
    if (ret < 0) {
        return ret;
    }
    if (!request) {
        return -22;
    }
    ret = disk_manage_prepare(request->disk_id, &disk, &sector_count);
    if (ret < 0) return ret;
    ret = disk_partition_mutable(disk);
    if (ret < 0) return ret;
    ret = disk_gpt_load(disk, sector_count, &table);
    if (ret < 0 || request->partition_index >= table.primary.partition_entry_count) {
        return ret < 0 ? ret : -22;
    }
    ret = disk_gpt_entries_valid(&table);
    if (ret < 0) {
        return ret;
    }
    entries = (struct gpt_entry *)(void *)storage_cluster_buf;
    if (!disk_gpt_entry_used(&entries[request->partition_index])) {
        return -2;
    }
    if (storage_disk_partition_volume_id(request->disk_id, request->partition_index,
                                         &mounted_volume) == 0) {
        return -RELIEFOS_EBUSY;
    }
    storage_memzero(&entries[request->partition_index], sizeof(entries[request->partition_index]));
    return disk_gpt_write(disk, &table);
}

/** Edit standard GPT type and/or printable partition name in both GPT copies. */
int storage_disk_edit_partition(const struct reliefos_disk_partition_edit *request)
{
    struct install_disk_state *disk;
    struct disk_gpt_table table;
    struct gpt_entry *entries;
    uint64_t sector_count;
    uint32_t mounted_volume;
    int ret = storage_acquire_task_io();
    if (ret < 0) {
        return ret;
    }
    if (!request || request->edit_mask == 0 ||
        (request->edit_mask & ~(RELIEFOS_DISK_PARTITION_EDIT_TYPE |
                                RELIEFOS_DISK_PARTITION_EDIT_NAME)) != 0) {
        return -22;
    }
    ret = disk_manage_prepare(request->disk_id, &disk, &sector_count);
    if (ret < 0) return ret;
    ret = disk_partition_mutable(disk);
    if (ret < 0) return ret;
    ret = disk_gpt_load(disk, sector_count, &table);
    if (ret < 0 || request->partition_index >= table.primary.partition_entry_count) {
        return ret < 0 ? ret : -22;
    }
    ret = disk_gpt_entries_valid(&table);
    if (ret < 0) {
        return ret;
    }
    entries = (struct gpt_entry *)(void *)storage_cluster_buf;
    if (!disk_gpt_entry_used(&entries[request->partition_index])) {
        return -2;
    }
    if (storage_disk_partition_volume_id(request->disk_id, request->partition_index,
                                         &mounted_volume) == 0) {
        return -RELIEFOS_EBUSY;
    }
    if (request->edit_mask & RELIEFOS_DISK_PARTITION_EDIT_TYPE) {
        ret = disk_gpt_set_partition_type(&entries[request->partition_index], request->type);
        if (ret < 0) {
            return ret;
        }
    }
    if (request->edit_mask & RELIEFOS_DISK_PARTITION_EDIT_NAME) {
        disk_gpt_set_name(entries[request->partition_index].name, request->name);
    }
    return disk_gpt_write(disk, &table);
}

/**
 * @brief Creates and formats a data partition in the first suitable free GPT range.
 * @param request Validated disk-management create request.
 * @return Zero on success or a negative errno-style storage error.
 */
int storage_disk_create_partition(const struct reliefos_disk_partition_create *request)
{
    struct install_disk_state *disk;
    struct disk_gpt_table table;
    struct gpt_entry *entries;
    uint64_t sector_count;
    uint64_t required_sectors;
    uint64_t first_lba;
    uint32_t free_index = RELIEFOS_DISK_MAX_PARTITIONS;
    int ret = storage_acquire_task_io();
    if (ret < 0) {
        return ret;
    }
    if (!request || request->size_mib == 0 ||
        (request->filesystem != RELIEFOS_DISK_FILESYSTEM_UNKNOWN &&
         !disk_filesystem_format_supported(request->filesystem))) {
        return -22;
    }
    required_sectors = (uint64_t)request->size_mib * 2048u;
    ret = disk_manage_prepare(request->disk_id, &disk, &sector_count);
    if (ret < 0) return ret;
    ret = disk_partition_mutable(disk);
    if (ret < 0) return ret;
    ret = disk_gpt_load(disk, sector_count, &table);
    if (ret < 0) {
        return ret;
    }
    ret = disk_gpt_entries_valid(&table);
    if (ret < 0) {
        return ret;
    }
    entries = (struct gpt_entry *)(void *)storage_cluster_buf;
    for (uint32_t i = 0; i < table.primary.partition_entry_count; ++i) {
        if (!disk_gpt_entry_used(&entries[i])) {
            free_index = i;
            break;
        }
    }
    if (free_index == RELIEFOS_DISK_MAX_PARTITIONS ||
        disk_gpt_find_free_range(&table, required_sectors, &first_lba) < 0 ||
        first_lba > table.primary.last_usable_lba ||
        required_sectors > table.primary.last_usable_lba - first_lba + 1u) {
        return -28;
    }
    storage_memzero(&entries[free_index], sizeof(entries[free_index]));
    entries[free_index].first_lba = first_lba;
    entries[free_index].last_lba = first_lba + required_sectors - 1u;
    storage_make_guid(entries[free_index].unique_guid,
                      0x4c34475041525431ULL ^ ((uint64_t)free_index << 32) ^ first_lba,
                      disk, sector_count);
    disk_gpt_set_filesystem_type(&entries[free_index], request->filesystem);
    disk_gpt_set_name(entries[free_index].name,
                      request->name[0] ? request->name : "ReliefOS Data");
    ret = disk_gpt_write(disk, &table);
    if (ret < 0) {
        return ret;
    }
    if (request->filesystem == RELIEFOS_DISK_FILESYSTEM_UNKNOWN) {
        return 0;
    }
    return disk_format_entry(disk, &entries[free_index], request->filesystem);
}
