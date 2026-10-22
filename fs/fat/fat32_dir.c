#include "internal.h"

static int fat32_dirent_is_acl_metadata(const struct fat32_dirent *de);


static int fat32_dirent_is_acl_metadata(const struct fat32_dirent *de)
{
    if (!de || de->attr == FAT32_ATTR_LFN ||
        (de->attr & FAT32_ATTR_DIRECTORY) != 0 ||
        (de->attr & 0x08u) != 0) {
        return 0;
    }
    return fat32_name_match_short(de, "LEONACL.SYS");
}

int fat32_find_in_dir(uint32_t dir_cluster, const char *name, struct storage_node *out)
{
    uint32_t cluster = dir_cluster;
    uint16_t lfn_parts[20][13];
    uint32_t lfn_count = 0;
    if (storage_dir_index_lookup(dir_cluster, name, out)) {
        return 0;
    }
    for (;;) {
        int ret = fat32_read_lookup_cluster(cluster, storage_cluster_buf);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        for (uint32_t off = 0; off < g_storage.cluster_bytes; off += sizeof(struct fat32_dirent)) {
            struct fat32_dirent *de = (struct fat32_dirent *)(void *)(storage_cluster_buf + off);
            if (de->name[0] == 0x00) {
                return -2;
            }
            if (de->name[0] == 0xe5) {
                lfn_count = 0;
                continue;
            }
            if (de->attr == FAT32_ATTR_LFN) {
                const struct fat32_lfn *lfn = (const struct fat32_lfn *)(const void *)de;
                uint8_t order = lfn->order & 0x1fu;
                if (order == 0 || order > 20) {
                    lfn_count = 0;
                    continue;
                }
                storage_memzero(lfn_parts[order - 1], sizeof(lfn_parts[0]));
                uint32_t len = 0;
                fat32_lfn_extract_utf16(lfn, lfn_parts[order - 1], &len, 13);
                if (order > lfn_count) {
                    lfn_count = order;
                }
                continue;
            }
            if ((de->attr & 0x08u) != 0) {
                lfn_count = 0;
                continue;
            }
            int matched = 0;
            struct storage_node candidate = {
                .type = (de->attr & FAT32_ATTR_DIRECTORY) ? RELIEFOS_FS_TYPE_DIR : RELIEFOS_FS_TYPE_FILE,
                .flags = 0,
                .first_cluster = ((uint32_t)de->first_cluster_hi << 16) | de->first_cluster_lo,
                .volume_id = g_storage.volume_id,
                .size = de->size,
            };
            if (lfn_count) {
                char full[RELIEFOS_FS_NAME_LEN];
                fat32_build_lfn_name(lfn_parts, lfn_count, full, sizeof(full));
                storage_dir_index_store(dir_cluster, full, &candidate);
                matched = storage_text_eq_ci(full, name) || fat32_name_match_short(de, name);
            } else {
                matched = fat32_name_match_short(de, name);
            }
            lfn_count = 0;
            if (!matched) {
                continue;
            }
            if (out) {
                *out = candidate;
            }
            return 0;
        }
        uint32_t next = 0;
        ret = fat32_read_fat_entry(cluster, &next);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        if (next >= FAT32_EOC) {
            return -2;
        }
        cluster = next;
    }
}

int fat32_find_dirent_ref_in_dir(uint32_t dir_cluster, const char *name, struct fat32_dir_ref *out)
{
    uint32_t cluster = dir_cluster;
    uint16_t lfn_parts[20][13];
    uint32_t lfn_count = 0;
    for (;;) {
        int ret = fat32_read_lookup_cluster(cluster, storage_cluster_buf);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        for (uint32_t off = 0; off < g_storage.cluster_bytes; off += sizeof(struct fat32_dirent)) {
            struct fat32_dirent *de = (struct fat32_dirent *)(void *)(storage_cluster_buf + off);
            if (de->name[0] == 0x00) {
                return -2;
            }
            if (de->name[0] == 0xe5) {
                lfn_count = 0;
                continue;
            }
            if (de->attr == FAT32_ATTR_LFN) {
                const struct fat32_lfn *lfn = (const struct fat32_lfn *)(const void *)de;
                uint8_t order = lfn->order & 0x1fu;
                if (order == 0 || order > 20) {
                    lfn_count = 0;
                    continue;
                }
                storage_memzero(lfn_parts[order - 1], sizeof(lfn_parts[0]));
                {
                    uint32_t len = 0;
                    fat32_lfn_extract_utf16(lfn, lfn_parts[order - 1], &len, 13);
                }
                if (order > lfn_count) {
                    lfn_count = order;
                }
                continue;
            }
            if ((de->attr & 0x08u) != 0) {
                lfn_count = 0;
                continue;
            }
            {
                int matched = 0;
                if (lfn_count) {
                    char full[RELIEFOS_FS_NAME_LEN];
                    fat32_build_lfn_name(lfn_parts, lfn_count, full, sizeof(full));
                    matched = storage_text_eq_ci(full, name) || fat32_name_match_short(de, name);
                } else {
                    matched = fat32_name_match_short(de, name);
                }
                lfn_count = 0;
                if (!matched) {
                    continue;
                }
                if (out) {
                    out->entry_cluster = cluster;
                    out->entry_offset = off;
                    out->dirent = *de;
                }
                return 0;
            }
        }
        {
            uint32_t next = 0;
            ret = fat32_read_fat_entry(cluster, &next);
            if (ret < 0) {
                return storage_read_failure(ret);
            }
            if (next >= FAT32_EOC) {
                return -2;
            }
            cluster = next;
        }
    }
}

int fat32_update_dirent(const struct fat32_dir_ref *ref)
{
    int ret;
    if (!ref || ref->entry_cluster < 2 || ref->entry_offset + sizeof(struct fat32_dirent) > g_storage.cluster_bytes) {
        return -22;
    }
    ret = fat32_read_cluster(ref->entry_cluster, storage_cluster_buf);
    if (ret < 0) {
        return storage_read_failure(ret);
    }
    *(struct fat32_dirent *)(void *)(storage_cluster_buf + ref->entry_offset) = ref->dirent;
    ret = fat32_write_cluster(ref->entry_cluster, storage_cluster_buf);
    if (ret < 0) {
        return storage_read_failure(ret);
    }
    return 0;
}

/**
 * @brief Reserve consecutive directory slots, extending a chain when needed.
 * Caller holds the storage transaction. Before linking a new empty cluster,
 * convert old unused end markers to deleted entries so FAT readers reach it.
 */
int fat32_find_free_dirent_span(uint32_t dir_cluster, uint32_t slots,
                                       struct fat32_dir_span *out)
{
    uint32_t cluster = dir_cluster;
    uint32_t per_cluster = g_storage.cluster_bytes / sizeof(struct fat32_dirent);
    if (!out || slots == 0 || slots > per_cluster) {
        return -22;
    }
    for (;;) {
        uint32_t run = 0;
        uint32_t run_start = 0;
        int ret = fat32_read_cluster(cluster, storage_cluster_buf);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        for (uint32_t off = 0; off < g_storage.cluster_bytes; off += sizeof(struct fat32_dirent)) {
            struct fat32_dirent *de = (struct fat32_dirent *)(void *)(storage_cluster_buf + off);
            if (de->name[0] == 0x00 || de->name[0] == 0xe5) {
                if (run == 0) {
                    run_start = off;
                }
                ++run;
                if (run >= slots) {
                    out->entry_cluster = cluster;
                    out->entry_offset = run_start;
                    return 0;
                }
            } else {
                run = 0;
            }
        }
        {
            uint32_t next = 0;
            ret = fat32_read_fat_entry(cluster, &next);
            if (ret < 0) {
                return storage_read_failure(ret);
            }
            if (next >= FAT32_EOC) {
                uint32_t new_cluster = 0;
                ret = fat32_find_free_cluster(&new_cluster);
                if (ret < 0) {
                    return storage_read_failure(ret);
                }
                ret = fat32_write_fat_entry(new_cluster, FAT32_EOC);
                if (ret < 0) {
                    return storage_read_failure(ret);
                }
                ret = fat32_note_allocated();
                if (ret < 0) {
                    return ret;
                }
                storage_memzero(storage_cluster_buf, g_storage.cluster_bytes);
                ret = fat32_write_cluster(new_cluster, storage_cluster_buf);
                if (ret < 0) {
                    return storage_read_failure(ret);
                }
                /* A 0x00 entry ends the entire directory, including later
                 * clusters. An LFN may not fit the old trailing free slots. */
                ret = fat32_read_cluster(cluster, storage_cluster_buf);
                if (ret < 0) return storage_read_failure(ret);
                int changed = 0;
                for (uint32_t off = 0; off < g_storage.cluster_bytes;
                     off += sizeof(struct fat32_dirent)) {
                    if (storage_cluster_buf[off] == 0) {
                        storage_cluster_buf[off] = 0xe5;
                        changed = 1;
                    }
                }
                if (changed) {
                    ret = fat32_write_cluster(cluster, storage_cluster_buf);
                    if (ret < 0) return storage_read_failure(ret);
                }
                /* Publish only after the new cluster is initialized. */
                ret = fat32_write_fat_entry(cluster, new_cluster);
                if (ret < 0) return storage_read_failure(ret);
                out->entry_cluster = new_cluster;
                out->entry_offset = 0;
                return 0;
            }
            cluster = next;
        }
    }
}

int fat32_create_dirent(uint32_t parent_cluster, const char *name, uint8_t attr,
                               uint32_t first_cluster, uint32_t size)
{
    struct fat32_dir_span span;
    struct fat32_dirent dirent;
    uint8_t short_name[11];
    uint8_t need_lfn = 0;
    uint32_t lfn_count = 0;
    int ret;

    ret = fat32_name_needs_lfn(name, short_name, &need_lfn);
    if (ret < 0) {
        return ret;
    }
    if (need_lfn) {
        ret = fat32_make_short_alias(parent_cluster, name, short_name);
        if (ret < 0) {
            return ret;
        }
        lfn_count = fat32_lfn_entry_count(name);
        if (lfn_count > 20u) {
            return -22;
        }
    }
    ret = fat32_find_free_dirent_span(parent_cluster, lfn_count + 1u, &span);
    if (ret < 0) {
        return ret;
    }
    storage_memzero(&dirent, sizeof(dirent));
    storage_memcpy(dirent.name, short_name, 11);
    dirent.attr = attr;
    dirent.first_cluster_hi = (uint16_t)(first_cluster >> 16);
    dirent.first_cluster_lo = (uint16_t)(first_cluster & 0xffffu);
    dirent.size = size;

    ret = fat32_read_cluster(span.entry_cluster, storage_cluster_buf);
    if (ret < 0) {
        return storage_read_failure(ret);
    }
    if (need_lfn) {
        uint16_t utf16_name[260];
        uint32_t name_len = fat32_utf16_name(name, utf16_name,
                                             sizeof(utf16_name) / sizeof(utf16_name[0]));
        uint8_t checksum = fat32_short_name_checksum(short_name);
        for (uint32_t i = 0; i < lfn_count; ++i) {
            struct fat32_lfn *lfn = (struct fat32_lfn *)(void *)(storage_cluster_buf +
                span.entry_offset + i * sizeof(struct fat32_dirent));
            uint32_t part = lfn_count - i - 1u;
            fat32_fill_lfn_entry(lfn, utf16_name, name_len, part, lfn_count, checksum);
        }
    }
    *(struct fat32_dirent *)(void *)(storage_cluster_buf +
        span.entry_offset + lfn_count * sizeof(struct fat32_dirent)) = dirent;
    ret = fat32_write_cluster(span.entry_cluster, storage_cluster_buf);
    if (ret < 0) {
        return storage_read_failure(ret);
    }
    return 0;
}

int fat32_delete_dirent(uint32_t dir_cluster, const char *name,
                               struct fat32_dirent *deleted)
{
    uint32_t cluster = dir_cluster;
    uint16_t lfn_parts[20][13];
    uint32_t lfn_count = 0;
    uint32_t lfn_start = 0xffffffffu;
    for (;;) {
        int ret = fat32_read_cluster(cluster, storage_cluster_buf);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        for (uint32_t off = 0; off < g_storage.cluster_bytes; off += sizeof(struct fat32_dirent)) {
            struct fat32_dirent *de = (struct fat32_dirent *)(void *)(storage_cluster_buf + off);
            if (de->name[0] == 0x00) {
                return -2;
            }
            if (de->name[0] == 0xe5) {
                lfn_count = 0;
                lfn_start = 0xffffffffu;
                continue;
            }
            if (de->attr == FAT32_ATTR_LFN) {
                const struct fat32_lfn *lfn = (const struct fat32_lfn *)(const void *)de;
                uint8_t order = lfn->order & 0x1fu;
                if (lfn_start == 0xffffffffu) {
                    lfn_start = off;
                }
                if (order == 0 || order > 20) {
                    lfn_count = 0;
                    continue;
                }
                storage_memzero(lfn_parts[order - 1], sizeof(lfn_parts[0]));
                {
                    uint32_t len = 0;
                    fat32_lfn_extract_utf16(lfn, lfn_parts[order - 1], &len, 13);
                }
                if (order > lfn_count) {
                    lfn_count = order;
                }
                continue;
            }
            if ((de->attr & 0x08u) != 0) {
                lfn_count = 0;
                lfn_start = 0xffffffffu;
                continue;
            }
            {
                int matched;
                if (lfn_count) {
                    char full[RELIEFOS_FS_NAME_LEN];
                    fat32_build_lfn_name(lfn_parts, lfn_count, full, sizeof(full));
                    matched = storage_text_eq_ci(full, name) || fat32_name_match_short(de, name);
                } else {
                    matched = fat32_name_match_short(de, name);
                }
                if (!matched) {
                    lfn_count = 0;
                    lfn_start = 0xffffffffu;
                    continue;
                }
                if (deleted) {
                    *deleted = *de;
                }
                uint32_t start = lfn_start == 0xffffffffu ? off : lfn_start;
                for (uint32_t clear = start; clear <= off; clear += sizeof(struct fat32_dirent)) {
                    struct fat32_dirent *clear_de =
                        (struct fat32_dirent *)(void *)(storage_cluster_buf + clear);
                    clear_de->name[0] = 0xe5u;
                }
                ret = fat32_write_cluster(cluster, storage_cluster_buf);
                if (ret < 0) {
                    return storage_read_failure(ret);
                }
                return 0;
            }
        }
        {
            uint32_t next = 0;
            ret = fat32_read_fat_entry(cluster, &next);
            if (ret < 0) {
                return storage_read_failure(ret);
            }
            if (next >= FAT32_EOC) {
                return -2;
            }
            cluster = next;
        }
    }
}

int fat32_delete_acl_metadata_file(uint32_t dir_cluster)
{
    struct storage_node meta;
    int ret = fat32_find_in_dir(dir_cluster, "LEONACL.SYS", &meta);
    if (ret == -2) {
        return 0;
    }
    if (ret < 0) {
        return ret;
    }
    if (meta.type != RELIEFOS_FS_TYPE_FILE) {
        return 0;
    }
    ret = fat32_delete_dirent(dir_cluster, "LEONACL.SYS", 0);
    if (ret < 0) {
        return ret;
    }
    if (meta.first_cluster >= 2) {
        ret = fat32_free_chain(meta.first_cluster);
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

int fat32_dir_is_empty(uint32_t dir_cluster)
{
    uint32_t cluster = dir_cluster;
    for (;;) {
        int ret = fat32_read_cluster(cluster, storage_cluster_buf);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        for (uint32_t off = 0; off < g_storage.cluster_bytes; off += sizeof(struct fat32_dirent)) {
            struct fat32_dirent *de = (struct fat32_dirent *)(void *)(storage_cluster_buf + off);
            if (de->name[0] == 0x00) {
                return 1;
            }
            if (de->name[0] == 0xe5 || de->attr == FAT32_ATTR_LFN || (de->attr & 0x08u) != 0) {
                continue;
            }
            if (fat32_dirent_is_acl_metadata(de)) {
                continue;
            }
            if (de->name[0] == '.' &&
                (de->name[1] == ' ' || (de->name[1] == '.' && de->name[2] == ' '))) {
                continue;
            }
            return 0;
        }
        {
            uint32_t next = 0;
            ret = fat32_read_fat_entry(cluster, &next);
            if (ret < 0) {
                return storage_read_failure(ret);
            }
            if (next >= FAT32_EOC) {
                return 1;
            }
            cluster = next;
        }
    }
}

int fat32_iter_dir_entry(uint32_t dir_cluster, uint64_t index, struct reliefos_dir_entry *entry)
{
    uint32_t cluster = dir_cluster;
    uint64_t emitted = 0;
    uint16_t lfn_parts[20][13];
    uint32_t lfn_count = 0;
    uint32_t first_offset = 0;
    if (!entry || dir_cluster < 2) {
        return -22;
    }
    if (storage_dir_iter_cache.valid &&
        storage_dir_iter_cache.volume == g_active_volume &&
        storage_dir_iter_cache.first_cluster == dir_cluster &&
        storage_dir_iter_cache.next_index == index) {
        cluster = storage_dir_iter_cache.cluster;
        first_offset = storage_dir_iter_cache.entry_offset;
        emitted = index;
    } else {
        storage_dir_iter_cache.valid = 0;
    }
    for (;;) {
        int ret = fat32_read_cluster(cluster, storage_cluster_buf);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        for (uint32_t off = first_offset; off < g_storage.cluster_bytes;
             off += sizeof(struct fat32_dirent)) {
            struct fat32_dirent *de = (struct fat32_dirent *)(void *)(storage_cluster_buf + off);
            if (de->name[0] == 0x00) {
                storage_dir_iter_cache.valid = 0;
                return -2;
            }
            if (de->name[0] == 0xe5) {
                lfn_count = 0;
                continue;
            }
            if (de->attr == FAT32_ATTR_LFN) {
                const struct fat32_lfn *lfn = (const struct fat32_lfn *)(const void *)de;
                uint8_t order = lfn->order & 0x1fu;
                if (order && order <= 20) {
                    storage_memzero(lfn_parts[order - 1], sizeof(lfn_parts[0]));
                    uint32_t len = 0;
                    fat32_lfn_extract_utf16(lfn, lfn_parts[order - 1], &len, 13);
                    if (order > lfn_count) {
                        lfn_count = order;
                    }
                }
                continue;
            }
            if ((de->attr & 0x08u) != 0) {
                lfn_count = 0;
                continue;
            }
            char name[RELIEFOS_FS_NAME_LEN];
            if (lfn_count) {
                fat32_build_lfn_name(lfn_parts, lfn_count, name, sizeof(name));
            } else {
                struct storage_node tmp;
                (void)tmp;
                uint32_t pos = 0;
                for (uint32_t i = 0; i < 8 && de->name[i] != ' '; ++i) {
                    char ch = (char)de->name[i];
                    if (ch >= 'A' && ch <= 'Z') {
                        ch = (char)(ch - 'A' + 'a');
                    }
                    if (pos + 1 < sizeof(name)) {
                        name[pos++] = ch;
                    }
                }
                if (de->name[8] != ' ' && pos + 1 < sizeof(name)) {
                    name[pos++] = '.';
                }
                for (uint32_t i = 8; i < 11 && de->name[i] != ' '; ++i) {
                    char ch = (char)de->name[i];
                    if (ch >= 'A' && ch <= 'Z') {
                        ch = (char)(ch - 'A' + 'a');
                    }
                    if (pos + 1 < sizeof(name)) {
                        name[pos++] = ch;
                    }
                }
                name[pos] = 0;
            }
            lfn_count = 0;
            if ((name[0] == '.' && name[1] == 0) ||
                (name[0] == '.' && name[1] == '.' && name[2] == 0)) {
                continue;
            }
            if (storage_is_acl_metadata_name(name)) {
                continue;
            }
            if (emitted++ != index) {
                continue;
            }
            entry->type = (de->attr & FAT32_ATTR_DIRECTORY) ? RELIEFOS_FS_TYPE_DIR : RELIEFOS_FS_TYPE_FILE;
            storage_copy_text(entry->name, sizeof(entry->name), name);
            storage_dir_iter_cache.volume = g_active_volume;
            storage_dir_iter_cache.first_cluster = dir_cluster;
            storage_dir_iter_cache.cluster = cluster;
            storage_dir_iter_cache.entry_offset = off + sizeof(struct fat32_dirent);
            storage_dir_iter_cache.next_index = index + 1u;
            if (storage_dir_iter_cache.entry_offset >= g_storage.cluster_bytes) {
                uint32_t next = 0;
                if (fat32_read_fat_entry(cluster, &next) < 0 || next >= FAT32_EOC) {
                    storage_dir_iter_cache.valid = 0;
                } else {
                    storage_dir_iter_cache.cluster = next;
                    storage_dir_iter_cache.entry_offset = 0;
                    storage_dir_iter_cache.valid = 1;
                }
            } else {
                storage_dir_iter_cache.valid = 1;
            }
            return 0;
        }
        uint32_t next = 0;
        ret = fat32_read_fat_entry(cluster, &next);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        if (next >= FAT32_EOC) {
            return -2;
        }
        cluster = next;
        first_offset = 0;
    }
}
