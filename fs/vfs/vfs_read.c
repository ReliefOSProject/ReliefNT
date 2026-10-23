#include "internal.h"

static int storage_read_node_cursor_unlocked(const struct storage_node *node, uint64_t offset,
                             void *buf, uint32_t len, uint32_t *out_read,
                             struct storage_read_cursor *cursor);


static int storage_read_node_cursor_unlocked(const struct storage_node *node, uint64_t offset,
                             void *buf, uint32_t len, uint32_t *out_read,
                             struct storage_read_cursor *cursor)
{
    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0;
    struct storage_volume *old_volume = 0;
    int ret;
    if (cursor && (!cursor->valid || cursor->offset != offset || cursor->cluster < 2u)) {
        cursor->valid = 0;
    }
    if (out_read) {
        *out_read = 0;
    }
    if (!node || !buf) {
        return -22;
    }
    struct storage_node refreshed = *node;
    if (node->flags & (STORAGE_NODE_FLAG_EXT2 | STORAGE_NODE_FLAG_TMPFS)) {
        ret = storage_inode_refresh(&refreshed);
        if (ret < 0) return ret;
        node = &refreshed;
    }
    if (node->type == RELIEFOS_FS_TYPE_DEVICE && (node->flags & STORAGE_NODE_FLAG_DEV_FB0)) {
        return -21;
    }
    if (node->type != RELIEFOS_FS_TYPE_FILE) {
        return -21;
    }
    if (offset >= node->size || len == 0) {
        if (cursor) {
            cursor->valid = 0;
        }
        return 0;
    }
    ret = storage_select_node_volume(node, &old_volume);
    if (ret < 0) {
        return ret;
    }
    if (len > node->size - offset) {
        len = (uint32_t)(node->size - offset);
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_TMPFS) {
        ret = tmpfs_read(g_storage.tmpfs, node->first_cluster, offset, buf, len, out_read);
        storage_restore_volume(old_volume);
        return ret;
    }

    if (g_storage.filesystem == STORAGE_FILESYSTEM_ISO9660) {
        if (cursor) {
            cursor->valid = 0;
        }
        uint64_t absolute = (uint64_t)node->first_cluster * ISO9660_BLOCK_SIZE + offset;
        uint32_t done = 0;
        while (done < len) {
            uint64_t block = absolute / ISO9660_BLOCK_SIZE;
            uint32_t block_offset = (uint32_t)(absolute % ISO9660_BLOCK_SIZE);
            uint32_t take = min_u32(ISO9660_BLOCK_SIZE - block_offset, len - done);
            ret = storage_read_iso_blocks(block, 1, storage_scratch);
            if (ret < 0) {
                storage_restore_volume(old_volume);
                return storage_read_failure(ret);
            }
            storage_memcpy(dst + done, storage_scratch + block_offset, take);
            absolute += take;
            done += take;
        }
        if (out_read) {
            *out_read = done;
        }
        storage_restore_volume(old_volume);
        return 0;
    }

    if (storage_ext4_is_ext_family(&g_storage)) {
        if (cursor) {
            cursor->valid = 0;
        }
        ret = storage_ext4_ops.read(&g_storage, node, offset, buf, len, out_read);
        storage_restore_volume(old_volume);
        return ret;
    }

    if (g_storage.filesystem == STORAGE_FILESYSTEM_EXFAT) {
        if (cursor) cursor->valid = 0;
        ret = exfat_read_node(node, offset, buf, len, out_read);
        storage_restore_volume(old_volume);
        return ret;
    }

    uint32_t cluster = node->first_cluster;
    uint64_t skip_clusters = offset / g_storage.cluster_bytes;
    uint32_t cluster_off = (uint32_t)(offset % g_storage.cluster_bytes);
    /* FAT32 directory/file reads alternate between the installer root and a
     * mounted /target or /target/boot volume. A cached cluster cursor can
     * survive a volume switch and turn a valid fragmented file into EIO.
     * FAT cursors are an optimization only; re-walk the chain for each
     * syscall slice until the read cache can be keyed by volume as well. */
    if (cursor) cursor->valid = 0;
    if (cursor && cursor->valid) {
        cluster = cursor->cluster;
    } else {
        while (skip_clusters--) {
            uint32_t next = 0;
            ret = fat32_read_fat_entry(cluster, &next);
            if (ret < 0) {
                storage_restore_volume(old_volume);
                return storage_read_failure(ret);
            }
            if (next >= FAT32_EOC || next < 2u) {
                console_error("[storage] fat skip invalid cluster=%u next=%u off=%llu\n",
                               cluster, next, (unsigned long long)offset);
                storage_restore_volume(old_volume);
                return -5;
            }
            cluster = next;
        }
    }

    while (done < len) {
        uint64_t cluster_file_offset = offset + done - cluster_off;
        uint64_t bytes_from_cluster = node->size - cluster_file_offset;
        /* Include the leading offset because cache fills are cluster-aligned. */
        uint64_t bytes_requested = len - done + cluster_off;
        uint32_t max_clusters;
        uint32_t cache_offset;
        uint32_t cache_clusters;
        uint32_t next;
        uint32_t take;
        if (bytes_from_cluster > bytes_requested) {
            bytes_from_cluster = bytes_requested;
        }
        max_clusters = (uint32_t)((bytes_from_cluster +
                                   g_storage.cluster_bytes - 1u) /
                                  g_storage.cluster_bytes);
        if (max_clusters > STORAGE_READAHEAD_SECTORS / g_storage.sectors_per_cluster) {
            max_clusters = STORAGE_READAHEAD_SECTORS / g_storage.sectors_per_cluster;
        }
        if (storage_read_cache_lookup(cluster, &cache_offset, &cache_clusters)) {
            if (cache_clusters > max_clusters) {
                cache_clusters = max_clusters;
            }
            ret = fat32_read_fat_entry(cluster + cache_clusters - 1u, &next);
            if (ret < 0) {
                storage_restore_volume(old_volume);
                return storage_read_failure(ret);
            }
        } else {
            ret = storage_read_contiguous_clusters(cluster, max_clusters,
                                                   &cache_clusters, &next);
            if (ret < 0) {
                storage_restore_volume(old_volume);
                return storage_read_failure(ret);
            }
            ret = storage_read_cache_fill(cluster, cache_clusters);
            if (ret < 0) {
                storage_restore_volume(old_volume);
                return storage_read_failure(ret);
            }
            cache_offset = 0;
        }
        take = cache_clusters * g_storage.cluster_bytes - cluster_off;
        if (take > len - done) {
            take = len - done;
        }
        storage_memcpy(dst + done, storage_read_cache_data + cache_offset + cluster_off,
                       take);
        done += take;
        if (done >= len) {
            if (cursor && offset + done < node->size) {
                uint32_t consumed = cluster_off + take;
                uint32_t advanced = consumed / g_storage.cluster_bytes;
                uint32_t next_cluster = cluster + advanced;
                if ((consumed % g_storage.cluster_bytes) == 0u &&
                    advanced >= cache_clusters) {
                    next_cluster = next;
                }
                if (next_cluster < 2u || next_cluster >= FAT32_EOC) {
                    console_error("[storage] fat cursor invalid cluster=%u next=%u next_cluster=%u advanced=%u cache_clusters=%u consumed=%u off=%llu size=%llu\n",
                                   cluster, next, next_cluster, advanced, cache_clusters,
                                   consumed, (unsigned long long)offset,
                                   (unsigned long long)node->size);
                    cursor->valid = 0;
                    storage_restore_volume(old_volume);
                    return -5;
                }
                cursor->offset = offset + done;
                cursor->cluster = next_cluster;
                cursor->valid = 1;
            } else if (cursor) {
                cursor->valid = 0;
            }
            break;
        }
        if (next >= FAT32_EOC) {
            console_error("[storage] fat mid eoc cluster=%u next=%u done=%u len=%u off=%llu size=%llu\n",
                           cluster, next, done, len, (unsigned long long)offset,
                           (unsigned long long)node->size);
            if (cursor) {
                cursor->valid = 0;
            }
            storage_restore_volume(old_volume);
            return -5;
        }
        if (next < 2u) {
            console_error("[storage] fat mid bad cluster=%u next=%u done=%u len=%u off=%llu size=%llu\n",
                           cluster, next, done, len, (unsigned long long)offset,
                           (unsigned long long)node->size);
            if (cursor) {
                cursor->valid = 0;
            }
            storage_restore_volume(old_volume);
            return -5;
        }
        if (cursor) {
            cursor->offset = offset + done;
            cursor->cluster = next;
            cursor->valid = 1;
        }
        cluster = next;
        cluster_off = 0;
    }
    if (out_read) {
        *out_read = done;
    }
    storage_restore_volume(old_volume);
    return 0;
}

int storage_read_node(const struct storage_node *node, uint64_t offset,
                      void *buf, uint32_t len, uint32_t *out_read)
{
    return storage_read_node_cursor(node, offset, buf, len, out_read, NULL);
}

int storage_read_node_cursor(const struct storage_node *node, uint64_t offset,
                             void *buf, uint32_t len, uint32_t *out_read,
                             struct storage_read_cursor *cursor)
{
    int ret;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    ret = storage_read_node_cursor_unlocked(node, offset, buf, len, out_read, cursor);
    kernel_execution_unlock_irqrestore(flags);
    return ret;
}

int storage_read_file(const char *path, const void **out_data, size_t *out_len)
{
    struct storage_node node;
    struct storage_volume *old_volume = g_active_volume;
    uint64_t pages;
    uint64_t phys;
    void *data;
    uint32_t got = 0;
    if (!out_data || !out_len) {
        return -22;
    }
    *out_data = 0;
    *out_len = 0;
    int ret = storage_lookup_path(path, &node);
    if (ret < 0) {
        storage_restore_volume(old_volume);
        return ret;
    }
    if (node.type != RELIEFOS_FS_TYPE_FILE) {
        storage_restore_volume(old_volume);
        return -21;
    }
    /* storage_read_node() and the page allocator use 32-bit lengths. Do not
     * silently truncate a larger FAT32 file into a short executable/image. */
    if (node.size > 0xffffffffULL ||
        (node.size + 4095ULL) / 4096ULL > 0xffffffffULL) {
        storage_restore_volume(old_volume);
        return -28;
    }
    pages = (node.size + 4095u) / 4096u;
    phys = pages ? mm_alloc_pages((uint32_t)pages) : mm_alloc_page();
    if (!phys) {
        storage_restore_volume(old_volume);
        return -12;
    }
    data = mm_phys_to_virt(phys);
    if (!data) {
        mm_free_pages(phys, pages ? (uint32_t)pages : 1u);
        storage_restore_volume(old_volume);
        return -12;
    }
    ret = storage_read_node(&node, 0, data, (uint32_t)node.size, &got);
    if (ret < 0 || got != node.size) {
        mm_free_pages(phys, pages ? (uint32_t)pages : 1u);
        storage_restore_volume(old_volume);
        return ret < 0 ? ret : -5;
    }
    *out_data = data;
    *out_len = node.size;
    storage_restore_volume(old_volume);
    return 0;
}
