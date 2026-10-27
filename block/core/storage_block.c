/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/export.h>
#include "storage_internal.h"
#include <linux/errno.h>

/** @brief Transfer logical blocks through a held exact native device reference.
 * @param volume Borrowed instance retaining device_ref for the entire call.
 * @param lba First native logical block. @param count Nonzero native blocks.
 * @param buffer Stable current kernel buffer of count*block_size bytes.
 * @param write Whether buffer is readable write input instead of read output.
 * @return Zero or negative transport errno, preserving -EAGAIN for resumable
 * reads. Execution ownership required; transport lock spans commands but never
 * a registry lock. The complete extent is validated before its first callback.
 */
static int storage_transfer_blocks(const struct storage_volume *volume,uint64_t lba,
                                      uint32_t count,void *buffer,bool write)
{
    struct block_device_info info;
    int rc=block_device_info(&volume->device_ref,&info);if(rc)return rc;
    if(!count || lba>=info.blocks || count>info.blocks-lba)return -EINVAL;
    kernel_spin_lock(&storage_transport_lock);
    while(count) {
        uint32_t chunk=count<info.max_blocks ? count : info.max_blocks;
        rc=write ? block_device_write(&volume->device_ref,lba,chunk,buffer) :
            block_device_read(&volume->device_ref,lba,chunk,buffer);
        if(rc)break;
        count-=chunk;lba+=chunk;buffer=(uint8_t *)buffer+(size_t)chunk*info.block_size;
    }
    kernel_spin_unlock(&storage_transport_lock);return rc;
}

/** @brief Read 512-byte physical sectors from an owned disk or reserved RAM image.
 * @param volume Stable volume retaining device ownership, required.
 * @param lba First sector. @param sector_count Nonzero complete sectors.
 * @param buffer Current writable kernel extent sector_count*512 bytes.
 * @return Zero or negative bounds/transport errno including -EAGAIN unchanged.
 * Execution ownership required; DMA always belongs to the native provider.
 */
int storage_read_device(const struct storage_volume *volume,uint64_t lba,uint32_t sector_count,void *buffer)
{
    if(!volume || !buffer || !sector_count)return -EINVAL;
    struct block_device_info info;int rc=block_device_info(&volume->device_ref,&info);
    if(rc)return rc;if(info.block_size!=512)return -EOPNOTSUPP;
    rc=storage_transfer_blocks(volume,lba,sector_count,buffer,false);
    if(rc<0 && rc!=-RELIEFOS_EAGAIN)
        console_error("[storage] device read failed volume=%u kind=%u transport=%u lba=%llu sectors=%u ret=%d\n",
            volume->volume_id,volume->kind,volume->transport,(unsigned long long)lba,sector_count,rc);
    return rc;
}

/** @brief Invalidate aliases before writing physical sectors through owned transport.
 * @param volume Stable volume retaining device ownership, required.
 * @param lba First sector. @param sector_count Nonzero complete sectors.
 * @param buffer Current readable kernel extent sector_count*512 bytes.
 * @return Zero or negative bounds/RO/transport errno; partial hardware failure is
 * never advertised as complete. Execution ownership; mutations are synchronous
 * under the existing syscall replay policy, no registry lock spans hardware.
 */
int storage_write_device(const struct storage_volume *volume,uint64_t lba,uint32_t sector_count,const void *buffer)
{
    if(!volume || !buffer || !sector_count)return -EINVAL;
    storage_filesystem_cache_event(volume,lba,sector_count);
    struct block_device_info info;int rc=block_device_info(&volume->device_ref,&info);
    if(rc)return rc;if(info.block_size!=512)return -EOPNOTSUPP;
    return storage_transfer_blocks(volume,lba,sector_count,(void *)buffer,true);
}

/** @brief Read a bounded raw extent with temporary exact device/code ownership.
 * @param disk Borrowed inventory record, required. @param lba First sector.
 * @param sector_count Nonzero sectors. @param buffer Current writable bytes.
 * @return Zero or native errno; all exits drop the temporary token. Caller holds
 * execution ownership and any outer multi-I/O transaction/OFD lifetime pin.
 */
int storage_read_install_disk(const struct install_disk_state *disk,uint64_t lba,uint32_t sector_count,void *buffer)
{
    struct storage_volume volume={0};int rc=storage_volume_from_install_disk(&volume,disk);
    if(!rc)rc=storage_read_device(&volume,lba,sector_count,buffer);
    storage_volume_put_device(&volume);return rc;
}

/** @brief Write a bounded raw extent while retaining exact device/code ownership.
 * @param disk Borrowed inventory record, required. @param lba First sector.
 * @param sector_count Nonzero sectors. @param buffer Current readable bytes.
 * @return Zero or native errno; all exits drop the temporary token. Caller holds
 * execution ownership and the outer multi-I/O transaction/OFD lifetime pin.
 */
int storage_write_install_disk(const struct install_disk_state *disk,uint64_t lba,uint32_t sector_count,const void *buffer)
{
    struct storage_volume volume={0};int rc=storage_volume_from_install_disk(&volume,disk);
    if(!rc)rc=storage_write_device(&volume,lba,sector_count,buffer);
    storage_volume_put_device(&volume);return rc;
}

/**
 * @brief Read physical sectors through the currently selected volume token.
 * @param lba First physical 512-byte sector.
 * @param sector_count Nonzero complete sector count.
 * @param buffer Writable kernel extent of sector_count times 512 bytes, stable throughout I/O.
 * @return Zero or negative bounds/transport errno, preserving resumable -EAGAIN.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call. Uses the registered device and transport exclusion; no raw RAM backing or ownership transfer.
 */
int storage_read_sectors(uint64_t lba, uint32_t sector_count, void *buffer)
{
    return storage_read_device(&g_storage, lba, sector_count, buffer);
}

static uint32_t storage_gpt_crc32(const void *data, uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (uint32_t bit = 0; bit < 8u; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static int storage_gpt_guid_empty(const uint8_t guid[16])
{
    if (!guid) return 1;
    for (uint32_t i = 0; i < 16u; ++i) {
        if (guid[i] != 0) return 0;
    }
    return 1;
}

/* Validate the primary GPT before allocating or reading its entry array. The
 * implementation supports up to ReliefOS's fixed 128-entry in-memory limit,
 * but does not require the table itself to start at LBA 2: valid GPT tools
 * may choose another table position. */
static int storage_gpt_header_valid_for_root(const struct gpt_header *header)
{
    struct gpt_header copy;
    uint64_t table_bytes;
    uint64_t table_sectors;
    uint64_t table_last_lba;
    if (!header || header->signature != 0x5452415020494645ULL ||
        header->revision < 0x00010000u ||
        header->header_size != sizeof(struct gpt_header) ||
        header->reserved != 0 || header->current_lba != 1u ||
        header->backup_lba <= header->current_lba ||
        header->first_usable_lba < 2u ||
        header->first_usable_lba > header->last_usable_lba ||
        header->last_usable_lba >= header->backup_lba ||
        header->partition_entries_lba < 2u ||
        header->partition_entry_count == 0 ||
        header->partition_entry_count > GPT_ENTRY_COUNT ||
        header->partition_entry_size != sizeof(struct gpt_entry)) {
        return -22;
    }
    table_bytes = (uint64_t)header->partition_entry_count *
                  header->partition_entry_size;
    table_sectors = (table_bytes + SECTOR_SIZE - 1u) / SECTOR_SIZE;
    if (!table_sectors || table_bytes > sizeof(storage_cluster_buf) ||
        table_sectors > header->first_usable_lba - header->partition_entries_lba) {
        return -22;
    }
    table_last_lba = header->partition_entries_lba + table_sectors - 1u;
    if (table_last_lba >= header->first_usable_lba ||
        table_last_lba >= header->backup_lba) {
        return -22;
    }
    copy = *header;
    copy.header_crc32 = 0;
    return storage_gpt_crc32(&copy, header->header_size) == header->header_crc32
               ? 0 : -5;
}

static int storage_gpt_entries_valid_for_root(const struct gpt_header *header,
                                              const uint8_t *table)
{
    uint32_t count;
    if (!header || !table) return -22;
    count = header->partition_entry_count;
    for (uint32_t i = 0; i < count; ++i) {
        const struct gpt_entry *entry =
            (const struct gpt_entry *)(const void *)(table + (uint64_t)i *
                                                     sizeof(struct gpt_entry));
        if (storage_gpt_guid_empty(entry->type_guid)) continue;
        if (storage_gpt_guid_empty(entry->unique_guid) ||
            entry->first_lba < header->first_usable_lba ||
            entry->last_lba < entry->first_lba ||
            entry->last_lba > header->last_usable_lba) {
            return -22;
        }
        for (uint32_t j = 0; j < i; ++j) {
            const struct gpt_entry *other =
                (const struct gpt_entry *)(const void *)(table +
                    (uint64_t)j * sizeof(struct gpt_entry));
            if (!storage_gpt_guid_empty(other->type_guid) &&
                !(entry->last_lba < other->first_lba ||
                  entry->first_lba > other->last_lba)) {
                return -22;
            }
        }
    }
    return 0;
}

/* GPT names are UTF-16LE on disk, but partition editors commonly differ in
 * ASCII case.  Compare the canonical ReliefOS root label without making the
 * filesystem probe depend on a particular editor's spelling. */
static int storage_gpt_name_matches(const uint16_t name[36], const char *ascii)
{
    uint32_t i;
    if (!name || !ascii) {
        return 0;
    }
    for (i = 0; i < 36u && ascii[i]; ++i) {
        uint16_t ch = name[i];
        uint8_t expected = (uint8_t)ascii[i];
        if (ch >= 'a' && ch <= 'z') {
            ch = (uint16_t)(ch - ('a' - 'A'));
        }
        if (expected >= 'a' && expected <= 'z') {
            expected = (uint8_t)(expected - ('a' - 'A'));
        }
        if (ch != expected) {
            return 0;
        }
    }
    return ascii[i] == 0 && i < 36u && name[i] == 0;
}

/* A filesystem signature is authoritative; GPT type/name metadata is not.
 * Keep this probe deliberately small because it runs once per GPT entry and
 * the full exFAT boot-region validation is performed by exfat_mount(). */
static int storage_probe_exfat_signature(uint64_t lba)
{
    int ret = storage_read_sectors(lba, 1u, storage_scratch);
    if (ret < 0) {
        return ret;
    }
    return storage_memcmp(storage_scratch + 3u, "EXFAT   ", 8u) == 0 &&
           storage_scratch[510] == 0x55u && storage_scratch[511] == 0xaau;
}

static int storage_probe_fat32_signature(uint64_t lba)
{
    int ret = storage_read_sectors(lba, 1u, storage_scratch);
    if (ret < 0) {
        return ret;
    }
    return storage_scratch[510] == 0x55u && storage_scratch[511] == 0xaau &&
           storage_memcmp(storage_scratch + 82u, "FAT32   ", 8u) == 0;
}

/** @brief Read validated optical logical blocks through an owned block provider.
 * @param lba First 2048-byte optical block. @param block_count Nonzero blocks.
 * @param buffer Current writable extent block_count*2048 bytes.
 * @return Zero or native bounds/transport errno, preserving resumable -EAGAIN.
 * Caller owns selected volume/execution context; the held block reference
 * supports ISO modules loaded after the independent optical device was probed.
 */
int storage_read_iso_blocks(uint64_t lba,uint32_t block_count,void *buffer)
{
    if(!buffer || !block_count || g_storage.filesystem!=STORAGE_FILESYSTEM_ISO9660)return -EINVAL;
    if(g_storage.iso_sector_count && (lba>=g_storage.iso_sector_count || block_count>g_storage.iso_sector_count-lba))return -EIO;
    struct block_device_info info;int rc=block_device_info(&g_storage.device_ref,&info);
    if(rc)return rc;if(info.block_size!=2048)return -EOPNOTSUPP;
    return storage_transfer_blocks(&g_storage,lba,block_count,buffer,false);
}

/** @brief Write selected-volume sectors using the registered native batch limit.
 * @param lba First physical sector. @param sector_count Complete sector extent.
 * @param buffer Current readable sector_count*512 kernel bytes.
 * @return Zero or native errno; never replays a partly accepted mutation after
 * EAGAIN. Execution ownership; cache invalidation precedes all device writes,
 * provider/code and current buffer remain pinned through each synchronous step.
 */
int storage_write_sectors(uint64_t lba, uint32_t sector_count, const void *buffer)
{
    const uint8_t *src = (const uint8_t *)buffer;
    /* RAM-backed installer roots are a writable live environment. Changes
     * affect the in-memory FAT image only and disappear when it reboots. */
    /* From this point the caller may have changed filesystem metadata.  Do
     * not return EAGAIN and replay a partially completed mutation. */
    storage_begin_mutation();
    storage_sector_cache_invalidate();
    struct block_device_info info;int rc=block_device_info(&g_storage.device_ref,&info);
    if(rc)return rc;
    uint32_t limit=info.max_blocks;
    while (sector_count) {
        uint32_t chunk = min_u32(sector_count,limit);
        int ret = storage_write_device(&g_storage, lba, chunk, src);
        if (ret < 0 && chunk > 1u) {
            /* A few virtual AHCI implementations occasionally fail a
             * multi-sector DMA command even though the media is healthy.
             * Retry the same idempotent payload one sector at a time so a
             * filesystem transaction is not abandoned after a transient
             * controller error. */
            console_warn("[reliefnt] storage write batch fallback lba=%llu sectors=%u ret=%d\n",
                           (unsigned long long)lba, chunk, ret);
            ret = 0;
            for (uint32_t i = 0; i < chunk; ++i) {
                int one = storage_write_device(&g_storage, lba + i, 1u,
                                               src + (size_t)i * SECTOR_SIZE);
                if (one < 0) {
                    ret = one;
                    break;
                }
            }
        }
        if (ret < 0) {
            return ret;
        }
        lba += chunk;
        sector_count -= chunk;
        src += chunk * SECTOR_SIZE;
    }
    return 0;
}

int gpt_find_esp(void)
{
    struct gpt_header *hdr = (struct gpt_header *)(void *)storage_scratch;
    uint8_t esp_found = 0;
    uint8_t canonical_exfat_root_found = 0;
    uint8_t basic_data_candidate_found = 0;
    uint64_t basic_data_candidate_lba = 0;
    uint8_t fat32_candidate_found = 0;
    uint64_t fat32_candidate_lba = 0;
    int ret = storage_read_sectors(1, 1, storage_scratch);
    if (ret < 0) {
        return storage_read_failure(ret);
    }
    if (hdr->signature != 0x5452415020494645ULL) {
        return -2;
    }
    uint32_t count;
    uint32_t size;
    uint32_t total_bytes;
    uint32_t total_sectors;
    uint32_t table_crc;
    if (storage_gpt_header_valid_for_root(hdr) < 0) {
        console_error("[reliefnt] GPT primary header invalid\n");
        return -22;
    }
    count = hdr->partition_entry_count;
    size = hdr->partition_entry_size;
    console_info("[reliefnt] GPT found entries=%u entry_size=%u\n", count, size);
    total_bytes = count * size;
    total_sectors = (total_bytes + SECTOR_SIZE - 1u) / SECTOR_SIZE;
    uint64_t phys = mm_alloc_pages((total_sectors + 7u) / 8u);
    if (!phys) {
        return -12;
    }
    uint8_t *table = (uint8_t *)mm_phys_to_virt(phys);
    if (!table) {
        mm_free_pages(phys, (total_sectors + 7u) / 8u);
        return -12;
    }
    ret = storage_read_sectors(hdr->partition_entries_lba, total_sectors, table);
    if (ret < 0) {
        mm_free_pages(phys, (total_sectors + 7u) / 8u);
        return ret;
    }
    table_crc = storage_gpt_crc32(table, total_bytes);
    if (table_crc != hdr->partition_entries_crc32 ||
        storage_gpt_entries_valid_for_root(hdr, table) < 0) {
        mm_free_pages(phys, (total_sectors + 7u) / 8u);
        console_error("[reliefnt] GPT partition table invalid\n");
        return -5;
    }
    for (uint32_t i = 0; i < count; ++i) {
        struct gpt_entry *entry = (struct gpt_entry *)(void *)(table + (uint64_t)i * size);
        if (!entry->first_lba || entry->last_lba < entry->first_lba) {
            if (i < 3) {
                console_debug("[reliefnt] GPT entry %u: first=%llu last=%llu (skip)\n",
                               i, (unsigned long long)entry->first_lba,
                               (unsigned long long)entry->last_lba);
            }
            continue;
        }
        console_debug("[reliefnt] GPT entry %u: first=%llu last=%llu guid=%x%x%x%x\n",
                       i, (unsigned long long)entry->first_lba,
                       (unsigned long long)entry->last_lba,
                       (unsigned)entry->type_guid[0], (unsigned)entry->type_guid[1],
                       (unsigned)entry->type_guid[2], (unsigned)entry->type_guid[3]);
        if (storage_memcmp(entry->type_guid, esp_guid, 16) &&
            !storage_boot_root_accept_guid(entry->unique_guid)) continue;
        if (storage_memcmp(entry->type_guid, esp_guid, 16) == 0) {
            g_storage.esp_start_lba = entry->first_lba;
            g_storage.esp_sector_count = entry->last_lba - entry->first_lba + 1u;
            storage_memcpy(g_storage.gpt_disk_guid, hdr->disk_guid,
                           sizeof(g_storage.gpt_disk_guid));
            storage_memcpy(g_storage.esp_unique_guid, entry->unique_guid,
                           sizeof(g_storage.esp_unique_guid));
            g_storage.has_gpt_identity =
                storage_guid_valid(g_storage.gpt_disk_guid) &&
                storage_guid_valid(g_storage.esp_unique_guid);
            esp_found = 1;
            console_info("[reliefnt] GPT ESP found lba=%llu\n",
                           (unsigned long long)entry->first_lba);
        } else if (storage_memcmp(entry->type_guid, basic_data_guid, 16) == 0) {
            uint8_t is_exfat = 0;
            uint8_t canonical_name = 0;
            int signature_ret;
            console_info("[reliefnt] GPT basic data partition found lba=%llu name[0]=0x%04x\n",
                           (unsigned long long)entry->first_lba, (unsigned int)entry->name[0]);
            if (!basic_data_candidate_found) {
                basic_data_candidate_found = 1;
                basic_data_candidate_lba = entry->first_lba;
            }
            canonical_name = storage_gpt_name_matches(entry->name, "RELIEFOS_ROOT") ||
                             storage_gpt_name_matches(entry->name, "LEONOS4_ROOT");
            signature_ret = storage_probe_exfat_signature(entry->first_lba);
            is_exfat = signature_ret > 0;
            if (!fat32_candidate_found && storage_probe_fat32_signature(entry->first_lba) > 0) {
                fat32_candidate_found = 1;
                fat32_candidate_lba = entry->first_lba;
            }
            if (is_exfat && !canonical_name && !canonical_exfat_root_found) {
                console_debug("[reliefnt] GPT exFAT signature found lba=%llu\n",
                               (unsigned long long)entry->first_lba);
            }
            if (is_exfat && (canonical_name || !canonical_exfat_root_found)) {
                g_storage.exfat_start_lba = entry->first_lba;
                g_storage.exfat_sector_count = entry->last_lba - entry->first_lba + 1u;
                if (canonical_name) {
                    canonical_exfat_root_found = 1;
                }
                console_info("[reliefnt] GPT exFAT root found lba=%llu sectors=%u\n",
                               (unsigned long long)entry->first_lba,
                               (unsigned int)(entry->last_lba - entry->first_lba + 1u));
            }
        } else if (storage_memcmp(entry->type_guid, linux_filesystem_guid, 16) == 0) {
            g_storage.ext2_start_lba = entry->first_lba;
            g_storage.ext2_sector_count = entry->last_lba - entry->first_lba + 1u;
        }
    }
    /* A hand-created GPT may carry a non-standard type or a damaged label.
     * Probe every non-ESP extent before falling back to the first Basic Data
     * entry, so an exFAT root remains discoverable even when its GPT type was
     * edited incorrectly. */
    if (!g_storage.exfat_start_lba) {
        for (uint32_t i = 0; i < count; ++i) {
            struct gpt_entry *candidate =
                (struct gpt_entry *)(void *)(table + (uint64_t)i * size);
            if (!candidate->first_lba || candidate->last_lba < candidate->first_lba ||
                !storage_boot_root_accept_guid(candidate->unique_guid) ||
                storage_memcmp(candidate->type_guid, esp_guid, 16) == 0) {
                continue;
            }
            if (storage_probe_exfat_signature(candidate->first_lba) > 0) {
                g_storage.exfat_start_lba = candidate->first_lba;
                g_storage.exfat_sector_count =
                    candidate->last_lba - candidate->first_lba + 1u;
                console_debug("[reliefnt] GPT exFAT signature found lba=%llu type=non-basic\n",
                               (unsigned long long)candidate->first_lba);
                break;
            }
        }
    }
    /* Formatting a FAT32 ESP through the installer intentionally preserves
     * its bytes but some partition tools reset the GPT type to Basic Data.
     * Recover the ESP from its FAT32 boot signature when no ESP GUID was
     * present, so the root and boot partitions are still usable. */
    if (!esp_found && !fat32_candidate_found) {
        for (uint32_t i = 0; i < count; ++i) {
            struct gpt_entry *candidate =
                (struct gpt_entry *)(void *)(table + (uint64_t)i * size);
            if (!candidate->first_lba || candidate->last_lba < candidate->first_lba ||
                storage_memcmp(candidate->type_guid, esp_guid, 16) == 0) {
                continue;
            }
            if (storage_probe_fat32_signature(candidate->first_lba) > 0) {
                fat32_candidate_found = 1;
                fat32_candidate_lba = candidate->first_lba;
                break;
            }
        }
    }
    if (!esp_found && fat32_candidate_found) {
        g_storage.esp_start_lba = fat32_candidate_lba;
        for (uint32_t i = 0; i < count; ++i) {
            struct gpt_entry *candidate =
                (struct gpt_entry *)(void *)(table + (uint64_t)i * size);
            if (candidate->first_lba == fat32_candidate_lba &&
                candidate->last_lba >= candidate->first_lba) {
                g_storage.esp_sector_count =
                    candidate->last_lba - candidate->first_lba + 1u;
                break;
            }
        }
        esp_found = 1;
        console_debug("[reliefnt] GPT ESP signature found without ESP type lba=%llu\n",
                       (unsigned long long)fat32_candidate_lba);
    }
    /* If the signature itself is damaged, retain the first Basic Data extent
     * as a mount candidate so exfat_mount() can report the actual corruption.
     * Never silently mount the ESP as the system root. */
    if (!g_storage.exfat_start_lba && !g_storage.ext2_start_lba && basic_data_candidate_found) {
        g_storage.exfat_start_lba = basic_data_candidate_lba;
        for (uint32_t i = 0; i < count; ++i) {
            struct gpt_entry *candidate =
                (struct gpt_entry *)(void *)(table + (uint64_t)i * size);
            if (candidate->first_lba == basic_data_candidate_lba &&
                candidate->last_lba >= candidate->first_lba) {
                g_storage.exfat_sector_count =
                    candidate->last_lba - candidate->first_lba + 1u;
                break;
            }
        }
        console_warn("[reliefnt] GPT exFAT signature not confirmed; trying Basic Data root lba=%llu\n",
                       (unsigned long long)g_storage.exfat_start_lba);
    }
    /* Preserve the identities of the actual boot-selected extents. */
    for (uint32_t i = 0; i < count; ++i) {
        const struct gpt_entry *entry = (const void *)(table + (uint64_t)i * size);
        if (!entry->first_lba) continue;
        uint64_t device = storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(g_storage.source_disk_id, i));
        if (entry->first_lba == g_storage.esp_start_lba) {
            storage_memcpy(g_storage.esp_unique_guid, entry->unique_guid, 16);
            g_storage.esp_device = device;
        }
        if (entry->first_lba == g_storage.ext2_start_lba) {
            storage_memcpy(g_storage.ext2_unique_guid, entry->unique_guid, 16);
            g_storage.ext_device = device;
        }
        if (entry->first_lba == g_storage.exfat_start_lba) {
            storage_memcpy(g_storage.exfat_unique_guid, entry->unique_guid, 16);
            g_storage.exfat_device = device;
        }
    }
    mm_free_pages(phys, (total_sectors + 7u) / 8u);
    return esp_found ? 0 : -2;
}

/* Native filesystem modules share this core-owned interface; CRC/release
 * checks bind it to the exact kernel, without a stable cross-release API. */
EXPORT_SYMBOL(storage_read_device);
EXPORT_SYMBOL(storage_read_iso_blocks);
EXPORT_SYMBOL(storage_read_sectors);
EXPORT_SYMBOL(storage_write_device);
EXPORT_SYMBOL(storage_write_sectors);
