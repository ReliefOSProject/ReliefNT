#include "internal.h"



/** @brief Parse a bounded exFAT extent through its already owned transport.
 * @return Zero or negative metadata/I/O/cache errno. Caller selects an exclusive
 * unpublished g_storage context under execution ownership, with nonzero sector
 * count and a device pin. LBA zero is a valid whole-device start. No callback
 * lock spans I/O; failed construction is retired by the filesystem factory.
 */
int exfat_mount(void)
{
    int cache_status=storage_inode_purge_volume(g_storage.volume_id);
    if(cache_status)return cache_status;
    uint8_t boot[SECTOR_SIZE];
    uint8_t backup_boot[SECTOR_SIZE];
    uint64_t volume_length;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t heap_offset;
    uint8_t spc_shift;
    uint8_t bitmap_found = 0, upcase_found = 0, label_found = 0;
    uint64_t limit;
    int ret;
    if (!g_storage.exfat_sector_count || g_storage.exfat_start_lba>UINT64_MAX-g_storage.exfat_sector_count) return -2;
    /* A writable root must not be mounted from only one half of the exFAT
     * boot region. Validate both copies and require identical geometry; a
     * damaged backup is otherwise indistinguishable from a later metadata
     * update and can make recovery tools choose the wrong layout. */
    console_debug("[reliefnt] exfat_mount: validating boot region at lba=%llu\n",
                   (unsigned long long)g_storage.exfat_start_lba);
    ret = exfat_boot_region_valid(g_storage.exfat_start_lba, boot);
    if (ret < 0) {
        console_error("[reliefnt] exfat_mount: primary boot region invalid ret=%d\n", ret);
        return ret;
    }
    ret = exfat_boot_region_valid(g_storage.exfat_start_lba + 12u, backup_boot);
    if (ret < 0) {
        console_error("[reliefnt] exfat_mount: backup boot region invalid ret=%d\n", ret);
        return ret;
    }
    for (uint32_t off = 64u; off < 112u; ++off) {
        if (boot[off] != backup_boot[off]) {
            console_warn("[reliefnt] exfat_mount: boot/backup mismatch at offset %u\n", off);
            return -5;
        }
    }
    volume_length = exfat_get_u64(boot + 72u);
    fat_offset = storage_get_u32(boot + 80u);
    fat_length = storage_get_u32(boot + 84u);
    heap_offset = storage_get_u32(boot + 88u);
    cluster_count = storage_get_u32(boot + 92u);
    root_cluster = storage_get_u32(boot + 96u);
    spc_shift = boot[109u];
    if (boot[108u] != 9u || spc_shift > 8u || boot[110u] != 1u ||
        exfat_get_u16(boot + 104u) != 0x0100u || (boot[106u] & 1u) != 0u ||
        (boot[112u] != 0xffu && boot[112u] > 100u) ||
        volume_length == 0 || volume_length > g_storage.exfat_sector_count || fat_offset < 24u ||
        fat_length == 0 || heap_offset <= fat_offset ||
        (uint64_t)fat_offset + fat_length > heap_offset || cluster_count == 0 ||
        (uint64_t)heap_offset + ((uint64_t)cluster_count << spc_shift) > volume_length ||
        root_cluster < 2u || root_cluster - 2u >= cluster_count ||
        fat_length < (uint32_t)((((uint64_t)cluster_count + 2u) * 4u + SECTOR_SIZE - 1u) /
                                SECTOR_SIZE)) return -2;
    g_storage.bytes_per_sector = SECTOR_SIZE;
    g_storage.sectors_per_cluster = 1u << spc_shift;
    g_storage.cluster_bytes = SECTOR_SIZE << spc_shift;
    g_storage.exfat_fat_offset = fat_offset;
    g_storage.exfat_fat_length = fat_length;
    g_storage.exfat_cluster_heap_offset = heap_offset;
    g_storage.exfat_cluster_count = cluster_count;
    g_storage.exfat_root_cluster = root_cluster;
    g_storage.exfat_sectors_per_cluster_shift = spc_shift;
    g_storage.exfat_next_free_cluster = root_cluster + 1u;
    ret = exfat_read_sectors_resilient(g_storage.exfat_start_lba + fat_offset,
                                       1u, storage_scratch);
    if (ret < 0) {
        console_error("[reliefnt] exfat_mount: FAT reserved sector read failed ret=%d\n", ret);
        return ret;
    }
    if (storage_get_u32(storage_scratch) != 0xfffffff8u ||
        storage_get_u32(storage_scratch + 4u) != 0xffffffffu) {
        console_error("[reliefnt] exfat_mount: FAT reserved entries invalid\n");
        return -5;
    }
    ret = exfat_validate_fat_chain(root_cluster);
    if (ret < 0) {
        console_error("[reliefnt] exfat_mount: root FAT chain invalid cluster=%u\n", root_cluster);
        return ret;
    }
    limit = exfat_dir_entry_limit();
    for (uint64_t pos = 0; pos < limit; ++pos) {
        uint8_t entry[EXFAT_ENTRY_SIZE];
            ret = exfat_dir_read_entry(root_cluster, 0u, (uint32_t)pos, entry);
            if (ret < 0) {
                console_error("[reliefnt] exfat_mount: root directory read failed index=%u ret=%d\n",
                               (uint32_t)pos, ret);
                return ret;
            }
        if (entry[0] == 0u) break;
        if (entry[0] == 0x83u) {
            if (label_found || entry[1] > 11u) { console_error("[reliefnt] exfat_mount: invalid volume label entry\n"); return -5; }
            for (uint32_t byte = 2u + entry[1] * 2u; byte < EXFAT_ENTRY_SIZE; ++byte) {
                if (entry[byte] != 0u) { console_error("[reliefnt] exfat_mount: volume label padding nonzero\n"); return -5; }
            }
            label_found = 1u;
        } else if (entry[0] == EXFAT_ENTRY_BITMAP && !bitmap_found) {
            uint32_t cluster = storage_get_u32(entry + 20u);
            uint64_t length = exfat_get_u64(entry + 24u);
            if (entry[1] & (uint8_t)~1u) { console_error("[reliefnt] exfat_mount: bitmap flags invalid=%x\n", entry[1]); return -5; }
            if (!exfat_cluster_valid(cluster) ||
                length < ((uint64_t)cluster_count + 7u) / 8u ||
                length > (uint64_t)cluster_count * g_storage.cluster_bytes) { console_error("[reliefnt] exfat_mount: bitmap geometry invalid cluster=%u length=%llu\n", cluster, (unsigned long long)length); return -5; }
            g_storage.exfat_bitmap_cluster = cluster;
            g_storage.exfat_bitmap_length = length;
            ret = exfat_system_stream_mode(cluster, length, &g_storage.exfat_bitmap_nofat);
            if (ret < 0) {
                console_error("[reliefnt] exfat_mount: invalid allocation bitmap stream cluster=%u length=%llu\n",
                               cluster, (unsigned long long)length);
                return ret;
            }
            bitmap_found = 1;
        } else if (entry[0] == EXFAT_ENTRY_BITMAP) {
            return -5;
        } else if (entry[0] == EXFAT_ENTRY_UPCASE && !upcase_found) {
            uint32_t cluster = storage_get_u32(entry + 20u);
            uint64_t length = exfat_get_u64(entry + 24u);
            if (entry[1] != 0u || entry[2] != 0u || entry[3] != 0u) { console_error("[reliefnt] exfat_mount: upcase reserved fields invalid\n"); return -5; }
            if (!exfat_cluster_valid(cluster) || length < 2u ||
                length > (uint64_t)cluster_count * g_storage.cluster_bytes) { console_error("[reliefnt] exfat_mount: upcase geometry invalid cluster=%u length=%llu\n", cluster, (unsigned long long)length); return -5; }
            g_storage.exfat_upcase_checksum = storage_get_u32(entry + 4u);
            g_storage.exfat_upcase_cluster = cluster;
            g_storage.exfat_upcase_length = length;
            ret = exfat_system_stream_mode(cluster, length, &g_storage.exfat_upcase_nofat);
            if (ret < 0) {
                console_error("[reliefnt] exfat_mount: invalid upcase stream cluster=%u length=%llu\n",
                               cluster, (unsigned long long)length);
                return ret;
            }
            upcase_found = 1;
        } else if (entry[0] == EXFAT_ENTRY_UPCASE) {
            console_error("[reliefnt] exfat_mount: duplicate upcase system entry\n"); return -5;
        }
    }
    if (!bitmap_found || !upcase_found) {
        console_warn("[reliefnt] exfat_mount: missing system entries bitmap=%u upcase=%u\n",
                       bitmap_found, upcase_found);
        return -2;
    }
    {
        uint8_t allocated;
        if (exfat_read_bitmap_bit(root_cluster, &allocated) < 0 || !allocated) {
            console_error("[reliefnt] exfat_mount: root cluster not allocated cluster=%u\n", root_cluster);
            return -5;
        }
    }
    {
        uint64_t bitmap_clusters = (g_storage.exfat_bitmap_length + g_storage.cluster_bytes - 1u) /
                                   g_storage.cluster_bytes;
        uint64_t upcase_clusters = (g_storage.exfat_upcase_length + g_storage.cluster_bytes - 1u) /
                                   g_storage.cluster_bytes;
        uint64_t bitmap_bits = g_storage.exfat_bitmap_length * 8u;
        if (bitmap_clusters == 0u || upcase_clusters == 0u ||
            g_storage.exfat_bitmap_cluster == g_storage.exfat_root_cluster ||
            g_storage.exfat_upcase_cluster == g_storage.exfat_root_cluster ||
            g_storage.exfat_bitmap_cluster == g_storage.exfat_upcase_cluster ||
            (g_storage.exfat_bitmap_nofat &&
             g_storage.exfat_bitmap_cluster > UINT32_MAX - (uint32_t)(bitmap_clusters - 1u)) ||
            (g_storage.exfat_upcase_nofat &&
             g_storage.exfat_upcase_cluster > UINT32_MAX - (uint32_t)(upcase_clusters - 1u)) ||
            (g_storage.exfat_bitmap_nofat && g_storage.exfat_upcase_nofat &&
             g_storage.exfat_bitmap_cluster <= UINT32_MAX - (uint32_t)(bitmap_clusters - 1u) &&
             g_storage.exfat_upcase_cluster <= UINT32_MAX - (uint32_t)(upcase_clusters - 1u) &&
             g_storage.exfat_bitmap_cluster <=
                 g_storage.exfat_upcase_cluster + (uint32_t)(upcase_clusters - 1u) &&
            g_storage.exfat_upcase_cluster <=
                 g_storage.exfat_bitmap_cluster + (uint32_t)(bitmap_clusters - 1u))) {
            console_error("[reliefnt] exfat_mount: system stream geometry/alias invalid bitmap=%u upcase=%u\n",
                           g_storage.exfat_bitmap_cluster, g_storage.exfat_upcase_cluster);
            return -5;
        }
        /* System streams and the root directory must not alias one another;
         * aliasing would let a normal file write corrupt allocation metadata. */
        if (exfat_fat_chain_contains(root_cluster, g_storage.exfat_bitmap_cluster) == 1 ||
            exfat_fat_chain_contains(root_cluster, g_storage.exfat_upcase_cluster) == 1) {
            console_error("[reliefnt] exfat_mount: system stream aliases root directory\n");
            return -5;
        }
        for (uint64_t i = 0u; i < bitmap_clusters; ++i) {
            uint32_t bitmap_cluster;
            ret = exfat_cluster_at(g_storage.exfat_bitmap_cluster,
                                   g_storage.exfat_bitmap_nofat,
                                   i, &bitmap_cluster);
            if (ret < 0) {
                console_error("[reliefnt] exfat_mount: bitmap cluster chain invalid index=%u ret=%d\n",
                               (uint32_t)i, ret);
                return ret;
            }
            if (exfat_stream_contains_cluster(g_storage.exfat_upcase_cluster,
                                              g_storage.exfat_upcase_nofat,
                                              g_storage.exfat_upcase_length,
                                              bitmap_cluster) == 1) {
                console_error("[reliefnt] exfat_mount: bitmap overlaps upcase cluster=%u\n", bitmap_cluster);
                return -5;
            }
        }
        for (uint64_t i = 0u; i < upcase_clusters; ++i) {
            uint32_t upcase_cluster;
            ret = exfat_cluster_at(g_storage.exfat_upcase_cluster,
                                   g_storage.exfat_upcase_nofat,
                                   i, &upcase_cluster);
            if (ret < 0) {
                console_error("[reliefnt] exfat_mount: upcase cluster chain invalid index=%u ret=%d\n",
                               (uint32_t)i, ret);
                return ret;
            }
            {
                uint8_t allocated;
                ret = exfat_read_bitmap_bit(upcase_cluster, &allocated);
                if (ret < 0) {
                    console_error("[reliefnt] exfat_mount: upcase bitmap read failed cluster=%u ret=%d\n",
                                   upcase_cluster, ret);
                    return ret;
                }
                if (!allocated) {
                    console_error("[reliefnt] exfat_mount: upcase cluster unallocated cluster=%u\n", upcase_cluster);
                    return -5;
                }
            }
        }
        for (uint64_t i = 0u; i < bitmap_clusters; ++i) {
            uint32_t bitmap_cluster;
            ret = exfat_cluster_at(g_storage.exfat_bitmap_cluster,
                                   g_storage.exfat_bitmap_nofat,
                                   i, &bitmap_cluster);
            if (ret < 0) {
                console_error("[reliefnt] exfat_mount: bitmap cluster chain invalid (verify) index=%u ret=%d\n",
                               (uint32_t)i, ret);
                return ret;
            }
            {
                uint8_t allocated;
                ret = exfat_read_bitmap_bit(bitmap_cluster, &allocated);
                if (ret < 0) {
                    console_error("[reliefnt] exfat_mount: bitmap allocation read failed cluster=%u ret=%d\n",
                                   bitmap_cluster, ret);
                    return ret;
                }
                if (!allocated) {
                    console_error("[reliefnt] exfat_mount: bitmap cluster unallocated cluster=%u\n", bitmap_cluster);
                    return -5;
                }
            }
        }
        /* Bits past the end of the cluster heap are reserved and must be
         * clear.  This catches truncated or mis-sized allocation bitmaps. */
        if (bitmap_bits > (uint64_t)cluster_count) {
            for (uint64_t bit = cluster_count; bit < bitmap_bits; ++bit) {
                uint8_t value;
                ret = exfat_read_stream(g_storage.exfat_bitmap_cluster,
                                        g_storage.exfat_bitmap_nofat, bit / 8u,
                                        &value, 1u);
                if (ret < 0) return ret;
                if ((value & (uint8_t)(1u << (bit & 7u))) != 0u) {
                    console_error("[reliefnt] exfat_mount: bitmap has reserved bit=%llu set\n",
                                   (unsigned long long)bit);
                    return -5;
                }
            }
        }
    }
    storage_exfat_upcase_ready[g_storage.volume_id] = 0u;
    ret = exfat_load_upcase_table();
    if (ret < 0) {
        console_error("[reliefnt] exfat_mount: upcase table load/checksum failed ret=%d cluster=%u length=%llu\n",
                       ret,
                       g_storage.exfat_upcase_cluster,
                       (unsigned long long)g_storage.exfat_upcase_length);
        return ret;
    }
    g_storage.root_cluster = root_cluster;
    g_storage.filesystem = STORAGE_FILESYSTEM_EXFAT;
    return 0;
}
