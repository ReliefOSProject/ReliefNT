#include "internal.h"

static int exfat_zero_stream_range(uint32_t first, uint8_t nofat,
                                   uint64_t offset, uint64_t length);

static uint8_t exfat_zero_data[64u * SECTOR_SIZE]
    __attribute__((aligned(4096)));
int exfat_read_node(const struct storage_node *node, uint64_t offset,
                           void *buffer, uint32_t len, uint32_t *out_read)
{
    if (out_read) *out_read = 0;
    if (!node || !buffer || node->type != RELIEFOS_FS_TYPE_FILE ||
        node->volume_id != g_storage.volume_id) return -22;
    if (offset >= node->size || len == 0) return 0;
    if (len > node->size - offset) len = (uint32_t)(node->size - offset);
    {
        int ret = exfat_read_stream(node->first_cluster,
                                    (node->flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                    offset, buffer, len);
        if (ret < 0) return ret;
    }
    if (out_read) *out_read = len;
    return 0;
}

static int exfat_zero_stream_range(uint32_t first, uint8_t nofat,
                                   uint64_t offset, uint64_t length)
{
    /* Use a dedicated zero buffer rather than storage_scratch: the stream
     * writer uses storage_scratch for read-modify-write, so passing the same
     * buffer would corrupt a partial-sector zero fill. */
    storage_memzero(exfat_zero_data, sizeof(exfat_zero_data));
    while (length) {
        uint32_t chunk = (uint32_t)min_u64(length, sizeof(exfat_zero_data));
        int ret = exfat_write_stream(first, nofat, offset, exfat_zero_data, chunk);
        if (ret < 0) return ret;
        offset += chunk;
        length -= chunk;
    }
    return 0;
}

int exfat_zero_clusters(uint32_t first, uint8_t nofat, uint32_t count)
{
    storage_memzero(storage_scratch, sizeof(storage_scratch));
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t cluster;
        int ret = exfat_cluster_at(first, nofat, i, &cluster);
        if (ret < 0) return ret;
        for (uint32_t sector = 0; sector < g_storage.sectors_per_cluster; ) {
            uint32_t take = min_u32(g_storage.sectors_per_cluster - sector, STORAGE_SCRATCH_SECTORS);
            ret = exfat_write_sectors_resilient(exfat_cluster_lba(cluster) + sector,
                                                take, storage_scratch);
            if (ret < 0) return ret;
            sector += take;
        }
    }
    return 0;
}

int exfat_allocate_clusters(uint32_t count, uint32_t *out_first, uint8_t *out_nofat)
{
    /* Cluster numbers are 2..cluster_count+1 inclusive.  Keep the upper
     * bound in this form, but never probe the sentinel beyond the heap. */
    uint32_t max = g_storage.exfat_cluster_count + 1u;
    uint32_t start = g_storage.exfat_next_free_cluster;
    uint32_t found = 0;
    if (!out_first || !out_nofat || count == 0 || count > FAT32_MAX_FILE_CLUSTERS) return -22;
    if (start < 2u || start > max) start = 2u;
    /* Complete a previous metadata transaction before beginning a new scan. */
    {
        int ret = exfat_bitmap_flush();
        if (ret < 0) return ret;
    }
    /* Prefer a contiguous extent so normal writes retain the exFAT NoFatChain
     * fast path. The second pass gives fragmented volumes a usable FAT chain. */
    for (uint32_t pass = 0; pass < 2u; ++pass) {
        uint32_t begin = pass ? 2u : start;
        uint32_t end = pass ? (start > 2u ? start - 1u : 1u) : max;
        uint32_t run = 0;
        for (uint32_t cluster = begin; cluster <= end; ++cluster) {
            if (!exfat_cluster_valid(cluster)) break;
            uint8_t allocated = 1;
            {
                int ret = exfat_read_bitmap_bit(cluster, &allocated);
                if (ret < 0) return ret;
            }
            if (!allocated) {
                if (++run == count) {
                    found = cluster + 1u - count;
                    goto contiguous;
                }
            } else run = 0;
        }
    }
contiguous:
    if (found) {
        for (uint32_t i = 0; i < count; ++i) {
            {
                int ret = exfat_bitmap_set_cached(found + i, 1u);
                if (ret < 0) return ret;
            }
        }
        {
            int ret = exfat_bitmap_flush();
            if (ret < 0) return ret;
        }
        g_storage.exfat_next_free_cluster = found + count <= max ? found + count : 2u;
        *out_first = found;
        *out_nofat = 1u;
        return 0;
    }
    /* Fragmented fallback. Existing FAT pages are updated before an entry set
     * names them, so a crash can leak space but cannot expose a corrupt file. */
    for (uint32_t cluster = 2u; cluster <= max && found < count; ++cluster) {
        if (!exfat_cluster_valid(cluster)) break;
        uint8_t allocated = 1;
        {
            int ret = exfat_read_bitmap_bit(cluster, &allocated);
            if (ret < 0) return ret;
        }
        if (!allocated) storage_new_chain[found++] = cluster;
    }
    if (found != count) return -28;
    for (uint32_t i = 0; i < count; ++i) {
        {
            int ret = exfat_bitmap_set_cached(storage_new_chain[i], 1u);
            if (ret < 0) return ret;
            ret = exfat_write_fat(storage_new_chain[i],
                                  i + 1u < count ? storage_new_chain[i + 1u] : EXFAT_EOC);
            if (ret < 0) return ret;
        }
    }
    {
        int ret = exfat_bitmap_flush();
        if (ret < 0) return ret;
    }
    *out_first = storage_new_chain[0];
    *out_nofat = 0u;
    return 0;
}

int exfat_free_clusters(uint32_t first, uint8_t nofat, uint64_t bytes)
{
    uint64_t count;
    if (!first || !bytes) return 0;
    if (!exfat_cluster_valid(first)) return -5;
    count = (bytes + g_storage.cluster_bytes - 1u) / g_storage.cluster_bytes;
    if (count > g_storage.exfat_cluster_count ||
        count > FAT32_MAX_FILE_CLUSTERS) return -5;
    {
        int ret = exfat_bitmap_flush();
        if (ret < 0) return ret;
    }
    /* Snapshot the complete chain before changing either the bitmap or FAT.
     * Looking up index N from the first cluster after clearing FAT[N-1]
     * traverses a chain that we have already destroyed; that used to make
     * truncating/deleting any multi-cluster file fail after its first cluster
     * and leave the remaining clusters allocated.  A later append could then
     * reuse only the first cluster and create a malformed (sometimes looping)
     * chain, which is particularly visible with large WAD files. */
    for (uint64_t i = 0u; i < count; ++i) {
        uint32_t cluster;
        if (nofat) {
            if (first > UINT32_MAX - (uint32_t)i ||
                !exfat_cluster_valid(first + (uint32_t)i)) {
                return -5;
            }
            cluster = first + (uint32_t)i;
        } else {
            uint32_t next = EXFAT_EOC;
            if (i == 0u) {
                cluster = first;
            } else {
                cluster = storage_new_chain[i - 1u];
                if (exfat_read_fat(cluster, &next) < 0 ||
                    next < 2u || next >= EXFAT_EOC ||
                    !exfat_cluster_valid(next)) {
                    return -5;
                }
                cluster = next;
            }
        }
        for (uint64_t prior = 0u; prior < i; ++prior) {
            if (storage_new_chain[prior] == cluster) return -5;
        }
        storage_new_chain[i] = cluster;
        if (!nofat && i + 1u < count) {
            uint32_t next;
            if (exfat_read_fat(cluster, &next) < 0 ||
                next < 2u || next >= EXFAT_EOC ||
                !exfat_cluster_valid(next)) {
                return -5;
            }
        }
    }
    for (uint64_t i = 0; i < count; ++i) {
        uint32_t cluster = storage_new_chain[i];
        int ret = exfat_bitmap_set_cached(cluster, 0u);
        if (ret < 0) return ret;
        if (!nofat) {
            ret = exfat_write_fat(cluster, 0u);
            if (ret < 0) return ret;
        }
    }
    {
        int ret = exfat_bitmap_flush();
        if (ret < 0) return ret;
    }
    if (first < g_storage.exfat_next_free_cluster) g_storage.exfat_next_free_cluster = first;
    return 0;
}

int exfat_write_file(const char *path, const void *buffer, uint32_t len)
{
    char parent_path[RELIEFOS_FS_PATH_LEN];
    char name[RELIEFOS_FS_NAME_LEN];
    struct storage_node parent;
    struct storage_node old;
    struct exfat_dir_ref parent_ref;
    struct exfat_dir_ref old_ref;
    uint32_t first = 0;
    uint8_t nofat = 0;
    uint8_t existing;
    uint32_t clusters = len ? (len + g_storage.cluster_bytes - 1u) / g_storage.cluster_bytes : 0;
    int ret;
    if ((!buffer && len) || !path) return -22;
    ret = storage_parent_path(path, parent_path, sizeof(parent_path), name, sizeof(name));
    if (ret < 0) return ret;
    ret = exfat_lookup_path_ref(parent_path, &parent, &parent_ref);
    if (ret < 0) {
        console_error("[reliefnt] exfat write file parent lookup failed path=%s parent=%s ret=%d\n",
                       path, parent_path, ret);
        return ret;
    }
    if (parent.type != RELIEFOS_FS_TYPE_DIR) return -20;
    ret = exfat_find_in_dir_ref(parent.first_cluster,
                                (parent.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                name, &old, &old_ref);
    if (ret != 0 && ret != -2) {
        console_error("[reliefnt] exfat write file lookup failed path=%s parent=%s name=%s ret=%d\n",
                       path, parent_path, name, ret);
    }
    if (ret == 0 && old.type != RELIEFOS_FS_TYPE_FILE) return -21;
    if (ret != 0 && ret != -2) return ret;
    existing = ret == 0;
    storage_begin_mutation();
    if (clusters) {
        ret = exfat_allocate_clusters(clusters, &first, &nofat);
        if (ret < 0) return ret;
        /* The file payload covers almost every sector. Writing a full zeroed
         * extent first doubled installer I/O; only clear the invisible tail
         * after the payload so later in-place growth cannot expose stale data. */
        ret = exfat_write_stream(first, nofat, 0, buffer, len);
        if (ret < 0 ||
            ((uint64_t)clusters * g_storage.cluster_bytes > len &&
             (ret = exfat_zero_stream_range(first, nofat, len,
                                            (uint64_t)clusters * g_storage.cluster_bytes - len)) < 0)) {
            (void)exfat_free_clusters(first, nofat, len);
            console_error("[reliefnt] exfat write file payload failed path=%s first=%u nofat=%u len=%u ret=%d\n",
                           path, first, nofat, len, ret);
            return ret;
        }
    }
    if (existing) {
        ret = exfat_update_entry_data(&old_ref, first, len, nofat);
        if (ret == 0) ret = exfat_free_clusters(old.first_cluster,
                                                 (old.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                                 old.size);
    } else {
        ret = exfat_create_entry(parent.first_cluster,
                                 (parent.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                 (parent.flags & STORAGE_NODE_FLAG_ROOT) ? 0 : &parent_ref,
                                 name, 0u, first, len, nofat, 0);
    }
    if (ret < 0 && first) (void)exfat_free_clusters(first, nofat, len);
    if (ret == 0) storage_cache_invalidate();
    return ret;
}

int exfat_write_node_path(const char *path, uint64_t offset,
                                 const void *buffer, uint32_t len, uint32_t *out_written)
{
    struct storage_node node;
    struct exfat_dir_ref ref;
    uint64_t length;
    uint32_t old_clusters;
    uint32_t new_clusters;
    uint32_t old_tail = 0;
    uint32_t new_first = 0;
    uint32_t add_clusters;
    uint32_t new_tail = 0;
    uint8_t old_nofat;
    uint8_t new_nofat;
    uint8_t final_nofat;
    uint8_t cached_append;
    int ret;
    if (out_written) *out_written = 0;
    if (!path || (!buffer && len)) return -22;
    ret = exfat_lookup_path_ref(path, &node, &ref);
    if (ret < 0 || node.type != RELIEFOS_FS_TYPE_FILE) return ret < 0 ? ret : -21;
    length = offset + len;
    if (length < offset || length > 0xffffffffULL || node.size > 0xffffffffULL) return -28;
    if (length < node.size) length = node.size;
    if (!len) return 0;
    old_nofat = (node.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0;
    old_clusters = node.size ? (uint32_t)((node.size + g_storage.cluster_bytes - 1u) /
                                          g_storage.cluster_bytes) : 0u;
    new_clusters = length ? (uint32_t)((length + g_storage.cluster_bytes - 1u) /
                                       g_storage.cluster_bytes) : 0u;
    if (new_clusters > FAT32_MAX_FILE_CLUSTERS) return -28;

    storage_begin_mutation();
    cached_append = offset == node.size && node.first_cluster >= 2u &&
                    storage_write_chain_cache.valid &&
                    storage_write_chain_cache.volume == g_active_volume &&
                    storage_write_chain_cache.first_cluster == node.first_cluster &&
                    storage_write_chain_cache.size == (uint32_t)node.size &&
                    storage_text_eq(storage_write_chain_cache.path, path) &&
                    storage_write_chain_cache.count == old_clusters;
    if (old_clusters && cached_append) {
        old_tail = storage_write_chain_cache.tail;
    } else if (old_clusters) {
        if (exfat_cluster_at(node.first_cluster, old_nofat, old_clusters - 1u, &old_tail) < 0) {
            return -5;
        }
    }

    final_nofat = old_nofat;
    new_first = node.first_cluster;
    if (new_clusters > old_clusters) {
        add_clusters = new_clusters - old_clusters;
        ret = exfat_allocate_clusters(add_clusters, &new_first, &new_nofat);
        if (ret < 0) {
            console_error("[reliefnt] exfat write path=%s stage=allocate old_clusters=%u new_clusters=%u count=%u ret=%d\n",
                           path, old_clusters, new_clusters, add_clusters, ret);
            return ret;
        }
        if (!old_clusters) {
            final_nofat = new_nofat;
        } else if (old_nofat && new_nofat &&
                   node.first_cluster <= UINT32_MAX - old_clusters &&
                   new_first == node.first_cluster + old_clusters) {
            /* The allocator found the physical continuation of a contiguous
             * NoFatChain extent, so no FAT conversion is needed. */
            new_first = node.first_cluster;
            final_nofat = 1u;
        } else {
            /* A fragmented append must become a normal FAT chain. Existing
             * contiguous clusters and a newly contiguous extent are both
             * materialized only once, at the point where the representation
             * changes. */
            if (old_nofat) {
                ret = exfat_write_contiguous_fat(node.first_cluster, old_clusters, new_first);
            } else {
                ret = exfat_write_fat(old_tail, new_first);
            }
            if (ret < 0) {
                console_error("[reliefnt] exfat write path=%s stage=link-old-chain first=%u tail=%u new=%u ret=%d\n",
                               path, node.first_cluster, old_tail, new_first, ret);
                return ret;
            }
            if (new_nofat) {
                ret = exfat_write_contiguous_fat(new_first, add_clusters, EXFAT_EOC);
                if (ret < 0) {
                    console_error("[reliefnt] exfat write path=%s stage=link-new-chain first=%u count=%u ret=%d\n",
                                   path, new_first, add_clusters, ret);
                    return ret;
                }
            }
            final_nofat = 0u;
            new_first = node.first_cluster;
        }
    }
    if (!new_first && new_clusters) return -5;

    /* A write past EOF creates a zero-filled hole. Newly allocated tail bytes
     * are also cleared so unwritten portions never expose stale disk data. */
    if (offset > node.size &&
        (ret = exfat_zero_stream_range(new_first, final_nofat, node.size,
                                       offset - node.size)) < 0) {
        console_error("[reliefnt] exfat write path=%s stage=hole-zero first=%u nofat=%u offset=%llu length=%llu ret=-5\n",
                       path, new_first, final_nofat, (unsigned long long)node.size,
                       (unsigned long long)(offset - node.size));
        return ret;
    }
    ret = exfat_write_stream(new_first, final_nofat, offset, buffer, len);
    if (ret < 0) {
        console_error("[reliefnt] exfat write path=%s stage=data first=%u nofat=%u offset=%llu length=%u ret=%d\n",
                       path, new_first, final_nofat, (unsigned long long)offset, len, ret);
        return ret;
    }
    if (new_clusters > old_clusters) {
        uint64_t data_end = offset + len;
        uint64_t allocated_bytes = (uint64_t)new_clusters * g_storage.cluster_bytes;
        if (data_end < allocated_bytes &&
            (ret = exfat_zero_stream_range(new_first, final_nofat, data_end,
                                           allocated_bytes - data_end)) < 0) {
            console_error("[reliefnt] exfat write path=%s stage=tail-zero first=%u nofat=%u offset=%llu length=%llu ret=%d\n",
                           path, new_first, final_nofat, (unsigned long long)data_end,
                           (unsigned long long)(allocated_bytes - data_end), ret);
            return ret;
        }
    }
    ret = exfat_update_entry_data(&ref, new_first, length, final_nofat);
    if (ret < 0) {
        console_error("[reliefnt] exfat write path=%s stage=dir-update first=%u nofat=%u size=%llu ret=%d\n",
                       path, new_first, final_nofat, (unsigned long long)length, ret);
        return ret;
    }
    if (new_clusters) {
        if (final_nofat) {
            if (new_first > UINT32_MAX - (new_clusters - 1u)) return -5;
            new_tail = new_first + new_clusters - 1u;
        } else if (exfat_cluster_at(new_first, 0u, new_clusters - 1u, &new_tail) < 0) {
            return -5;
        }
    }
    storage_cache_invalidate();
    storage_write_chain_cache.volume = g_active_volume;
    storage_write_chain_cache.first_cluster = new_first;
    storage_write_chain_cache.size = (uint32_t)length;
    storage_write_chain_cache.count = new_clusters;
    storage_write_chain_cache.tail = new_tail;
    storage_copy_text(storage_write_chain_cache.path,
                      sizeof(storage_write_chain_cache.path), path);
    storage_write_chain_cache.valid = 1u;
    if (!final_nofat && new_clusters) {
        exfat_chain_hint.volume = g_active_volume;
        exfat_chain_hint.first = new_first;
        exfat_chain_hint.cluster = new_tail;
        exfat_chain_hint.index = new_clusters - 1u;
        exfat_chain_hint.nofat = 0u;
        exfat_chain_hint.valid = 1u;
    }
    if (out_written) *out_written = len;
    return 0;
}

int exfat_truncate_file(const char *path, uint64_t length)
{
    struct storage_node node;
    struct exfat_dir_ref ref;
    uint32_t old_clusters;
    uint32_t new_clusters;
    uint8_t nofat;
    int ret;
    if (!path || length > 0xffffffffULL) return -22;
    ret = exfat_lookup_path_ref(path, &node, &ref);
    if (ret < 0 || node.type != RELIEFOS_FS_TYPE_FILE) return ret < 0 ? ret : -21;
    if (length == node.size) return 0;
    old_clusters = node.size ? (uint32_t)((node.size + g_storage.cluster_bytes - 1u) /
                                          g_storage.cluster_bytes) : 0u;
    new_clusters = length ? (uint32_t)((length + g_storage.cluster_bytes - 1u) /
                                       g_storage.cluster_bytes) : 0u;
    if (new_clusters > FAT32_MAX_FILE_CLUSTERS) return -28;
    nofat = (node.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0;
    if (length > node.size) {
        uint64_t remaining = length - node.size;
        uint64_t offset = node.size;
        storage_memzero(exfat_zero_data, sizeof(exfat_zero_data));
        while (remaining) {
            uint32_t chunk = (uint32_t)min_u64(remaining, sizeof(exfat_zero_data));
            uint32_t written = 0;
            ret = exfat_write_node_path(path, offset, exfat_zero_data, chunk, &written);
            if (ret < 0 || written != chunk) return ret < 0 ? ret : -5;
            offset += written;
            remaining -= written;
        }
        return 0;
    }

    storage_begin_mutation();
    if (new_clusters == 0u) {
        /* Hide the old chain from the namespace before returning its space. */
        ret = exfat_update_entry_data(&ref, 0u, 0u, 0u);
        if (ret < 0) return ret;
        ret = exfat_free_clusters(node.first_cluster, nofat, node.size);
    } else if (new_clusters < old_clusters) {
        uint32_t tail;
        uint32_t first = node.first_cluster;
        if (!exfat_cluster_valid(first)) return -5;
        if (nofat) {
            if (first > UINT32_MAX - new_clusters) return -5;
            tail = first + new_clusters;
            ret = exfat_update_entry_data(&ref, first, length, 1u);
            if (ret == 0) ret = exfat_free_clusters(tail, 1u,
                                                     (uint64_t)(old_clusters - new_clusters) *
                                                         g_storage.cluster_bytes);
        } else {
            uint32_t next;
            ret = exfat_cluster_at(first, 0u, new_clusters - 1u, &tail);
            if (ret < 0) return ret;
            ret = exfat_read_fat(tail, &next);
            if (ret < 0) return ret;
            if (next < 2u || next >= EXFAT_EOC || !exfat_cluster_valid(next)) return -5;
            ret = exfat_update_entry_data(&ref, first, length, 0u);
            if (ret == 0 && exfat_write_fat(tail, EXFAT_EOC) < 0) ret = -5;
            if (ret == 0) ret = exfat_free_clusters(next, 0u,
                                                     (uint64_t)(old_clusters - new_clusters) *
                                                         g_storage.cluster_bytes);
        }
    } else {
        /* Only the logical file size changes; the final cluster remains part
         * of the stream and its existing bytes are intentionally preserved. */
        ret = exfat_update_entry_data(&ref, node.first_cluster, length, nofat);
    }
    if (ret == 0) {
        storage_cache_invalidate();
        storage_write_chain_cache.valid = 0;
    }
    return ret;
}
