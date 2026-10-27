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
