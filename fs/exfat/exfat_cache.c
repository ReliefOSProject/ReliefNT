#include "internal.h"

static int exfat_transport_read(uint64_t lba, uint32_t sectors, void *buffer);

static int exfat_transport_write(uint64_t lba, uint32_t sectors,
                                 const void *buffer);

static int exfat_boot_checksum(uint64_t base_lba, uint32_t *out_checksum);

static int exfat_fat_cache_load(uint64_t lba);

static int exfat_bitmap_cache_load(uint64_t byte_offset);

uint8_t storage_exfat_upcase_ready[STORAGE_MAX_VOLUMES];

static uint8_t exfat_fat_cache_data[EXFAT_META_CACHE_SECTORS * SECTOR_SIZE]
    __attribute__((aligned(4096)));
static struct storage_volume *exfat_fat_cache_volume;
static uint64_t exfat_fat_cache_lba;
static uint32_t exfat_fat_cache_count;
static uint8_t exfat_fat_cache_valid;
static uint8_t exfat_bitmap_cache_data[EXFAT_META_CACHE_SECTORS * SECTOR_SIZE]
    __attribute__((aligned(4096)));
static struct storage_volume *exfat_bitmap_cache_volume;
static uint64_t exfat_bitmap_cache_offset;
static uint32_t exfat_bitmap_cache_bytes;
static uint8_t exfat_bitmap_cache_valid;
static uint8_t exfat_bitmap_cache_dirty;
struct exfat_chain_hint exfat_chain_hint;

struct storage_volume *exfat_dir_cache_volume;

uint32_t exfat_dir_cache_cluster;

uint64_t exfat_dir_cache_offset;

uint32_t exfat_dir_cache_bytes;

uint8_t exfat_dir_cache_valid;

static int exfat_transport_read(uint64_t lba, uint32_t sectors, void *buffer)
{
    bool saved_async = storage_io_async_context;
    bool saved_write_started = storage_io_write_started;
    int ret;
    /* exFAT requests are often chained (FAT -> directory -> payload).  A
     * single syscall retry must never replay only part of that chain while an
     * earlier AHCI command is still pending.  Force every metadata/data
     * request through the synchronous transport, even when the caller did not
     * come through the normal syscall async context (page faults and boot
     * helpers can enter here directly). */
    storage_io_async_context = false;
    storage_io_write_started = true;
    ret = storage_read_sectors(lba, sectors, buffer);
    storage_io_async_context = saved_async;
    storage_io_write_started = saved_write_started;
    return ret;
}

static int exfat_transport_write(uint64_t lba, uint32_t sectors,
                                 const void *buffer)
{
    bool saved_async = storage_io_async_context;
    bool saved_write_started = storage_io_write_started;
    int ret;
    storage_io_async_context = false;
    storage_io_write_started = true;
    ret = storage_write_sectors(lba, sectors, buffer);
    storage_io_async_context = saved_async;
    storage_io_write_started = saved_write_started;
    return ret;
}

int exfat_read_sectors_resilient(uint64_t lba, uint32_t sectors,
                                        uint8_t *data)
{
    if (!data || !sectors) return -22;
    while (sectors) {
        uint32_t chunk = min_u32(sectors, EXFAT_READ_BATCH_SECTORS);
        int ret = exfat_transport_read(lba, chunk, data);
        if (ret == -RELIEFOS_EAGAIN) return ret;
        if (ret < 0 && chunk > 1u) {
            console_warn("[reliefnt] exfat transport read batch failed lba=%llu sectors=%u ret=%d; retrying sectors\n",
                           (unsigned long long)lba, chunk, ret);
            ret = 0;
            for (uint32_t i = 0; i < chunk; ++i) {
                int one = exfat_transport_read(lba + i, 1u,
                                               data + (size_t)i * SECTOR_SIZE);
                if (one == -RELIEFOS_EAGAIN) return one;
                if (one < 0) {
                    console_error("[reliefnt] exfat transport read sector failed lba=%llu ret=%d\n",
                                   (unsigned long long)(lba + i), one);
                    return one;
                }
            }
        }
        if (ret < 0) return ret;
        lba += chunk;
        sectors -= chunk;
        data += (size_t)chunk * SECTOR_SIZE;
    }
    return 0;
}

int exfat_write_sectors_resilient(uint64_t lba, uint32_t sectors,
                                         const uint8_t *data)
{
    int ret;
    if (!data || !sectors) return -22;
    ret = exfat_transport_write(lba, sectors, data);
    if (ret >= 0 || sectors == 1u) return ret;
    console_warn("[reliefnt] exfat write batch fallback lba=%llu sectors=%u ret=%d\n",
                   (unsigned long long)lba, sectors, ret);
    for (uint32_t i = 0; i < sectors; ++i) {
        storage_memcpy(storage_scratch, data + (size_t)i * SECTOR_SIZE, SECTOR_SIZE);
        ret = exfat_transport_write(lba + i, 1u, storage_scratch);
        if (ret < 0) return ret;
    }
    return 0;
}

void exfat_cache_invalidate(void)
{
    /* A dirty bitmap only exists during a completed allocation/free operation;
     * flush it before dropping the cache so a volume switch cannot lose bits. */
    if (exfat_bitmap_cache_dirty) {
        (void)exfat_bitmap_flush();
    }
    exfat_fat_cache_valid = 0;
    exfat_fat_cache_volume = 0;
    exfat_fat_cache_count = 0;
    exfat_bitmap_cache_valid = 0;
    exfat_bitmap_cache_volume = 0;
    exfat_bitmap_cache_bytes = 0;
    exfat_bitmap_cache_dirty = 0;
    exfat_dir_cache_volume = 0;
    exfat_dir_cache_cluster = 0;
    exfat_dir_cache_offset = 0;
    exfat_dir_cache_bytes = 0;
    exfat_dir_cache_valid = 0;
    exfat_chain_hint.valid = 0;
}

int exfat_cluster_valid(uint32_t cluster)
{
    return cluster >= 2u && cluster - 2u < g_storage.exfat_cluster_count;
}

uint64_t exfat_cluster_lba(uint32_t cluster)
{
    return g_storage.exfat_start_lba + g_storage.exfat_cluster_heap_offset +
           (uint64_t)(cluster - 2u) * g_storage.sectors_per_cluster;
}

uint16_t exfat_checksum(const uint8_t *entries, uint32_t count)
{
    uint16_t sum = 0;
    uint32_t bytes = count * EXFAT_ENTRY_SIZE;
    for (uint32_t i = 0; i < bytes; ++i) {
        if (i == 2u || i == 3u) {
            continue;
        }
        sum = (uint16_t)((sum << 15) | (sum >> 1));
        sum = (uint16_t)(sum + entries[i]);
    }
    return sum;
}

static int exfat_boot_checksum(uint64_t base_lba, uint32_t *out_checksum)
{
    uint32_t sum = 0;
    if (!out_checksum) return -22;
    for (uint32_t sector = 0; sector < 11u; ++sector) {
        int ret = exfat_read_sectors_resilient(base_lba + sector, 1u,
                                               storage_scratch);
        if (ret < 0) {
            console_error("[reliefnt] exfat boot checksum read failed lba=%llu sector=%u ret=%d\n",
                           (unsigned long long)(base_lba + sector), sector, ret);
            return ret;
        }
        for (uint32_t byte = 0; byte < SECTOR_SIZE; ++byte) {
            if (sector == 0u && (byte == 106u || byte == 107u || byte == 112u)) {
                continue;
            }
            sum = (sum << 31) | (sum >> 1);
            sum += storage_scratch[byte];
        }
    }
    *out_checksum = sum;
    return 0;
}

int exfat_boot_region_valid(uint64_t base_lba, uint8_t boot_sector[SECTOR_SIZE])
{
    uint32_t checksum;
    int read_ret = exfat_read_sectors_resilient(base_lba, 1u, storage_scratch);
    if (read_ret == 0) {
        storage_memcpy(boot_sector, storage_scratch, SECTOR_SIZE);
    }
    console_debug("[reliefnt] exfat_boot_region_valid: storage_read_sectors(lba=%llu) returned %d\n",
                   (unsigned long long)base_lba, read_ret);
    if (read_ret < 0) {
        console_error("[reliefnt] exfat_boot_region_valid: read failed at lba=%llu\n",
                       (unsigned long long)base_lba);
        return read_ret;
    }
    console_debug("[reliefnt] exfat_boot_region_valid: signature bytes: %c%c%c%c%c%c%c%c\n",
                   boot_sector[3], boot_sector[4], boot_sector[5], boot_sector[6],
                   boot_sector[7], boot_sector[8], boot_sector[9], boot_sector[10]);
    if (storage_memcmp(boot_sector + 3u, "EXFAT   ", 8u) != 0) {
        console_debug("[reliefnt] exfat_boot_region_valid: signature mismatch\n");
        return -2;
    }
    if (boot_sector[510] != 0x55u || boot_sector[511] != 0xaau) {
        console_error("[reliefnt] exfat_boot_region_valid: boot signature invalid: %x %x\n",
                       boot_sector[510], boot_sector[511]);
        return -2;
    }
    /* Every extended boot sector in the region has its own signature.  The
     * final two sectors are reserved and must remain zero.  Checking these
     * bytes prevents accepting a boot checksum copied over a damaged region. */
    for (uint32_t sector = 1u; sector <= 10u; ++sector) {
        read_ret = exfat_read_sectors_resilient(base_lba + sector, 1u,
                                                storage_scratch);
        if (read_ret < 0) {
            console_error("[reliefnt] exfat_boot_region_valid: read failed at sector %u ret=%d\n",
                           sector, read_ret);
            return read_ret;
        }
        if (sector <= 8u) {
            if (storage_scratch[510] != 0x55u || storage_scratch[511] != 0xaau) {
                console_error("[reliefnt] exfat_boot_region_valid: extended boot sector %u signature invalid: %x %x\n",
                               sector, storage_scratch[510], storage_scratch[511]);
                return -5;
            }
        } else {
            for (uint32_t byte = 0; byte < SECTOR_SIZE; ++byte) {
                if (storage_scratch[byte] != 0u) {
                    console_info("[reliefnt] exfat_boot_region_valid: reserved sector %u not zero at byte %u\n",
                                   sector, byte);
                    return -5;
                }
            }
        }
    }
    read_ret = exfat_boot_checksum(base_lba, &checksum);
    if (read_ret < 0) {
        console_error("[reliefnt] exfat_boot_region_valid: checksum read failed ret=%d\n",
                       read_ret);
        return read_ret;
    }
    console_debug("[reliefnt] exfat_boot_region_valid: computed checksum = 0x%08x\n", checksum);
    read_ret = exfat_read_sectors_resilient(base_lba + 11u, 1u, storage_scratch);
    if (read_ret < 0) {
        console_error("[reliefnt] exfat_boot_region_valid: failed to read checksum sector ret=%d\n",
                       read_ret);
        return read_ret;
    }
    for (uint32_t off = 0; off < SECTOR_SIZE; off += 4u) {
        uint32_t stored = storage_get_u32(storage_scratch + off);
        if (stored != checksum) {
            console_warn("[reliefnt] exfat_boot_region_valid: checksum mismatch at offset %u: stored=0x%08x computed=0x%08x\n",
                           off, stored, checksum);
            return -5;
        }
    }
    console_debug("[reliefnt] exfat_boot_region_valid: all checks passed\n");
    return 0;
}

static int exfat_fat_cache_load(uint64_t lba)
{
    uint64_t fat_start = g_storage.exfat_start_lba + g_storage.exfat_fat_offset;
    uint64_t fat_end = fat_start + g_storage.exfat_fat_length;
    uint64_t first;
    uint32_t count;
    if (lba < fat_start || lba >= fat_end) return -5;
    if (exfat_fat_cache_valid && exfat_fat_cache_volume == g_active_volume &&
        lba >= exfat_fat_cache_lba &&
        lba < exfat_fat_cache_lba + exfat_fat_cache_count) return 0;
    first = fat_start + ((lba - fat_start) / EXFAT_META_CACHE_SECTORS) *
            EXFAT_META_CACHE_SECTORS;
    count = (uint32_t)min_u64(EXFAT_META_CACHE_SECTORS, fat_end - first);
    {
        int ret = exfat_read_sectors_resilient(first, count, exfat_fat_cache_data);
        if (ret < 0) {
            console_error("[reliefnt] exfat FAT cache read failed volume=%u lba=%llu sectors=%u ret=%d\n",
                           g_storage.volume_id, (unsigned long long)first, count, ret);
            exfat_fat_cache_valid = 0;
            exfat_fat_cache_volume = 0;
            return ret;
        }
    }
    exfat_fat_cache_lba = first;
    exfat_fat_cache_count = count;
    exfat_fat_cache_volume = g_active_volume;
    exfat_fat_cache_valid = 1;
    return 0;
}

int exfat_read_fat(uint32_t cluster, uint32_t *next)
{
    uint64_t lba;
    uint32_t offset;
    if (!next || !exfat_cluster_valid(cluster)) return -22;
    lba = g_storage.exfat_start_lba + g_storage.exfat_fat_offset +
          ((uint64_t)cluster * 4u) / SECTOR_SIZE;
    {
        int ret = exfat_fat_cache_load(lba);
        if (ret < 0) return ret;
    }
    offset = (cluster * 4u) % SECTOR_SIZE +
             (uint32_t)(lba - exfat_fat_cache_lba) * SECTOR_SIZE;
    if (offset + sizeof(uint32_t) > exfat_fat_cache_count * SECTOR_SIZE) return -5;
    *next = storage_get_u32(exfat_fat_cache_data + offset);
    return 0;
}

int exfat_write_fat(uint32_t cluster, uint32_t next)
{
    uint64_t lba;
    uint32_t offset;
    uint32_t sector_offset;
    if (!exfat_cluster_valid(cluster) ||
        (next < 2u && next != 0u) ||
        (next >= 2u && next < EXFAT_EOC && !exfat_cluster_valid(next))) return -22;
    lba = g_storage.exfat_start_lba + g_storage.exfat_fat_offset +
          ((uint64_t)cluster * 4u) / SECTOR_SIZE;
    {
        int ret = exfat_fat_cache_load(lba);
        if (ret < 0) return ret;
    }
    sector_offset = (uint32_t)(lba - exfat_fat_cache_lba);
    offset = sector_offset * SECTOR_SIZE + (cluster * 4u) % SECTOR_SIZE;
    if (offset + sizeof(uint32_t) > exfat_fat_cache_count * SECTOR_SIZE) return -5;
    storage_put_u32(exfat_fat_cache_data + offset, next);
    /* Write-through keeps crash behavior identical to the old path while the
     * read cache removes the redundant sector read for adjacent entries. */
    {
        int ret = exfat_write_sectors_resilient(
            lba, 1u, exfat_fat_cache_data + sector_offset * SECTOR_SIZE);
        if (ret < 0) {
            exfat_fat_cache_valid = 0;
            exfat_fat_cache_volume = 0;
            return ret;
        }
    }
    return 0;
}

int exfat_cluster_at(uint32_t first, uint8_t nofat, uint64_t index,
                            uint32_t *out_cluster)
{
    uint32_t cluster = first;
    uint64_t remaining = index;
    if (!out_cluster || !exfat_cluster_valid(first)) {
        return -22;
    }
    if (nofat) {
        if (index >= g_storage.exfat_cluster_count ||
            first > UINT32_MAX - (uint32_t)index ||
            !exfat_cluster_valid(first + (uint32_t)index)) {
            return -5;
        }
        *out_cluster = first + (uint32_t)index;
        return 0;
    }
    if (index > g_storage.exfat_cluster_count) {
        return -5;
    }
    if (!nofat && exfat_chain_hint.valid && exfat_chain_hint.volume == g_active_volume &&
        exfat_chain_hint.first == first && exfat_chain_hint.nofat == nofat &&
        index >= exfat_chain_hint.index) {
        cluster = exfat_chain_hint.cluster;
        remaining = index - exfat_chain_hint.index;
    }
    while (remaining--) {
        int ret = exfat_read_fat(cluster, &cluster);
        if (ret < 0) {
            return ret;
        }
        if (cluster < 2u || cluster >= EXFAT_EOC || !exfat_cluster_valid(cluster)) {
            return -5;
        }
    }
    *out_cluster = cluster;
    if (!nofat) {
        exfat_chain_hint.volume = g_active_volume;
        exfat_chain_hint.first = first;
        exfat_chain_hint.cluster = cluster;
        exfat_chain_hint.index = index;
        exfat_chain_hint.nofat = nofat;
        exfat_chain_hint.valid = 1u;
    }
    return 0;
}

int exfat_system_stream_mode(uint32_t first, uint64_t length, uint8_t *out_nofat)
{
    uint64_t count;
    uint32_t cluster;
    if (!out_nofat || !exfat_cluster_valid(first) || !length) return -22;
    count = (length + g_storage.cluster_bytes - 1u) / g_storage.cluster_bytes;
    if (count == 0 || count > g_storage.exfat_cluster_count) return -5;
    cluster = first;
    /* A system stream has no stream-extension flags.  For a one-cluster
     * stream the FAT value is inherently ambiguous, but either access mode
     * addresses the same first cluster.  Prefer the contiguous interpretation
     * when the following physical clusters are valid and the first FAT entry
     * is an end marker; this is what mkfs.exfat and the installer formatter
     * emit for bitmap/upcase extents. */
    if (count == 1u) {
        uint32_t next;
        int ret = exfat_read_fat(first, &next);
        if (ret < 0) return ret;
        if (next < EXFAT_EOC) {
            return -5;
        }
        *out_nofat = 1u;
        return 0;
    }
    for (uint64_t index = 1u; index < count; ++index) {
        uint32_t next;
        {
            int ret = exfat_read_fat(cluster, &next);
            if (ret < 0) return ret;
        }
        if (next >= EXFAT_EOC) {
            if (first > UINT32_MAX - (uint32_t)(count - 1u) ||
                !exfat_cluster_valid(first + (uint32_t)(count - 1u))) return -5;
            *out_nofat = 1u;
            return 0;
        }
        if (!exfat_cluster_valid(next)) return -5;
        /* A FAT loop would otherwise make a short system file appear valid
         * while repeatedly reading the same cluster.  The system streams are
         * small, so a bounded duplicate check is preferable to trusting the
         * chain blindly. */
        for (uint64_t prior = 0u; prior < index; ++prior) {
            uint32_t seen;
            {
                int ret = exfat_cluster_at(first, 0u, prior, &seen);
                if (ret < 0) return ret;
                if (seen == next) return -5;
            }
        }
        cluster = next;
    }
    {
        uint32_t next;
        {
            int ret = exfat_read_fat(cluster, &next);
            if (ret < 0) return ret;
            if (next < EXFAT_EOC) return -5;
        }
    }
    *out_nofat = 0u;
    return 0;
}

int exfat_validate_fat_chain(uint32_t first)
{
    uint32_t cluster = first;
    for (uint32_t count = 0u; count < g_storage.exfat_cluster_count; ++count) {
        uint32_t next;
        if (!exfat_cluster_valid(cluster)) return -5;
        {
            int ret = exfat_read_fat(cluster, &next);
            if (ret < 0) return ret;
        }
        if (next >= EXFAT_EOC) return 0;
        if (!exfat_cluster_valid(next)) return -5;
        cluster = next;
    }
    return -5;
}

int exfat_fat_chain_contains(uint32_t first, uint32_t wanted)
{
    uint32_t cluster = first;
    for (uint32_t count = 0u; count < g_storage.exfat_cluster_count; ++count) {
        uint32_t next;
        if (!exfat_cluster_valid(cluster)) return -5;
        {
            int ret = exfat_read_fat(cluster, &next);
            if (ret < 0) return ret;
        }
        if (cluster == wanted) return 1;
        if (next >= EXFAT_EOC) return 0;
        if (!exfat_cluster_valid(next)) return -5;
        cluster = next;
    }
    return -5;
}

int exfat_stream_contains_cluster(uint32_t first, uint8_t nofat,
                                          uint64_t length, uint32_t wanted)
{
    uint64_t count = (length + g_storage.cluster_bytes - 1u) / g_storage.cluster_bytes;
    if (!exfat_cluster_valid(wanted) || count == 0u || count > g_storage.exfat_cluster_count) {
        return -22;
    }
    for (uint64_t index = 0u; index < count; ++index) {
        uint32_t cluster;
        {
            int ret = exfat_cluster_at(first, nofat, index, &cluster);
            if (ret < 0) return ret;
        }
        if (cluster == wanted) return 1;
    }
    return 0;
}

int exfat_read_stream(uint32_t first, uint8_t nofat, uint64_t offset,
                             void *buffer, uint32_t len)
{
    uint8_t *dst = (uint8_t *)buffer;
    uint32_t done = 0;
    uint64_t cluster_index;
    uint32_t cluster;
    if ((!buffer && len) || (len && !exfat_cluster_valid(first)) ||
        offset > UINT64_MAX - len) {
        return -22;
    }
    if (!len) return 0;
    cluster_index = offset / g_storage.cluster_bytes;
    {
        int ret = exfat_cluster_at(first, nofat, cluster_index, &cluster);
        if (ret < 0) {
        console_error("[reliefnt] exfat stream cluster lookup failed first=%u nofat=%u index=%llu\n",
                       first, nofat, (unsigned long long)cluster_index);
            return ret;
        }
    }
    while (done < len) {
        uint64_t pos = offset + done;
        uint32_t cluster_offset = (uint32_t)(pos % g_storage.cluster_bytes);
        uint32_t take;
        uint32_t sector_offset;
        uint64_t lba;
        int ret;
        take = min_u32(len - done, g_storage.cluster_bytes - cluster_offset);
        sector_offset = cluster_offset % SECTOR_SIZE;
        lba = exfat_cluster_lba(cluster) + cluster_offset / SECTOR_SIZE;
        if (sector_offset) {
            uint32_t part = min_u32(take, SECTOR_SIZE - sector_offset);
            ret = exfat_read_sectors_resilient(lba, 1u, storage_scratch);
            if (ret < 0) return ret;
            storage_memcpy(dst + done, storage_scratch + sector_offset, part);
            done += part;
            /* A partial-sector read can consume the final bytes of a cluster.
             * Move along the FAT chain before the next iteration; otherwise a
             * 512-byte read straddling a 32 KiB boundary would restart at the
             * old cluster's offset zero and duplicate the previous cluster. */
            if (done < len && cluster_offset + part == g_storage.cluster_bytes) {
                uint32_t next;
                if (nofat) {
                    if (!exfat_cluster_valid(cluster + 1u)) {
                        console_error("[reliefnt] exfat stream contiguous chain ended first=%u cluster=%u done=%u len=%u\n",
                                       first, cluster, done, len);
                        return -5;
                    }
                    cluster = cluster + 1u;
                } else {
                    int fat_ret = exfat_read_fat(cluster, &next);
                    if (fat_ret < 0) {
                        console_error("[reliefnt] exfat stream FAT read failed first=%u cluster=%u done=%u len=%u ret=%d\n",
                                       first, cluster, done, len, fat_ret);
                        return fat_ret;
                    }
                    if (next < 2u || next >= EXFAT_EOC || !exfat_cluster_valid(next)) {
                        console_error("[reliefnt] exfat stream FAT chain ended first=%u cluster=%u done=%u len=%u next=%u\n",
                                       first, cluster, done, len, next);
                        return -5;
                    }
                    cluster = next;
                }
            }
            continue;
        }
        while (take >= SECTOR_SIZE) {
            uint32_t sectors = min_u32(take / SECTOR_SIZE,
                                        (uint32_t)(sizeof(storage_cluster_buf) / SECTOR_SIZE));
            /* The caller may be a Ring-3 buffer.  Transport DMA addresses are
             * physical addresses, while user pointers are process virtual
             * addresses and are not guaranteed to be contiguous.  Always DMA
             * into the aligned kernel scratch and copy out afterwards. */
            ret = exfat_read_sectors_resilient(lba, sectors, storage_cluster_buf);
            if (ret < 0) return ret;
            storage_memcpy(dst + done, storage_cluster_buf, sectors * SECTOR_SIZE);
            lba += sectors;
            done += sectors * SECTOR_SIZE;
            take -= sectors * SECTOR_SIZE;
        }
        if (take) {
            ret = exfat_read_sectors_resilient(lba, 1u, storage_scratch);
            if (ret < 0) return ret;
            storage_memcpy(dst + done, storage_scratch, take);
            done += take;
        }
        if (done < len) {
            uint32_t next;
            if (nofat) {
                if (!exfat_cluster_valid(cluster + 1u)) {
                    console_error("[reliefnt] exfat stream contiguous chain ended first=%u cluster=%u done=%u len=%u\n",
                                   first, cluster, done, len);
                    return -5;
                }
                cluster = cluster + 1u;
            } else {
                int fat_ret = exfat_read_fat(cluster, &next);
                if (fat_ret < 0) {
                    console_error("[reliefnt] exfat stream FAT read failed first=%u cluster=%u done=%u len=%u ret=%d\n",
                                   first, cluster, done, len, fat_ret);
                    return fat_ret;
                }
                if (next < 2u || next >= EXFAT_EOC || !exfat_cluster_valid(next)) {
                    console_error("[reliefnt] exfat stream FAT chain ended first=%u cluster=%u done=%u len=%u next=%u\n",
                                   first, cluster, done, len, next);
                    return -5;
                }
                cluster = next;
            }
        }
    }
    return 0;
}

int exfat_write_stream(uint32_t first, uint8_t nofat, uint64_t offset,
                              const void *buffer, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t done = 0;
    uint64_t cluster_index;
    uint32_t cluster;
    if ((!buffer && len) || (len && !exfat_cluster_valid(first)) ||
        offset > UINT64_MAX - len) {
        return -22;
    }
    if (!len) return 0;
    cluster_index = offset / g_storage.cluster_bytes;
    {
        int ret = exfat_cluster_at(first, nofat, cluster_index, &cluster);
        if (ret < 0) return ret;
    }
    while (done < len) {
        uint64_t pos = offset + done;
        uint32_t cluster_offset = (uint32_t)(pos % g_storage.cluster_bytes);
        uint32_t take;
        uint32_t sector_offset;
        uint64_t lba;
        int ret;
        take = min_u32(len - done, g_storage.cluster_bytes - cluster_offset);
        sector_offset = cluster_offset % SECTOR_SIZE;
        lba = exfat_cluster_lba(cluster) + cluster_offset / SECTOR_SIZE;
        if (sector_offset) {
            uint32_t part = min_u32(take, SECTOR_SIZE - sector_offset);
            ret = exfat_read_sectors_resilient(lba, 1u, storage_scratch);
            if (ret < 0) {
                console_error("[reliefnt] exfat stream read-modify-read failed first=%u nofat=%u lba=%llu ret=%d\n",
                               first, nofat, (unsigned long long)lba, ret);
                return ret;
            }
            storage_memcpy(storage_scratch + sector_offset, src + done, part);
            ret = exfat_write_sectors_resilient(lba, 1u, storage_scratch);
            if (ret < 0) {
                console_error("[reliefnt] exfat stream read-modify-write failed first=%u nofat=%u lba=%llu ret=%d\n",
                               first, nofat, (unsigned long long)lba, ret);
                return ret;
            }
            done += part;
            /* Keep the write path consistent with reads: a partial-sector
             * update may finish a cluster, so follow the chain before the
             * next iteration instead of restarting at the old cluster. */
            if (done < len && cluster_offset + part == g_storage.cluster_bytes) {
                uint32_t next;
                if (nofat) {
                    if (!exfat_cluster_valid(cluster + 1u)) return -5;
                    cluster = cluster + 1u;
                } else {
                    int fat_ret = exfat_read_fat(cluster, &next);
                    if (fat_ret < 0) return fat_ret;
                    if (next < 2u || next >= EXFAT_EOC || !exfat_cluster_valid(next)) {
                        return -5;
                    }
                    cluster = next;
                }
            }
            continue;
        }
        while (take >= SECTOR_SIZE) {
            /* Keep each filesystem transaction within the transport's proven
             * AHCI request size.  The larger staging buffer remains useful,
             * while the generic block layer handles the request atomically. */
            uint32_t sectors = min_u32(
                min_u32(take / SECTOR_SIZE, STORAGE_WRITE_MAX_SECTORS),
                (uint32_t)(sizeof(storage_cluster_buf) / SECTOR_SIZE));
            /* As with reads, stage writes so a user virtual buffer is never
             * interpreted as a device physical address. */
            storage_memcpy(storage_cluster_buf, src + done, sectors * SECTOR_SIZE);
            ret = exfat_write_sectors_resilient(lba, sectors, storage_cluster_buf);
            if (ret < 0) {
                console_error("[reliefnt] exfat stream bulk write failed first=%u nofat=%u lba=%llu sectors=%u done=%u ret=%d\n",
                               first, nofat, (unsigned long long)lba, sectors, done, ret);
                return ret;
            }
            lba += sectors;
            done += sectors * SECTOR_SIZE;
            take -= sectors * SECTOR_SIZE;
        }
        if (take) {
            ret = exfat_read_sectors_resilient(lba, 1u, storage_scratch);
            if (ret < 0) {
                console_error("[reliefnt] exfat stream tail read failed first=%u nofat=%u lba=%llu ret=%d\n",
                               first, nofat, (unsigned long long)lba, ret);
                return ret;
            }
            storage_memcpy(storage_scratch, src + done, take);
            ret = exfat_write_sectors_resilient(lba, 1u, storage_scratch);
            if (ret < 0) {
                console_error("[reliefnt] exfat stream tail write failed first=%u nofat=%u lba=%llu ret=%d\n",
                               first, nofat, (unsigned long long)lba, ret);
                return ret;
            }
            done += take;
        }
        if (done < len) {
            uint32_t next;
            if (nofat) {
                if (!exfat_cluster_valid(cluster + 1u)) return -5;
                cluster = cluster + 1u;
            } else {
                int fat_ret = exfat_read_fat(cluster, &next);
                if (fat_ret < 0) return fat_ret;
                if (next < 2u || next >= EXFAT_EOC || !exfat_cluster_valid(next)) {
                    return -5;
                }
            }
            if (!nofat) cluster = next;
        }
    }
    return 0;
}

static int exfat_bitmap_cache_load(uint64_t byte_offset)
{
    uint64_t aligned;
    uint64_t end;
    uint32_t bytes;
    if (byte_offset >= g_storage.exfat_bitmap_length) return -5;
    if (exfat_bitmap_cache_valid && exfat_bitmap_cache_volume == g_active_volume &&
        byte_offset >= exfat_bitmap_cache_offset &&
        byte_offset < exfat_bitmap_cache_offset + exfat_bitmap_cache_bytes) return 0;
    if (exfat_bitmap_cache_dirty) {
        int ret = exfat_bitmap_flush();
        if (ret < 0) return ret;
    }
    aligned = (byte_offset / SECTOR_SIZE) * SECTOR_SIZE;
    end = min_u64(g_storage.exfat_bitmap_length,
                  aligned + EXFAT_META_CACHE_SECTORS * SECTOR_SIZE);
    bytes = (uint32_t)(end - aligned);
    {
        int ret = exfat_read_stream(g_storage.exfat_bitmap_cluster, g_storage.exfat_bitmap_nofat,
                                    aligned, exfat_bitmap_cache_data, bytes);
        if (ret < 0) {
            console_error("[reliefnt] exfat bitmap cache read failed volume=%u offset=%llu bytes=%u ret=%d\n",
                           g_storage.volume_id, (unsigned long long)aligned, bytes, ret);
            exfat_bitmap_cache_valid = 0;
            exfat_bitmap_cache_volume = 0;
            return ret;
        }
    }
    exfat_bitmap_cache_offset = aligned;
    exfat_bitmap_cache_bytes = bytes;
    exfat_bitmap_cache_volume = g_active_volume;
    exfat_bitmap_cache_valid = 1;
    exfat_bitmap_cache_dirty = 0;
    return 0;
}

int exfat_bitmap_flush(void)
{
    if (!exfat_bitmap_cache_valid || !exfat_bitmap_cache_dirty) return 0;
    {
        int ret = exfat_write_stream(g_storage.exfat_bitmap_cluster, g_storage.exfat_bitmap_nofat,
                                     exfat_bitmap_cache_offset, exfat_bitmap_cache_data,
                                     exfat_bitmap_cache_bytes);
        if (ret < 0) {
            console_error("[reliefnt] exfat bitmap flush failed volume=%u offset=%llu bytes=%u ret=%d\n",
                           g_storage.volume_id,
                           (unsigned long long)exfat_bitmap_cache_offset,
                           exfat_bitmap_cache_bytes, ret);
            return ret;
        }
    }
    exfat_bitmap_cache_dirty = 0;
    return 0;
}

int exfat_bitmap_get_cached(uint32_t cluster, uint8_t *allocated)
{
    uint64_t bit;
    uint64_t byte;
    if (!allocated || !exfat_cluster_valid(cluster)) return -22;
    bit = cluster - 2u;
    byte = bit / 8u;
    if (byte >= g_storage.exfat_bitmap_length) return -5;
    {
        int ret = exfat_bitmap_cache_load(byte);
        if (ret < 0) return ret;
    }
    *allocated = (exfat_bitmap_cache_data[byte - exfat_bitmap_cache_offset] >>
                  (bit & 7u)) & 1u;
    return 0;
}

int exfat_bitmap_set_cached(uint32_t cluster, uint8_t allocated)
{
    uint64_t bit;
    uint64_t byte;
    uint8_t *value;
    if (!exfat_cluster_valid(cluster)) return -22;
    bit = cluster - 2u;
    byte = bit / 8u;
    if (byte >= g_storage.exfat_bitmap_length) return -5;
    {
        int ret = exfat_bitmap_cache_load(byte);
        if (ret < 0) return ret;
    }
    value = &exfat_bitmap_cache_data[byte - exfat_bitmap_cache_offset];
    if (allocated) *value |= (uint8_t)(1u << (bit & 7u));
    else *value &= (uint8_t)~(1u << (bit & 7u));
    exfat_bitmap_cache_dirty = 1;
    return 0;
}

int exfat_read_bitmap_bit(uint32_t cluster, uint8_t *allocated)
{
    return exfat_bitmap_get_cached(cluster, allocated);
}

int exfat_write_contiguous_fat(uint32_t first, uint32_t count, uint32_t next_after)
{
    if (!count || !exfat_cluster_valid(first) ||
        first > UINT32_MAX - (count - 1u) ||
        !exfat_cluster_valid(first + count - 1u)) return -22;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t next = i + 1u < count ? first + i + 1u : next_after;
        int ret = exfat_write_fat(first + i, next);
        if (ret < 0) return ret;
    }
    return 0;
}
