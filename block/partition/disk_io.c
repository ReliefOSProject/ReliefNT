#include "internal.h"
#include <reliefnt/heap.h>

static int disk_block_cache_load(uint32_t disk_id, struct install_disk_state *disk,
                                 uint64_t sector_count);

static int disk_block_range(uint32_t disk_id, struct install_disk_state *disk,
                            uint64_t sector_count, int32_t partition_index,
                            uint64_t *out_first_lba, uint64_t *out_sector_count);

/**
 * @brief Validates an on-disk GPT header before its table is read or modified.
 * @param header Header copied from the managed disk.
 * @param sector_count Total number of addressable disk sectors.
 * @return Zero on success or a negative errno-style storage error.
 */
static int disk_gpt_header_valid(const struct gpt_header *header, uint64_t sector_count);

/** @brief Retire cached GPT validity for one stable core disk identity.
 * @param disk_id Reserved inventory slot; unknown/no-cache entries are ignored.
 * Execution ownership required; no I/O/allocation/reference transfer, cache
 * bytes remain owned by their core record until reset/device retirement.
 */
void disk_block_cache_invalidate(uint32_t disk_id)
{
    if (disk_id < g_install_disk_count && g_install_disks[disk_id].block_cache) {
        g_install_disks[disk_id].block_cache->valid = 0;
    }
}

/** @brief Consume all core GPT caches during a quiescent boot reset.
 * Execution ownership before concurrent cache users; each exact allocation is
 * freed once and its record pointer cleared. No native code, I/O or code pin.
 */
void storage_disk_block_cache_reset(void)
{
    for(uint32_t i=0;i<g_install_disk_count;++i) {
        kernel_free(g_install_disks[i].block_cache);g_install_disks[i].block_cache=NULL;
    }
}

void disk_block_cache_invalidate_disk(const struct install_disk_state *disk)
{
    if (!disk) return;
    for (uint32_t i = 0; i < g_install_disk_count; ++i) {
        if (&g_install_disks[i] == disk) {
            disk_block_cache_invalidate(i);
            return;
        }
    }
}

/** @brief Lazily allocate/validate the exact core record's native GPT snapshot.
 * @param disk_id Stable admitted inventory slot. @param disk Borrowed record
 * valid for this execution transaction. @param sector_count Actual disk sectors.
 * @return Zero or allocation/geometry/native errno. Caller holds a complete
 * block/code transaction pin; cache is core-owned and no record pointer escapes.
 * OOM leaves existing geometry/cache state intact, no registry lock spans I/O.
 */
static int disk_block_cache_load(uint32_t disk_id, struct install_disk_state *disk,
                                 uint64_t sector_count)
{
    struct disk_block_partition_cache *cache;
    struct disk_gpt_table table;
    const struct gpt_entry *entries;
    int ret;
    if (disk_id >= g_install_disk_count || !disk) return -22;
    if(!disk->block_cache) {
        disk->block_cache=kernel_malloc(sizeof(*disk->block_cache));
        if(!disk->block_cache)return -12;
        storage_memzero(disk->block_cache,sizeof(*disk->block_cache));
    }
    cache = disk->block_cache;
    if (cache->valid && cache->disk_sectors == sector_count) {
        return cache->status;
    }
    ret = disk_gpt_load(disk, sector_count, &table);
    if (ret < 0) {
        /* Only content errors are stable enough to cache. Transport failures
         * such as EIO may be transient (especially on AHCI after reset); a
         * cached EIO made every later /dev/sdXN or /dev/nvmeXnYpZ lookup fail until reboot. */
        if (ret == -2 || ret == -22) {
            storage_memzero(cache, sizeof(*cache));
            cache->disk_sectors = sector_count;
            cache->status = ret;
            cache->valid = 1;
        }
        return ret;
    }
    ret = disk_gpt_entries_valid(&table);
    if (ret < 0) {
        /* Entry validation has completed from disk data, so this is a stable
         * malformed-GPT result rather than an in-flight transport failure. */
        storage_memzero(cache, sizeof(*cache));
        cache->disk_sectors = sector_count;
        cache->status = ret;
        cache->valid = 1;
        return ret;
    }
    entries = (const struct gpt_entry *)(const void *)storage_cluster_buf;
    storage_memzero(cache, sizeof(*cache));
    cache->disk_sectors = sector_count;
    cache->status = 0;
    for (uint32_t i = 0; i < table.primary.partition_entry_count; ++i) {
        if (!disk_gpt_entry_used(&entries[i])) continue;
        cache->entries[i].first_lba = entries[i].first_lba;
        cache->entries[i].sector_count = entries[i].last_lba - entries[i].first_lba + 1u;
        cache->entries[i].present = 1;
        __builtin_memcpy(cache->entries[i].unique_guid, entries[i].unique_guid, 16);
    }
    cache->valid = 1;
    return 0;
}

static int disk_block_range(uint32_t disk_id, struct install_disk_state *disk,
                            uint64_t sector_count, int32_t partition_index,
                            uint64_t *out_first_lba, uint64_t *out_sector_count)
{
    int ret;

    if (!disk || !out_first_lba || !out_sector_count) return -22;
    if (partition_index == -1) {
        *out_first_lba = 0;
        *out_sector_count = sector_count;
        return 0;
    }
    if (partition_index < -1 || (uint32_t)partition_index >= RELIEFOS_DISK_MAX_PARTITIONS) {
        return -22;
    }
    ret = disk_block_cache_load(disk_id, disk, sector_count);
    if (ret < 0) return ret;
    if (!g_install_disks[disk_id].block_cache->entries[partition_index].present) return -2;
    *out_first_lba = g_install_disks[disk_id].block_cache->entries[partition_index].first_lba;
    *out_sector_count = g_install_disks[disk_id].block_cache->entries[partition_index].sector_count;
    return 0;
}

/**
 * @brief Checks whether a GPT GUID is all zeroes.
 * @param guid GPT type or unique GUID.
 * @return Nonzero when the GUID denotes an unused entry.
 */
int disk_gpt_guid_empty(const uint8_t guid[16])
{
    if (!guid) {
        return 1;
    }
    for (uint32_t i = 0; i < 16; ++i) {
        if (guid[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Tests whether a GPT entry is allocated.
 * @param entry GPT partition entry to inspect.
 * @return Nonzero if the entry has a type GUID.
 */
int disk_gpt_entry_used(const struct gpt_entry *entry)
{
    return entry && !disk_gpt_guid_empty(entry->type_guid);
}

/**
 * @brief Validates an on-disk GPT header before its table is read or modified.
 * @param header Header copied from the managed disk.
 * @param sector_count Total number of addressable disk sectors.
 * @return Zero on success or a negative errno-style storage error.
 */
static int disk_gpt_header_valid(const struct gpt_header *header, uint64_t sector_count)
{
    struct gpt_header check;
    uint64_t table_bytes;
    uint64_t table_sectors;
    uint64_t table_last_lba;
    if (!header || sector_count < 2u ||
        header->signature != 0x5452415020494645ULL ||
        header->revision < 0x00010000u ||
        header->header_size != sizeof(struct gpt_header) ||
        header->reserved != 0 ||
        header->current_lba >= sector_count || header->backup_lba >= sector_count ||
        header->backup_lba == header->current_lba ||
        header->first_usable_lba > header->last_usable_lba ||
        header->last_usable_lba >= sector_count || header->first_usable_lba < 2u ||
        header->partition_entry_count == 0 ||
        header->partition_entry_count > RELIEFOS_DISK_MAX_PARTITIONS ||
        header->partition_entry_size != sizeof(struct gpt_entry)) {
        return -22;
    }
    table_bytes = (uint64_t)header->partition_entry_count * header->partition_entry_size;
    table_sectors = (table_bytes + SECTOR_SIZE - 1u) / SECTOR_SIZE;
    if (table_bytes > sizeof(storage_cluster_buf) ||
        header->partition_entries_lba < 2u ||
        header->partition_entries_lba >= sector_count ||
        table_sectors > sector_count - header->partition_entries_lba ||
        table_sectors == 0) {
        return -22;
    }
    table_last_lba = header->partition_entries_lba + table_sectors - 1u;
    if ((header->current_lba >= header->first_usable_lba &&
         header->current_lba <= header->last_usable_lba) ||
        (header->backup_lba >= header->first_usable_lba &&
         header->backup_lba <= header->last_usable_lba) ||
        !(table_last_lba < header->first_usable_lba ||
          header->partition_entries_lba > header->last_usable_lba)) {
        return -22;
    }
    check = *header;
    check.header_crc32 = 0;
    return storage_crc32(&check, sizeof(check)) == header->header_crc32 ? 0 : -5;
}

/**
 * @brief Prepares a block disk for a synchronous disk-management operation.
 * @param disk_id Detected AHCI, IDE/PATA, or NVMe disk index.
 * @param out_disk Receives the selected block-disk state.
 * @param out_sector_count Receives the total disk sectors.
 * @return Zero on success or a negative errno-style storage error.
 */
int disk_manage_prepare(uint32_t disk_id, struct install_disk_state **out_disk,
                               uint64_t *out_sector_count)
{
    struct install_disk_state *disk;
    uint64_t sector_count;
    int ret;
    storage_refresh_disks();
    if (!out_disk || !out_sector_count || disk_id >= g_install_disk_count ||
        !g_install_disks[disk_id].present) {
        return -2;
    }
    disk = &g_install_disks[disk_id];
    ret = storage_prepare_install_disk(disk);
    if (ret < 0) {
        return ret;
    }
    sector_count = disk->sector_count;
    if (sector_count == 0) {
        ret = storage_install_identify(disk, &sector_count);
        if (ret < 0) {
            return ret;
        }
        disk->sector_count = sector_count;
    }
    *out_disk = disk;
    *out_sector_count = sector_count;
    return 0;
}

/**
 * @brief Reads and validates both GPT headers and the primary partition table.
 * @param disk Prepared block-disk state.
 * @param sector_count Total number of disk sectors.
 * @param out_table Receives validated GPT header metadata.
 * @return Zero on success or a negative errno-style storage error.
 */
int disk_gpt_load(struct install_disk_state *disk, uint64_t sector_count,
                         struct disk_gpt_table *out_table)
{
    struct gpt_header primary;
    struct gpt_header backup;
    uint32_t table_bytes;
    uint32_t table_sectors;
    int ret;
    if (!disk || !out_table) {
        return -22;
    }
    ret = install_read_sectors(disk, 1u, 1u, storage_scratch);
    if (ret < 0) {
        return ret;
    }
    storage_memcpy(&primary, storage_scratch, sizeof(primary));
    if (primary.signature != 0x5452415020494645ULL) {
        return -2;
    }
    ret = disk_gpt_header_valid(&primary, sector_count);
    if (ret < 0 || primary.current_lba != 1u) {
        return ret < 0 ? ret : -22;
    }
    table_bytes = primary.partition_entry_count * primary.partition_entry_size;
    table_sectors = (table_bytes + SECTOR_SIZE - 1u) / SECTOR_SIZE;
    ret = install_read_sectors(disk, primary.backup_lba, 1u, storage_scratch);
    if (ret == -RELIEFOS_EAGAIN) {
        /* The AHCI async path still owns the pending command.  Do not treat
         * an incomplete backup-header read as a missing header and submit a
         * second request against the shared command slot. */
        return ret;
    }
    if (ret < 0) {
        storage_memcpy(&backup, &primary, sizeof(backup));
        backup.current_lba = sector_count - 1u;
        backup.backup_lba = primary.current_lba;
        backup.partition_entries_lba = sector_count - table_sectors - 1u;
        backup.header_crc32 = 0;
    } else {
        storage_memcpy(&backup, storage_scratch, sizeof(backup));
    }
    ret = disk_gpt_header_valid(&backup, sector_count);
    if (ret < 0 || backup.current_lba != primary.backup_lba ||
        backup.backup_lba != primary.current_lba ||
        backup.partition_entry_count != primary.partition_entry_count ||
        backup.partition_entry_size != primary.partition_entry_size ||
        storage_memcmp(backup.disk_guid, primary.disk_guid,
                       sizeof(primary.disk_guid)) != 0 ||
        backup.first_usable_lba != primary.first_usable_lba ||
        backup.last_usable_lba != primary.last_usable_lba ||
        backup.partition_entries_crc32 != primary.partition_entries_crc32) {
        storage_memcpy(&backup, &primary, sizeof(backup));
        backup.current_lba = sector_count - 1u;
        backup.backup_lba = primary.current_lba;
        backup.partition_entries_lba = sector_count - table_sectors - 1u;
        backup.header_crc32 = 0;
    }
    ret = install_read_sectors(disk, primary.partition_entries_lba, table_sectors,
                               storage_cluster_buf);
    if (ret < 0) {
        return ret;
    }
    if (storage_crc32(storage_cluster_buf, table_bytes) != primary.partition_entries_crc32) {
        return -5;
    }
    out_table->primary = primary;
    out_table->backup = backup;
    out_table->table_bytes = table_bytes;
    out_table->table_sectors = table_sectors;
    return 0;
}

static int storage_disk_block_info_owned(uint32_t disk_id, int32_t partition_index,
                            uint64_t *out_first_lba, uint64_t *out_sector_count)
{
    struct install_disk_state *disk;
    uint64_t sector_count;
    int ret;
    if (!out_first_lba || !out_sector_count) return -22;
    *out_first_lba = 0;
    *out_sector_count = 0;
    ret = storage_acquire_task_io();
    if (ret < 0) return ret;
    ret = disk_manage_prepare(disk_id, &disk, &sector_count);
    if (ret < 0) return ret;
    return disk_block_range(disk_id, disk, sector_count, partition_index,
                            out_first_lba, out_sector_count);
}

/** @brief Flush one raw disk/optical transaction with exact block/code ownership.
 * @param disk_id Stable inventory or optical identity.
 * @return Zero or native lookup/flush errno. Owns execution and transport
 * exclusion, forces synchronous completion, releases token on every exit.
 * Read-only optical providers complete without unsupported ATA flush commands.
 */
int storage_sync_disk(uint32_t disk_id)
{
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    struct block_device_ref ref={0};int rc=storage_disk_block_get(disk_id,&ref);
    if(!rc) {
        kernel_spin_lock(&storage_transport_lock);
        bool async=storage_io_async_context;storage_io_async_context=false;
        rc=block_device_flush(&ref);storage_io_async_context=async;
        kernel_spin_unlock(&storage_transport_lock);
    }
    block_device_put(&ref);kernel_execution_unlock_irqrestore(flags);return rc;
}

/** @brief Return a validated GPT partition UUID; caller holds the storage lock.
 * @param disk_id Published disk index.
 * @param partition_index Zero-based GPT slot.
 * @param uuid Output buffer of at least 37 bytes, including NUL.
 * @return Zero or negative errno; no UUID is fabricated for non-GPT disks.
 */
static int storage_disk_partition_uuid_owned(uint32_t disk_id, uint32_t partition_index, char uuid[37])
{
    if (!uuid || disk_id >= g_install_disk_count || partition_index >= RELIEFOS_DISK_MAX_PARTITIONS) return -22;
    uint64_t first, sectors;
    int ret = storage_disk_block_info(disk_id, (int32_t)partition_index, &first, &sectors);
    if (ret < 0) return ret;
    const uint8_t *guid = g_install_disks[disk_id].block_cache->entries[partition_index].unique_guid;
    storage_partition_guid_text(guid, uuid);
    return 0;
}

static int storage_disk_block_read_owned(uint32_t disk_id, int32_t partition_index,
                            uint64_t offset, void *buffer, uint32_t length,
                            uint32_t *out_read)
{
    uint64_t first_lba, sectors;
    uint32_t done = 0;
    uint8_t *destination = (uint8_t *)buffer;
    struct install_disk_state *disk;
    int ret;
    if (out_read) *out_read = 0;
    if (!buffer && length) return -22;
    if (!length) return 0;
    ret = storage_acquire_task_io();
    if (ret < 0) return ret;
    ret = disk_manage_prepare(disk_id, &disk, &sectors);
    if (ret < 0) return ret;
    ret = disk_block_range(disk_id, disk, sectors, partition_index, &first_lba, &sectors);
    if (ret < 0) return ret;
    if (sectors > UINT64_MAX / 512u) return -75;
    uint64_t bytes = sectors * 512u;
    if (offset >= bytes) return 0;
    if (length > bytes - offset) length = bytes - offset;
    /* Block drivers program DMA with physical kernel addresses.  A raw
     * device syscall receives a user virtual address, so it must never be
     * used directly as a DMA target.  Transfer through the kernel-owned,
     * page-aligned scratch buffer in bounded chunks instead. */
    while (done < length) {
        uint64_t position = offset + done;
        uint32_t skip = position % 512u;
        uint32_t take = min_u32(STORAGE_SCRATCH_SECTORS * 512u - skip, length - done);
        ret = install_read_sectors(disk, first_lba + position / 512u,
                                   (skip + take + 511u) / 512u, storage_scratch);
        if (ret < 0) { if (out_read) *out_read = done; return ret; }
        storage_memcpy(destination + done, storage_scratch + skip, take);
        done += take;
    }
    if (out_read) *out_read = length;
    return 0;
}

static int storage_disk_block_write_owned(uint32_t disk_id, int32_t partition_index,
                             uint64_t offset, const void *buffer, uint32_t length,
                             uint32_t *out_written)
{
    uint64_t first_lba, sectors;
    uint32_t done = 0;
    const uint8_t *source = (const uint8_t *)buffer;
    struct install_disk_state *disk;
    int ret;
    if (out_written) *out_written = 0;
    if (!buffer && length) return -22;
    if (!length) return 0;
    ret = storage_acquire_task_io();
    if (ret < 0) return ret;
    ret = disk_manage_prepare(disk_id, &disk, &sectors);
    if (ret < 0) return ret;
    ret = disk_block_range(disk_id, disk, sectors, partition_index, &first_lba, &sectors);
    if (ret < 0) return ret;
    if (sectors > UINT64_MAX / 512u) return -75;
    uint64_t bytes = sectors * 512u;
    if (offset >= bytes) return -28;
    if (length > bytes - offset) length = bytes - offset;
    /* Linux raw block I/O follows descriptor DAC/access-mode checks. Mount
     * conflicts apply to exclusive opens, rather than every write to another
     * range on the same disk (for example GRUB embedding beside an ESP). */
    if (partition_index < 0) {
        /* A whole-disk write may modify either GPT copy even when the caller
         * ultimately reports an I/O error.  Drop cached partition extents
         * before issuing the first sector so a subsequent operation cannot
         * use metadata from before a partially completed update. */
        disk_block_cache_invalidate(disk_id);
    }
    storage_begin_mutation();
    /* See storage_disk_block_read(): source user mappings are not DMA-safe.
     * Copy each sector-aligned request into kernel memory before submitting it
     * to the controller. */
    while (done < length) {
        uint64_t position = offset + done;
        uint32_t skip = position % 512u;
        uint32_t take = min_u32(STORAGE_SCRATCH_SECTORS * 512u - skip, length - done);
        uint32_t chunk = (skip + take + 511u) / 512u;
        if (skip || (take & 511u)) {
            ret = install_read_sectors(disk, first_lba + position / 512u, chunk, storage_scratch);
            if (ret < 0) { if (out_written) *out_written = done; return ret; }
        }
        storage_memcpy(storage_scratch + skip, source + done, take);
        ret = install_write_sectors(disk, first_lba + position / 512u, chunk, storage_scratch);
        if (ret < 0) { if (out_written) *out_written = done; return ret; }
        done += take;
    }
    /* A write through a partition node cannot change its GPT extent. Keeping
     * that range cached avoids re-reading the table before every 4 KiB mkfs
     * write. Whole-disk raw writes may alter GPT metadata, so they still
     * force rediscovery before the next partition access. */
    if (partition_index < 0) {
        disk_block_cache_invalidate(disk_id);
    }
    if (out_written) *out_written = length;
    return 0;
}

/**
 * @brief Invalidate a discovered disk's partition cache after excluding active mounts.
 * @param disk_id Borrowed registered whole-disk identifier.
 * @return Zero or negative ownership/discovery/protected-root/busy errno.
 * Storage execution ownership required; no namespace or module lock spans
 * preparation I/O. Actual volume memberships, including detached held ones,
 * keep the disk busy. Cache invalidation does not transfer device ownership.
 */
static int storage_disk_block_reread_owned(uint32_t disk_id)
{
    struct install_disk_state *disk;
    uint64_t sectors;
    int ret = storage_acquire_task_io();
    if (ret < 0) return ret;
    ret = disk_manage_prepare(disk_id, &disk, &sectors);
    if (ret < 0) return ret;
    if ((disk->boot_root && !storage_installer_root_active()) ||
        storage_disk_has_data_mount(disk_id)) {
        return -16;
    }
    storage_cache_invalidate();
    disk_block_cache_invalidate(disk_id);
    return 0;
}

/** @brief Retain the exact block/code identity for a complete disk transaction.
 * @param disk_id Stable inventory identity, never a native pointer.
 * @param partition_index GPT entry index or whole-device selector as supported.
 * @param out_first_lba Caller-owned operation argument/output; existing worker validates bounds.
 * @param out_sector_count Caller-owned operation argument/output; existing worker validates bounds.
 * @return Zero or existing validation/lookup/native errno, including EAGAIN.
 * Caller holds execution ownership. The token spans every GPT/data/cache step
 * and all worker exits, with no registry/module lock over native I/O. Failed
 * writes never release code/backing early or resolve a reloaded device identity.
 */
int storage_disk_block_info(uint32_t disk_id, int32_t partition_index,
                            uint64_t *out_first_lba, uint64_t *out_sector_count)
{
    struct block_device_ref ref={0};int rc=storage_disk_block_get(disk_id,&ref);
    if(!rc && (disk_id&STORAGE_BLOCK_OPTICAL_BIT)) {
        if(!out_first_lba || !out_sector_count || partition_index!=-1)rc=-22;
        else {
            struct block_device_info info;rc=block_device_info(&ref,&info);
            if(!rc && (info.block_size!=2048 || info.blocks>UINT64_MAX/4))rc=-75;
            if(!rc){*out_first_lba=0;*out_sector_count=info.blocks*4;}
        }
    } else if(!rc)rc=storage_disk_block_info_owned(disk_id,partition_index,out_first_lba,out_sector_count);
    block_device_put(&ref);return rc;
}

/** @brief Retain the exact block/code identity for a complete disk transaction.
 * @param disk_id Stable inventory identity, never a native pointer.
 * @param partition_index GPT entry index or whole-device selector as supported.
 * @param offset Caller-owned operation argument/output; existing worker validates bounds.
 * @param buffer Current readable/writable kernel extent as required by operation.
 * @param length Byte count bounded by the existing worker.
 * @param out_read Optional completed-byte output, cleared before admission.
 * @return Zero or existing validation/lookup/native errno, including EAGAIN.
 * Caller holds execution ownership. The token spans every GPT/data/cache step
 * and all worker exits, with no registry/module lock over native I/O. Failed
 * writes never release code/backing early or resolve a reloaded device identity.
 */
int storage_disk_block_read(uint32_t disk_id, int32_t partition_index,
                            uint64_t offset, void *buffer, uint32_t length,
                            uint32_t *out_read)
{
    if(out_read)*out_read=0;
    struct block_device_ref ref={0};int rc=storage_disk_block_get(disk_id,&ref);
    if(!rc && (disk_id&STORAGE_BLOCK_OPTICAL_BIT))
        rc=partition_index==-1 ? storage_optical_read(&ref,offset,buffer,length,out_read) : -22;
    else if(!rc)rc=storage_disk_block_read_owned(disk_id,partition_index,offset,buffer,length,out_read);
    block_device_put(&ref);return rc;
}

/** @brief Retain the exact block/code identity for a complete disk transaction.
 * @param disk_id Stable inventory identity, never a native pointer.
 * @param partition_index GPT entry index or whole-device selector as supported.
 * @param offset Caller-owned operation argument/output; existing worker validates bounds.
 * @param buffer Current readable/writable kernel extent as required by operation.
 * @param length Byte count bounded by the existing worker.
 * @param out_written Optional completed-byte output, cleared before admission.
 * @return Zero or existing validation/lookup/native errno, including EAGAIN.
 * Caller holds execution ownership. The token spans every GPT/data/cache step
 * and all worker exits, with no registry/module lock over native I/O. Failed
 * writes never release code/backing early or resolve a reloaded device identity.
 */
int storage_disk_block_write(uint32_t disk_id, int32_t partition_index,
                             uint64_t offset, const void *buffer, uint32_t length,
                             uint32_t *out_written)
{
    if(out_written)*out_written=0;
    struct block_device_ref ref={0};int rc=storage_disk_block_get(disk_id,&ref);
    if(!rc && (disk_id&STORAGE_BLOCK_OPTICAL_BIT))rc=-30;
    else if(!rc)rc=storage_disk_block_write_owned(disk_id,partition_index,offset,buffer,length,out_written);
    block_device_put(&ref);return rc;
}

/** @brief Retain the exact block/code identity for a complete disk transaction.
 * @param disk_id Stable inventory identity, never a native pointer.
 * @return Zero or existing validation/lookup/native errno, including EAGAIN.
 * Caller holds execution ownership. The token spans every GPT/data/cache step
 * and all worker exits, with no registry/module lock over native I/O. Failed
 * writes never release code/backing early or resolve a reloaded device identity.
 */
int storage_disk_block_reread(uint32_t disk_id)
{
    struct block_device_ref ref={0};int rc=storage_disk_block_get(disk_id,&ref);
    if(!rc)rc=storage_disk_block_reread_owned(disk_id);
    block_device_put(&ref);return rc;
}

/** @brief Retain the exact block/code identity for a complete disk transaction.
 * @param disk_id Stable inventory identity, never a native pointer.
 * @param partition_index GPT entry index or whole-device selector as supported.
 * @param uuid Caller-owned operation argument/output; existing worker validates bounds.
 * @return Zero or existing validation/lookup/native errno, including EAGAIN.
 * Caller holds execution ownership. The token spans every GPT/data/cache step
 * and all worker exits, with no registry/module lock over native I/O. Failed
 * writes never release code/backing early or resolve a reloaded device identity.
 */
int storage_disk_partition_uuid(uint32_t disk_id, uint32_t partition_index, char uuid[37])
{
    struct block_device_ref ref={0};int rc=storage_disk_block_get(disk_id,&ref);
    if(!rc)rc=storage_disk_partition_uuid_owned(disk_id,partition_index,uuid);
    block_device_put(&ref);return rc;
}
