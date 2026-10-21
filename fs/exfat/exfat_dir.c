#include "internal.h"

static int exfat_dir_cache_load(uint32_t directory_cluster, uint8_t nofat,
                                uint64_t offset);

static int exfat_dir_write_entry(uint32_t directory_cluster, uint8_t nofat,
                                 uint32_t index, const uint8_t entry[EXFAT_ENTRY_SIZE]);

static void exfat_dir_cache_merge_sector(uint32_t directory_cluster, uint8_t nofat,
                                         uint64_t sector_offset,
                                         const uint8_t sector[SECTOR_SIZE]);

static int exfat_read_file_set(uint32_t directory_cluster, uint8_t nofat, uint32_t index,
                               uint8_t entries[20][EXFAT_ENTRY_SIZE], uint8_t *out_count,
                               uint16_t name[255], uint32_t *out_name_length,
                               struct storage_node *out_node);

static int exfat_find_free_span(uint32_t directory_cluster, uint8_t nofat,
                                const struct exfat_dir_ref *self_ref,
                                uint32_t needed, uint32_t *out_index);

static int exfat_write_file_set(const struct exfat_dir_ref *ref, uint8_t entries[20][EXFAT_ENTRY_SIZE]);

static int exfat_grow_directory(uint32_t directory_cluster, uint8_t directory_nofat,
                                const struct exfat_dir_ref *self_ref);

static uint8_t exfat_dir_cache_data[64u * SECTOR_SIZE]
    __attribute__((aligned(4096)));
static uint8_t exfat_dir_cache_nofat;
static int exfat_dir_cache_load(uint32_t directory_cluster, uint8_t nofat,
                                uint64_t offset)
{
    if (!g_storage.cluster_bytes || (g_storage.cluster_bytes % SECTOR_SIZE) != 0u) {
        return -22;
    }
    uint64_t window = min_u64(sizeof(exfat_dir_cache_data), g_storage.cluster_bytes);
    uint64_t aligned = (offset / window) * window;
    uint64_t cluster_offset = aligned % g_storage.cluster_bytes;
    uint32_t bytes = (uint32_t)min_u64(sizeof(exfat_dir_cache_data),
                                       g_storage.cluster_bytes - cluster_offset);
    uint64_t cluster_index = g_storage.cluster_bytes ?
                             aligned / g_storage.cluster_bytes : 0u;
    uint32_t physical_cluster = 0;
    uint64_t lba = 0;
    int ret;
    if (!exfat_cluster_valid(directory_cluster) || !bytes) return -22;
    ret = exfat_cluster_at(directory_cluster, nofat, cluster_index, &physical_cluster);
    if (ret < 0) {
        exfat_dir_cache_valid = 0;
        console_error("[reliefnt] exfat directory cache cluster lookup failed volume=%u dir=%u nofat=%u offset=%llu index=%llu ret=%d\n",
                       g_storage.volume_id, directory_cluster, nofat,
                       (unsigned long long)aligned,
                       (unsigned long long)cluster_index, ret);
        return ret;
    }
    lba = exfat_cluster_lba(physical_cluster) + cluster_offset / SECTOR_SIZE;
    ret = exfat_read_stream(directory_cluster, nofat, aligned,
                            exfat_dir_cache_data, bytes);
    if (ret < 0) {
        exfat_dir_cache_valid = 0;
        exfat_dir_cache_volume = 0;
        exfat_dir_cache_bytes = 0;
        console_error("[reliefnt] exfat directory cache load failed volume=%u dir=%u nofat=%u index=%llu lba=%llu bytes=%u sectors=%u ret=%d\n",
                       g_storage.volume_id, directory_cluster, nofat,
                       (unsigned long long)cluster_index,
                       (unsigned long long)lba, bytes,
                       (unsigned)((bytes + SECTOR_SIZE - 1u) / SECTOR_SIZE), ret);
        return ret;
    }
    exfat_dir_cache_volume = g_active_volume;
    exfat_dir_cache_cluster = directory_cluster;
    exfat_dir_cache_nofat = nofat;
    exfat_dir_cache_offset = aligned;
    exfat_dir_cache_bytes = bytes;
    exfat_dir_cache_valid = 1u;
    return 0;
}

int exfat_dir_read_entry(uint32_t directory_cluster, uint8_t nofat,
                                uint32_t index, uint8_t entry[EXFAT_ENTRY_SIZE])
{
    uint64_t offset = (uint64_t)index * EXFAT_ENTRY_SIZE;
    int ret;
    if (!entry || index > UINT32_MAX / EXFAT_ENTRY_SIZE) return -22;
    if (!exfat_dir_cache_valid || exfat_dir_cache_volume != g_active_volume ||
        exfat_dir_cache_cluster != directory_cluster || exfat_dir_cache_nofat != nofat ||
        offset < exfat_dir_cache_offset ||
        offset + EXFAT_ENTRY_SIZE > exfat_dir_cache_offset + exfat_dir_cache_bytes) {
        ret = exfat_dir_cache_load(directory_cluster, nofat, offset);
        if (ret < 0) {
            console_error("[reliefnt] exfat directory entry read failed cluster=%u nofat=%u index=%u ret=%d\n",
                           directory_cluster, nofat, index, ret);
            return ret;
        }
    }
    storage_memcpy(entry, exfat_dir_cache_data + (offset - exfat_dir_cache_offset),
                   EXFAT_ENTRY_SIZE);
    return 0;
}

static int exfat_dir_write_entry(uint32_t directory_cluster, uint8_t nofat,
                                 uint32_t index, const uint8_t entry[EXFAT_ENTRY_SIZE])
{
    uint64_t offset = (uint64_t)index * EXFAT_ENTRY_SIZE;
    int ret;
    if (!entry || index > UINT32_MAX / EXFAT_ENTRY_SIZE) return -22;
    if (!exfat_dir_cache_valid || exfat_dir_cache_volume != g_active_volume ||
        exfat_dir_cache_cluster != directory_cluster || exfat_dir_cache_nofat != nofat ||
        offset < exfat_dir_cache_offset ||
        offset + EXFAT_ENTRY_SIZE > exfat_dir_cache_offset + exfat_dir_cache_bytes) {
        ret = exfat_dir_cache_load(directory_cluster, nofat, offset);
        if (ret < 0) {
            console_error("[reliefnt] exfat directory entry load failed cluster=%u nofat=%u index=%u ret=%d\n",
                           directory_cluster, nofat, index, ret);
            return ret;
        }
    }
    storage_memcpy(exfat_dir_cache_data + (offset - exfat_dir_cache_offset),
                   entry, EXFAT_ENTRY_SIZE);
    ret = exfat_write_stream(directory_cluster, nofat, offset, entry, EXFAT_ENTRY_SIZE);
    if (ret < 0) {
        console_error("[reliefnt] exfat directory entry write failed cluster=%u nofat=%u index=%u ret=%d\n",
                       directory_cluster, nofat, index, ret);
        exfat_dir_cache_valid = 0;
    }
    return ret;
}

static void exfat_dir_cache_merge_sector(uint32_t directory_cluster, uint8_t nofat,
                                         uint64_t sector_offset,
                                         const uint8_t sector[SECTOR_SIZE])
{
    uint64_t sector_end;
    uint64_t overlap_start;
    uint64_t overlap_end;
    if (!sector || !exfat_dir_cache_valid || exfat_dir_cache_volume != g_active_volume ||
        exfat_dir_cache_cluster != directory_cluster || exfat_dir_cache_nofat != nofat) {
        return;
    }
    sector_end = sector_offset + SECTOR_SIZE;
    overlap_start = sector_offset > exfat_dir_cache_offset ?
                    sector_offset : exfat_dir_cache_offset;
    overlap_end = sector_end < exfat_dir_cache_offset + exfat_dir_cache_bytes ?
                  sector_end : exfat_dir_cache_offset + exfat_dir_cache_bytes;
    if (overlap_end > overlap_start) {
        storage_memcpy(exfat_dir_cache_data + (overlap_start - exfat_dir_cache_offset),
                       sector + (overlap_start - sector_offset),
                       (size_t)(overlap_end - overlap_start));
    }
}

static int exfat_read_file_set(uint32_t directory_cluster, uint8_t nofat, uint32_t index,
                               uint8_t entries[20][EXFAT_ENTRY_SIZE], uint8_t *out_count,
                               uint16_t name[255], uint32_t *out_name_length,
                               struct storage_node *out_node)
{
    uint8_t count;
    uint32_t name_length;
    uint32_t name_pos = 0;
    uint32_t rendered_length = 0;
    char rendered[RELIEFOS_FS_NAME_LEN];
    int ret;
    if (!entries || !out_count || !name || !out_name_length || !out_node) return -22;
    ret = exfat_dir_read_entry(directory_cluster, nofat, index, entries[0]);
    if (ret < 0) return ret;
    if (entries[0][0] != EXFAT_ENTRY_FILE) return -2;
    count = entries[0][1];
    if (count < 2u || count > 19u) return -5;
    for (uint32_t i = 1; i <= count; ++i) {
        ret = exfat_dir_read_entry(directory_cluster, nofat, index + i, entries[i]);
        if (ret < 0) return ret;
    }
    if (exfat_checksum(&entries[0][0], count + 1u) != exfat_get_u16(entries[0] + 2u) ||
        entries[1][0] != EXFAT_ENTRY_STREAM || (entries[1][1] & (uint8_t)~EXFAT_STREAM_NO_FAT_CHAIN)) {
        return -5;
    }
    name_length = entries[1][3];
    if (name_length == 0u || name_length > 255u || count != 1u + (name_length + 14u) / 15u) return -5;
    for (uint32_t i = 2; i <= count; ++i) {
        if (entries[i][0] != EXFAT_ENTRY_NAME) return -5;
        for (uint32_t j = 0; j < 15u && name_pos < name_length; ++j) {
            name[name_pos++] = exfat_get_u16(entries[i] + 2u + j * 2u);
        }
    }
    if (name_pos != name_length) return -5;
    if (exfat_name_hash(name, name_length) != exfat_get_u16(entries[1] + 4u)) return -5;
    if (text_utf16le_to_utf8(name, name_length, rendered, sizeof(rendered),
                             &rendered_length) < 0 || !rendered[0]) return -5;
    storage_memzero(out_node, sizeof(*out_node));
    out_node->type = (exfat_get_u16(entries[0] + 4u) & EXFAT_ATTR_DIRECTORY)
                         ? RELIEFOS_FS_TYPE_DIR : RELIEFOS_FS_TYPE_FILE;
    out_node->flags = STORAGE_NODE_FLAG_EXFAT |
                      ((entries[1][1] & EXFAT_STREAM_NO_FAT_CHAIN)
                           ? STORAGE_NODE_FLAG_EXFAT_NOFAT : 0u);
    out_node->first_cluster = storage_get_u32(entries[1] + 20u);
    out_node->volume_id = g_storage.volume_id;
    out_node->size = exfat_get_u64(entries[1] + 24u);
    {
        uint64_t valid_length = exfat_get_u64(entries[1] + 8u);
        uint64_t needed = out_node->size
            ? (out_node->size + g_storage.cluster_bytes - 1u) / g_storage.cluster_bytes : 0u;
        if (valid_length > out_node->size ||
            (out_node->size && (!exfat_cluster_valid(out_node->first_cluster) ||
                                needed > g_storage.exfat_cluster_count)) ||
            (out_node->type == RELIEFOS_FS_TYPE_DIR &&
             (!exfat_cluster_valid(out_node->first_cluster) ||
              out_node->size < g_storage.cluster_bytes))) {
            return -5;
        }
        if (out_node->size && (entries[1][1] & EXFAT_STREAM_NO_FAT_CHAIN) &&
            (out_node->first_cluster > UINT32_MAX - (uint32_t)(needed - 1u) ||
             !exfat_cluster_valid(out_node->first_cluster + (uint32_t)(needed - 1u)))) {
            return -5;
        }
    }
    *out_count = count;
    *out_name_length = name_length;
    return 0;
}

uint64_t exfat_dir_entry_limit(void)
{
    uint64_t limit = (uint64_t)g_storage.exfat_cluster_count * g_storage.cluster_bytes /
                     EXFAT_ENTRY_SIZE;
    return limit > UINT32_MAX ? UINT32_MAX : limit;
}

int exfat_find_in_dir_ref(uint32_t directory_cluster, uint8_t nofat, const char *wanted,
                                 struct storage_node *out, struct exfat_dir_ref *out_ref)
{
    uint8_t raw[20][EXFAT_ENTRY_SIZE];
    uint16_t name[255];
    uint8_t count;
    uint32_t length;
    uint64_t limit;
    if (!wanted || !exfat_cluster_valid(directory_cluster)) return -22;
    /* Callers that will rewrite an entry need its physical entry-set
     * reference, so they must bypass the node-only directory cache. */
    if (!out_ref && storage_dir_index_lookup(directory_cluster, wanted, out)) return 0;
    limit = exfat_dir_entry_limit();
    for (uint64_t pos = 0; pos < limit; ) {
        uint8_t first[EXFAT_ENTRY_SIZE];
        int ret = exfat_dir_read_entry(directory_cluster, nofat, (uint32_t)pos, first);
        if (ret < 0) {
            console_error("[reliefnt] exfat lookup failed dir=%u nofat=%u name=%s index=%u ret=%d\n",
                           directory_cluster, nofat, wanted, (uint32_t)pos, ret);
            return ret;
        }
        if (first[0] == 0u) return -2;
        if (first[0] != EXFAT_ENTRY_FILE) {
            ++pos;
            continue;
        }
        {
            struct storage_node node;
            ret = exfat_read_file_set(directory_cluster, nofat, (uint32_t)pos, raw, &count,
                                      name, &length, &node);
            if (ret < 0) {
                console_error("[reliefnt] exfat lookup entry-set invalid dir=%u nofat=%u name=%s index=%u ret=%d\n",
                               directory_cluster, nofat, wanted, (uint32_t)pos, ret);
                return ret;
            }
            if (exfat_name_equals(name, length, wanted)) {
                if (out) *out = node;
                if (out_ref) {
                    out_ref->directory_cluster = directory_cluster;
                    out_ref->directory_nofat = nofat;
                    out_ref->first_entry = (uint32_t)pos;
                    out_ref->secondary_count = count;
                }
                storage_dir_index_store(directory_cluster, wanted, &node);
                return 0;
            }
            pos += count + 1u;
        }
    }
    /* A complete scan without a matching entry is the normal ENOENT case.
     * Callers use -2 to distinguish it from media or metadata corruption and
     * to decide whether O_CREAT should create a new entry-set. */
    return -2;
}

int exfat_lookup_path_ref(const char *path, struct storage_node *out,
                                 struct exfat_dir_ref *out_ref)
{
    struct storage_node node;
    struct exfat_dir_ref matched_ref;
    char name[RELIEFOS_FS_NAME_LEN];
    uint32_t length = 0;
    const char *cursor;
    if (!path || path[0] != '/') return -22;
    storage_memzero(&node, sizeof(node));
    node.type = RELIEFOS_FS_TYPE_DIR;
    node.flags = STORAGE_NODE_FLAG_ROOT | STORAGE_NODE_FLAG_EXFAT;
    node.first_cluster = g_storage.exfat_root_cluster;
    node.volume_id = g_storage.volume_id;
    if (out_ref) storage_memzero(out_ref, sizeof(*out_ref));
    cursor = path + 1;
    while (*cursor) {
        if (*cursor == '/') {
            if (length) {
                name[length] = 0;
                int ret;
                if (node.type != RELIEFOS_FS_TYPE_DIR) return -20;
                ret = exfat_find_in_dir_ref(node.first_cluster,
                                            (node.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                            name, &node, &matched_ref);
                if (ret < 0) return ret;
                if (out_ref) *out_ref = matched_ref;
                length = 0;
            }
        } else if (length + 1u < sizeof(name)) {
            name[length++] = *cursor;
        } else return -22;
        ++cursor;
    }
    if (length) {
        name[length] = 0;
        int ret;
        if (node.type != RELIEFOS_FS_TYPE_DIR) return -20;
        ret = exfat_find_in_dir_ref(node.first_cluster,
                                    (node.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                    name, &node, &matched_ref);
        if (ret < 0) return ret;
        if (out_ref) *out_ref = matched_ref;
    }
    if (out) *out = node;
    return 0;
}

int exfat_lookup_path(const char *path, struct storage_node *out)
{
    return exfat_lookup_path_ref(path, out, 0);
}

int exfat_iter_dir_entry(uint32_t directory_cluster, uint8_t nofat, uint64_t wanted,
                                struct reliefos_dir_entry *out)
{
    uint8_t raw[20][EXFAT_ENTRY_SIZE];
    uint16_t name[255];
    uint8_t count;
    uint32_t length;
    uint64_t ordinal = 0;
    uint64_t limit = exfat_dir_entry_limit();
    if (!out || !exfat_cluster_valid(directory_cluster)) return -22;
    for (uint64_t pos = 0; pos < limit; ) {
        uint8_t first[EXFAT_ENTRY_SIZE];
        struct storage_node node;
        uint32_t rendered_length = 0;
        int ret = exfat_dir_read_entry(directory_cluster, nofat, (uint32_t)pos, first);
        if (ret < 0) return ret;
        if (first[0] == 0u) return -2;
        if (first[0] != EXFAT_ENTRY_FILE) {
            ++pos;
            continue;
        }
        ret = exfat_read_file_set(directory_cluster, nofat, (uint32_t)pos, raw, &count,
                                  name, &length, &node);
        if (ret < 0) return ret;
        if (text_utf16le_to_utf8(name, length, out->name, sizeof(out->name),
                                 &rendered_length) < 0) return -5;
        /* A 255-unit exFAT name needs more room than a directory entry offers;
         * terminate the retained prefix so callers never read past the array. */
        if (rendered_length >= sizeof(out->name)) {
            out->name[sizeof(out->name) - 1u] = 0;
        }
        if (!storage_is_acl_metadata_name(out->name)) {
            if (ordinal == wanted) {
                out->type = node.type;
                return 0;
            }
            ++ordinal;
        }
        pos += count + 1u;
    }
    return -2;
}

static int exfat_find_free_span(uint32_t directory_cluster, uint8_t nofat,
                                const struct exfat_dir_ref *self_ref,
                                uint32_t needed, uint32_t *out_index)
{
    uint64_t limit = exfat_dir_entry_limit();
    if (!out_index || needed == 0 || !exfat_cluster_valid(directory_cluster)) return -22;
    for (;;) {
        uint32_t run = 0;
        uint8_t grew = 0;
        for (uint64_t pos = 0; pos < limit; ++pos) {
            uint8_t entry[EXFAT_ENTRY_SIZE];
            int ret = exfat_dir_read_entry(directory_cluster, nofat, (uint32_t)pos, entry);
            if (ret < 0) {
                console_error("[reliefnt] exfat find-free-span read failed dir=%u nofat=%u needed=%u pos=%u ret=%d\n",
                               directory_cluster, nofat, needed, (uint32_t)pos, ret);
                return ret;
            }
            if ((entry[0] & 0x80u) != 0u) {
                run = 0;
                continue;
            }
            ++run;
            if (run == needed) {
                *out_index = (uint32_t)(pos + 1u - needed);
                return 0;
            }
            if (entry[0] == 0u) {
                /* Entries after the end marker are free, but may only be used
                 * inside the currently allocated cluster. Grow explicitly if
                 * the complete entry-set would cross that boundary. */
                uint32_t candidate = (uint32_t)(pos + 1u - run);
                uint32_t per_cluster = g_storage.cluster_bytes / EXFAT_ENTRY_SIZE;
                uint32_t in_cluster = per_cluster ? candidate % per_cluster : 0u;
                if (per_cluster && needed <= per_cluster - in_cluster) {
                    *out_index = candidate;
                    return 0;
                }
                if (nofat) return -28;
                ret = exfat_grow_directory(directory_cluster, nofat, self_ref);
                if (ret < 0) {
                    console_error("[reliefnt] exfat find-free-span grow after end failed dir=%u needed=%u pos=%u ret=%d\n",
                                   directory_cluster, needed, (uint32_t)pos, ret);
                    return ret;
                }
                grew = 1;
                break;
            }
        }
        if (grew) continue;
        console_error("[reliefnt] exfat find-free-span exhausted dir=%u needed=%u\n",
                       directory_cluster, needed);
        return -28;
    }
}

static int exfat_write_file_set(const struct exfat_dir_ref *ref, uint8_t entries[20][EXFAT_ENTRY_SIZE])
{
    uint8_t total;
    if (!ref || !entries || ref->secondary_count < 2u || ref->secondary_count > 19u) return -22;
    total = ref->secondary_count + 1u;
    exfat_put_u16(entries[0] + 2u, exfat_checksum(&entries[0][0], total));
    for (uint32_t i = 0; i < total; ) {
        uint64_t stream_offset = (uint64_t)(ref->first_entry + i) * EXFAT_ENTRY_SIZE;
        uint32_t sector_offset = (uint32_t)(stream_offset % SECTOR_SIZE);
        uint32_t run = min_u32(total - i, (SECTOR_SIZE - sector_offset) / EXFAT_ENTRY_SIZE);
        uint64_t sector_base = stream_offset - sector_offset;
        int ret;
        if (!run) return -22;
        /* Read the complete sector before changing any 32-byte entries. This
         * preserves unrelated entries and commits all entries in this sector
         * atomically from the filesystem's perspective. */
        ret = exfat_read_stream(ref->directory_cluster, ref->directory_nofat,
                                sector_base, storage_scratch, SECTOR_SIZE);
        if (ret < 0) {
            exfat_dir_cache_valid = 0;
            console_error("[reliefnt] exfat entry-set read-modify-read failed dir=%u nofat=%u index=%u lba-offset=%llu entries=%u ret=%d\n",
                           ref->directory_cluster, ref->directory_nofat,
                           ref->first_entry + i, (unsigned long long)sector_base, run, ret);
            return ret;
        }
        for (uint32_t j = 0; j < run; ++j) {
            storage_memcpy(storage_scratch + sector_offset + j * EXFAT_ENTRY_SIZE,
                           entries[i + j], EXFAT_ENTRY_SIZE);
        }
        ret = exfat_write_stream(ref->directory_cluster, ref->directory_nofat,
                                 sector_base, storage_scratch, SECTOR_SIZE);
        if (ret < 0) {
            exfat_dir_cache_valid = 0;
            console_error("[reliefnt] exfat entry-set sector write failed dir=%u nofat=%u index=%u offset=%llu entries=%u ret=%d\n",
                           ref->directory_cluster, ref->directory_nofat,
                           ref->first_entry + i, (unsigned long long)sector_base, run, ret);
            return ret;
        }
        exfat_dir_cache_merge_sector(ref->directory_cluster, ref->directory_nofat,
                                     sector_base, storage_scratch);
        i += run;
    }
    return 0;
}

int exfat_create_entry(uint32_t directory_cluster, uint8_t directory_nofat,
                              const struct exfat_dir_ref *self_ref, const char *name,
                              uint8_t directory, uint32_t first_cluster, uint64_t size,
                              uint8_t nofat, struct exfat_dir_ref *out_ref)
{
    uint8_t entries[20][EXFAT_ENTRY_SIZE];
    uint16_t utf16[255];
    uint32_t length;
    uint32_t names;
    uint32_t index;
    struct exfat_dir_ref ref;
    int ret = exfat_utf8_name(name, utf16, &length);
    if (ret < 0) return ret;
    names = (length + 14u) / 15u;
    if (names + 2u > 20u) return -22;
    ret = exfat_find_free_span(directory_cluster, directory_nofat, self_ref,
                               names + 2u, &index);
    if (ret < 0) {
        console_error("[reliefnt] exfat create entry free-span failed dir=%u name=%s needed=%u ret=%d\n",
                       directory_cluster, name, names + 2u, ret);
        return ret;
    }
    storage_memzero(entries, sizeof(entries));
    entries[0][0] = EXFAT_ENTRY_FILE;
    entries[0][1] = (uint8_t)(names + 1u);
    exfat_put_u16(entries[0] + 4u, directory ? EXFAT_ATTR_DIRECTORY : 0u);
    entries[1][0] = EXFAT_ENTRY_STREAM;
    entries[1][1] = nofat ? EXFAT_STREAM_NO_FAT_CHAIN : 0u;
    entries[1][3] = (uint8_t)length;
    exfat_put_u16(entries[1] + 4u, exfat_name_hash(utf16, length));
    exfat_put_u64(entries[1] + 8u, size);
    storage_put_u32(entries[1] + 20u, first_cluster);
    exfat_put_u64(entries[1] + 24u, size);
    for (uint32_t n = 0; n < names; ++n) {
        entries[n + 2u][0] = EXFAT_ENTRY_NAME;
        for (uint32_t i = 0; i < 15u && n * 15u + i < length; ++i) {
            exfat_put_u16(entries[n + 2u] + 2u + i * 2u, utf16[n * 15u + i]);
        }
    }
    ref.directory_cluster = directory_cluster;
    ref.directory_nofat = directory_nofat;
    ref.first_entry = index;
    ref.secondary_count = (uint8_t)(names + 1u);
    ret = exfat_write_file_set(&ref, entries);
    if (ret < 0) {
        console_error("[reliefnt] exfat create entry failed dir=%u name=%s index=%u entries=%u ret=%d\n",
                       directory_cluster, name, index, names + 2u, ret);
    } else {
        console_debug("[reliefnt] exfat create entry committed dir=%u name=%s index=%u entries=%u\n",
                       directory_cluster, name, index, names + 2u);
    }
    if (ret == 0 && out_ref) *out_ref = ref;
    return ret;
}

int exfat_update_entry_data(const struct exfat_dir_ref *ref, uint32_t first_cluster,
                                   uint64_t size, uint8_t nofat)
{
    uint8_t entries[20][EXFAT_ENTRY_SIZE];
    uint16_t name[255];
    uint8_t count;
    uint32_t length;
    struct storage_node node;
    int ret = exfat_read_file_set(ref->directory_cluster, ref->directory_nofat, ref->first_entry,
                                  entries, &count, name, &length, &node);
    if (ret < 0 || count != ref->secondary_count) return ret < 0 ? ret : -5;
    entries[1][1] = nofat ? EXFAT_STREAM_NO_FAT_CHAIN : 0u;
    exfat_put_u64(entries[1] + 8u, size);
    storage_put_u32(entries[1] + 20u, first_cluster);
    exfat_put_u64(entries[1] + 24u, size);
    return exfat_write_file_set(ref, entries);
}

static int exfat_grow_directory(uint32_t directory_cluster, uint8_t directory_nofat,
                                const struct exfat_dir_ref *self_ref)
{
    uint32_t tail = directory_cluster;
    uint32_t clusters = 1u;
    uint32_t next;
    uint32_t new_cluster;
    uint8_t new_nofat;
    int ret;
    if (directory_nofat || !exfat_cluster_valid(directory_cluster)) return -28;
    while (clusters <= g_storage.exfat_cluster_count) {
        ret = exfat_read_fat(tail, &next);
        if (ret < 0) return ret;
        if (next >= EXFAT_EOC) break;
        if (!exfat_cluster_valid(next)) return -5;
        tail = next;
        ++clusters;
    }
    if (clusters > g_storage.exfat_cluster_count) return -5;
    ret = exfat_allocate_clusters(1u, &new_cluster, &new_nofat);
    if (ret < 0) return ret;
    ret = exfat_write_fat(new_cluster, EXFAT_EOC);
    if (ret < 0) {
        (void)exfat_free_clusters(new_cluster, new_nofat, g_storage.cluster_bytes);
        console_error("[reliefnt] exfat directory grow stage=link-new cluster=%u ret=%d\n",
                       new_cluster, ret);
        return ret;
    }
    ret = exfat_zero_clusters(new_cluster, new_nofat, 1u);
    if (ret < 0) {
        (void)exfat_free_clusters(new_cluster, new_nofat, g_storage.cluster_bytes);
        console_error("[reliefnt] exfat directory grow stage=zero cluster=%u ret=%d\n",
                       new_cluster, ret);
        return ret;
    }
    ret = exfat_write_fat(tail, new_cluster);
    if (ret < 0) {
        (void)exfat_free_clusters(new_cluster, new_nofat, g_storage.cluster_bytes);
        console_error("[reliefnt] exfat directory grow stage=link-tail tail=%u new=%u ret=%d\n",
                       tail, new_cluster, ret);
        return ret;
    }
    if (self_ref) {
        ret = exfat_update_entry_data(self_ref, directory_cluster,
                                      (uint64_t)(clusters + 1u) * g_storage.cluster_bytes, 0u);
        if (ret < 0) return ret;
    }
    return 0;
}

int exfat_delete_entry(const struct exfat_dir_ref *ref)
{
    uint8_t entry[EXFAT_ENTRY_SIZE];
    if (!ref) return -22;
    for (uint32_t i = 0; i <= ref->secondary_count; ++i) {
        int ret = exfat_dir_read_entry(ref->directory_cluster, ref->directory_nofat,
                                       ref->first_entry + i, entry);
        if (ret < 0) return ret;
        entry[0] &= 0x7fu;
        ret = exfat_dir_write_entry(ref->directory_cluster, ref->directory_nofat,
                                    ref->first_entry + i, entry);
        if (ret < 0) return ret;
    }
    return 0;
}

int exfat_dir_is_empty(const struct storage_node *node)
{
    uint64_t limit;
    if (!node || node->type != RELIEFOS_FS_TYPE_DIR) return -22;
    limit = exfat_dir_entry_limit();
    for (uint64_t pos = 0; pos < limit; ) {
        uint8_t entry[EXFAT_ENTRY_SIZE];
        int ret = exfat_dir_read_entry(node->first_cluster,
                                       (node->flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                       (uint32_t)pos, entry);
        if (ret < 0) return ret;
        if (entry[0] == 0u) return 1;
        if (entry[0] == EXFAT_ENTRY_FILE) {
            uint8_t raw[20][EXFAT_ENTRY_SIZE];
            uint16_t name[255];
            char rendered[RELIEFOS_FS_NAME_LEN];
            uint8_t count;
            uint32_t length;
            uint32_t rendered_length = 0;
            struct storage_node child;

            ret = exfat_read_file_set(node->first_cluster,
                                      (node->flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                      (uint32_t)pos, raw, &count, name, &length, &child);
            if (ret < 0) return ret;
            if (text_utf16le_to_utf8(name, length, rendered, sizeof(rendered),
                                     &rendered_length) < 0) return -5;
            if (rendered_length >= sizeof(rendered)) {
                rendered[sizeof(rendered) - 1u] = 0;
            }
            if (!storage_is_acl_metadata_name(rendered)) return 0;
            pos += count + 1u;
            continue;
        }
        ++pos;
    }
    return -5;
}
