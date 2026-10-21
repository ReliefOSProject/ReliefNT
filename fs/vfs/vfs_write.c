#include "internal.h"



int storage_write_node(const char *path, uint64_t offset,
                       const void *buf, uint32_t len, uint32_t *out_written)
{
    struct storage_node node;
    char backend_path[RELIEFOS_FS_PATH_LEN];
    uint64_t end64;
    uint32_t total_len;
    uint32_t final_len;
    uint32_t old_count = 0;
    uint32_t clusters_needed;
    uint32_t first_cluster;
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t pos;
    uint32_t remaining;
    uint32_t read_len = 0;
    uint64_t phys = 0;
    uint32_t pages = 0;
    int ret;
    if (out_written) {
        *out_written = 0;
    }
    if (!path || (!buf && len != 0)) {
        return -22;
    }
    ret = storage_lookup_path(path, &node);
    if (ret < 0) {
        return ret;
    }
    /* A descriptor may have populated the path cache before the write grew
     * the file. Invalidate cached directory metadata before changing the
     * directory entry so a later open sees the new first cluster and size. */
    storage_cache_invalidate();
    if (node.type != RELIEFOS_FS_TYPE_FILE) {
        return -21;
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_TMPFS) {
        storage_begin_mutation();
        return tmpfs_write(g_storage.tmpfs, node.first_cluster, offset, buf, len, out_written);
    }
    if (storage_ext4_is_ext_family(&g_storage)) {
        storage_begin_mutation();
        /* storage_lookup_path() already selected /target's ext2 volume and
         * returned its inode. Passing a backend-local string through the
         * global router again would reinterpret /docs/foo as the installer
         * FAT root rather than /target/docs/foo. */
        return storage_ext4_ops.write(&g_storage, &node, offset, buf, len, out_written);
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_EXFAT) {
        ret = storage_backend_path(path, backend_path, sizeof(backend_path));
        if (ret < 0) return ret;
        storage_begin_mutation();
        return exfat_write_node_path(backend_path, offset, buf, len, out_written);
    }
    if (g_storage.filesystem != STORAGE_FILESYSTEM_FAT32) {
        return -30;
    }
    if (node.size > 0xffffffffULL) {
        return -28;
    }
    end64 = offset + len;
    if (end64 > 0xffffffffu || end64 < offset) {
        return -28;
    }
    total_len = (uint32_t)end64;
    if (len == 0) {
        return 0;
    }
    /* The remaining path modifies file data and its directory entry. Keep
     * pre-read/modify/write sequences in one non-replayable transaction. */
    storage_begin_mutation();
    if (offset <= node.size) {
        char parent[RELIEFOS_FS_PATH_LEN];
        char name[RELIEFOS_FS_NAME_LEN];
        struct storage_node parent_node;
        struct fat32_dir_ref ref;

        final_len = total_len > node.size ? total_len : (uint32_t)node.size;
        clusters_needed = final_len ?
            (final_len + g_storage.cluster_bytes - 1u) / g_storage.cluster_bytes : 0;
        if (clusters_needed > FAT32_MAX_FILE_CLUSTERS) {
            return -28;
        }
        uint8_t cached_append =
            offset == node.size && node.first_cluster >= 2 &&
            storage_write_chain_cache.valid &&
            storage_write_chain_cache.volume == g_active_volume &&
            storage_write_chain_cache.first_cluster == node.first_cluster &&
            storage_write_chain_cache.size == (uint32_t)node.size &&
            storage_text_eq(storage_write_chain_cache.path, path) &&
            storage_write_chain_cache.count > 0 &&
            clusters_needed >= storage_write_chain_cache.count;
        if (node.first_cluster >= 2) {
            if (cached_append) {
                old_count = storage_write_chain_cache.count;
                /* Only the last cluster is needed when the append stays
                 * inside the existing chain; restore that cached tail. */
                storage_old_chain[old_count - 1u] = storage_write_chain_cache.tail;
            } else {
                ret = fat32_collect_chain(node.first_cluster, storage_old_chain, &old_count);
                if (ret < 0) {
                    return ret;
                }
            }
        }
        /* Any later I/O failure can leave the chain partly changed. Rebuild
         * the append hint only after the directory entry commits below. */
        storage_write_chain_cache.valid = 0;
        for (uint32_t i = old_count; i < clusters_needed; ++i) {
            ret = fat32_find_free_cluster(&storage_old_chain[i]);
            if (ret < 0) {
                return ret;
            }
            if (fat32_write_fat_entry(storage_old_chain[i], FAT32_EOC) < 0) {
                return -5;
            }
            if (fat32_note_allocated() < 0) {
                return -5;
            }
        }
        if (cached_append) {
            if (clusters_needed > old_count) {
                if (fat32_write_fat_entry(storage_old_chain[old_count - 1u],
                                          storage_old_chain[old_count]) < 0) {
                    return -5;
                }
                for (uint32_t i = old_count; i < clusters_needed; ++i) {
                    uint32_t next = i + 1u < clusters_needed
                                        ? storage_old_chain[i + 1u]
                                        : FAT32_EOC;
                    if (fat32_write_fat_entry(storage_old_chain[i], next) < 0) {
                        return -5;
                    }
                }
            }
        } else if (offset == node.size && old_count && clusters_needed > old_count) {
            if (fat32_write_fat_entry(storage_old_chain[old_count - 1u],
                                      storage_old_chain[old_count]) < 0) {
                return -5;
            }
            for (uint32_t i = old_count; i < clusters_needed; ++i) {
                uint32_t next = i + 1u < clusters_needed
                                    ? storage_old_chain[i + 1u]
                                    : FAT32_EOC;
                if (fat32_write_fat_entry(storage_old_chain[i], next) < 0) {
                    return -5;
                }
            }
        } else {
            for (uint32_t i = 0; i < clusters_needed; ++i) {
                uint32_t next = i + 1u < clusters_needed
                                    ? storage_old_chain[i + 1u]
                                    : FAT32_EOC;
                if (fat32_write_fat_entry(storage_old_chain[i], next) < 0) {
                    return -5;
                }
            }
        }

        pos = offset;
        remaining = len;
        while (remaining) {
            uint32_t cluster_index = (uint32_t)(pos / g_storage.cluster_bytes);
            uint32_t cluster_off = (uint32_t)(pos % g_storage.cluster_bytes);
            uint32_t take = min_u32(g_storage.cluster_bytes - cluster_off, remaining);
            if (cluster_index >= clusters_needed) {
                return -5;
            }
            if (cluster_off != 0 || take != g_storage.cluster_bytes) {
                if (cluster_index >= old_count) {
                    /* Newly allocated clusters have no visible contents yet.
                     * Zero only the bytes this partial write does not cover;
                     * avoid an otherwise redundant full-cluster DMA write. */
                    storage_memzero(storage_cluster_buf, g_storage.cluster_bytes);
                } else if (fat32_read_cluster(storage_old_chain[cluster_index],
                                               storage_cluster_buf) < 0) {
                    return -5;
                }
            }
            storage_memcpy(storage_cluster_buf + cluster_off, src, take);
            if (fat32_write_cluster(storage_old_chain[cluster_index], storage_cluster_buf) < 0) {
                return -5;
            }
            pos += take;
            src += take;
            remaining -= take;
        }

        first_cluster = clusters_needed ?
            (cached_append ? node.first_cluster : storage_old_chain[0]) : 0;
        if (storage_parent_path(path, parent, sizeof(parent), name, sizeof(name)) < 0) {
            return -22;
        }
        ret = storage_lookup_path(parent, &parent_node);
        if (ret < 0) {
            return ret;
        }
        ret = fat32_find_dirent_ref_in_dir(parent_node.first_cluster, name, &ref);
        if (ret < 0) {
            return ret;
        }
        ref.dirent.first_cluster_hi = (uint16_t)(first_cluster >> 16);
        ref.dirent.first_cluster_lo = (uint16_t)(first_cluster & 0xffffu);
        ref.dirent.size = final_len;
        if (fat32_update_dirent(&ref) < 0) {
            return -5;
        }
        if (out_written) {
            *out_written = len;
        }
        storage_write_chain_cache.volume = g_active_volume;
        storage_write_chain_cache.first_cluster = first_cluster;
        storage_write_chain_cache.size = final_len;
        storage_write_chain_cache.count = clusters_needed;
        storage_write_chain_cache.tail = clusters_needed ?
            storage_old_chain[clusters_needed - 1u] : 0;
        storage_copy_text(storage_write_chain_cache.path,
                          sizeof(storage_write_chain_cache.path), path);
        storage_write_chain_cache.valid = 1;
        return 0;
    }

    if (g_storage.cluster_bytes == 0 ||
        (uint64_t)total_len > (uint64_t)FAT32_MAX_FILE_CLUSTERS *
                              g_storage.cluster_bytes) {
        return -28;
    }
    pages = total_len ? (uint32_t)((total_len + 4095u) / 4096u) : 1u;
    phys = mm_alloc_pages(pages);
    if (!phys) {
        return -12;
    }
    if (node.size) {
        ret = storage_read_node(&node, 0, (void *)(uintptr_t)phys, (uint32_t)node.size, &read_len);
        if (ret < 0 || read_len != node.size) {
            mm_free_pages(phys, pages);
            return ret < 0 ? ret : -5;
        }
    }
    if (offset > node.size) {
        storage_memzero((uint8_t *)(uintptr_t)phys + node.size, (uint32_t)offset - (uint32_t)node.size);
    }
    if (len) {
        storage_memcpy((uint8_t *)(uintptr_t)phys + offset, buf, len);
    }
    ret = storage_write_file(path, (const void *)(uintptr_t)phys, total_len);
    mm_free_pages(phys, pages);
    if (ret < 0) {
        return ret;
    }
    if (out_written) {
        *out_written = len;
    }
    storage_write_chain_cache.valid = 0;
    return 0;
}

int storage_write_file(const char *path, const void *buf, uint32_t len)
{
    char resolved[RELIEFOS_FS_PATH_LEN];
    char backend_path[RELIEFOS_FS_PATH_LEN];
    char parent[RELIEFOS_FS_PATH_LEN];
    char name[RELIEFOS_FS_NAME_LEN];
    struct storage_node parent_node;
    struct storage_node existing;
    struct fat32_dir_ref ref;
    struct fat32_dir_span span;
    uint8_t short_name[11];
    uint8_t need_lfn = 0;
    uint32_t clusters_needed = 0;
    uint32_t old_count = 0;
    uint32_t data_written = 0;
    uint32_t start_cluster = 0;
    uint8_t creating = 0;
    int ret;
    if (!path || (!buf && len != 0)) {
        return -22;
    }
    storage_write_chain_cache.valid = 0;
    if (!storage_ready()) {
        return -2;
    }
    if (storage_resolve_path("/", path, resolved, sizeof(resolved)) < 0) {
        return -22;
    }
    if (storage_parent_path(resolved, parent, sizeof(parent), name, sizeof(name)) < 0) {
        return -22;
    }
    ret = storage_lookup_path(parent, &parent_node);
    if (ret < 0) {
        return ret;
    }
    if (parent_node.type != RELIEFOS_FS_TYPE_DIR) {
        return -20;
    }
    if (storage_ext4_is_ext_family(&g_storage)) {
        if (storage_backend_path(resolved, backend_path, sizeof(backend_path)) < 0) {
            return -22;
        }
        storage_begin_mutation();
        return storage_ext4_replace(&g_storage, backend_path, buf, len);
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_TMPFS) {
        ret = storage_backend_path(resolved, backend_path, sizeof(backend_path));
        if (ret < 0) return ret;
        storage_begin_mutation();
        ret = tmpfs_lookup(g_storage.tmpfs, backend_path, &existing);
        if (ret == -2) ret = tmpfs_create(g_storage.tmpfs, backend_path, LINUX_S_IFREG | 0666, NULL, &existing);
        if (!ret) ret = tmpfs_truncate(g_storage.tmpfs, existing.first_cluster, 0);
        if (!ret) ret = tmpfs_write(g_storage.tmpfs, existing.first_cluster, 0, buf, len, &data_written);
        return ret;
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_EXFAT) {
        ret = storage_backend_path(resolved, backend_path, sizeof(backend_path));
        if (ret < 0) return ret;
        storage_begin_mutation();
        return exfat_write_file(backend_path, buf, len);
    }
    if (g_storage.filesystem != STORAGE_FILESYSTEM_FAT32) {
        return -30;
    }
    if (parent_node.flags & STORAGE_NODE_FLAG_DEV_DIR) {
        return -21;
    }
    ret = fat32_validate_name(name);
    if (ret < 0) {
        return ret;
    }
    ret = fat32_make_short_name(name, short_name);
    if (ret == 0) {
        char rendered[RELIEFOS_FS_NAME_LEN];
        uint32_t pos = 0;
        for (uint32_t i = 0; i < 8 && short_name[i] != ' '; ++i) {
            char ch = (char)short_name[i];
            if (ch >= 'A' && ch <= 'Z') {
                ch = (char)(ch - 'A' + 'a');
            }
            if (pos + 1 < sizeof(rendered)) {
                rendered[pos++] = ch;
            }
        }
        if (short_name[8] != ' ' && pos + 1 < sizeof(rendered)) {
            rendered[pos++] = '.';
        }
        for (uint32_t i = 8; i < 11 && short_name[i] != ' '; ++i) {
            char ch = (char)short_name[i];
            if (ch >= 'A' && ch <= 'Z') {
                ch = (char)(ch - 'A' + 'a');
            }
            if (pos + 1 < sizeof(rendered)) {
                rendered[pos++] = ch;
            }
        }
        rendered[pos] = 0;
        need_lfn = storage_text_eq(name, rendered) ? 0u : 1u;
    } else {
        need_lfn = 1;
        ret = fat32_make_short_alias(parent_node.first_cluster, name, short_name);
        if (ret < 0) {
            return ret;
        }
    }

    ret = storage_lookup_path(resolved, &existing);
    if (ret == 0) {
        if (existing.type != RELIEFOS_FS_TYPE_FILE) {
            return -21;
        }
        ret = fat32_find_dirent_ref_in_dir(parent_node.first_cluster, name, &ref);
        if (ret < 0) {
            return ret;
        }
    } else if (ret == -2) {
        uint32_t lfn_count = need_lfn ? fat32_lfn_entry_count(name) : 0;
        uint32_t slot_count = lfn_count + 1u;
        if (lfn_count > 20u) {
            return -22;
        }
        creating = 1;
        ret = fat32_find_free_dirent_span(parent_node.first_cluster, slot_count, &span);
        if (ret < 0) {
            return ret;
        }
        storage_memzero(&ref, sizeof(ref));
        ref.entry_cluster = span.entry_cluster;
        ref.entry_offset = span.entry_offset + lfn_count * sizeof(struct fat32_dirent);
        storage_memzero(&ref.dirent, sizeof(ref.dirent));
        storage_memcpy(ref.dirent.name, short_name, 11);
        ref.dirent.attr = storage_is_acl_metadata_name(name)
                               ? (FAT32_ATTR_HIDDEN | FAT32_ATTR_SYSTEM | FAT32_ATTR_ARCHIVE)
                               : FAT32_ATTR_ARCHIVE;
        existing.first_cluster = 0;
        existing.size = 0;
    } else {
        return ret;
    }

    /* Empty replacement only needs the chain head. Avoid collecting the
     * entire old chain before immediately freeing it; on a large file that
     * otherwise performs two full FAT walks during O_TRUNC. */
    if (existing.first_cluster >= 2 && len != 0) {
        ret = fat32_collect_chain(existing.first_cluster, storage_old_chain, &old_count);
        if (ret < 0) {
            return ret;
        }
    }

    /* All discovery above is replayable. The work below changes FAT chains,
     * file clusters or directory entries, so its supporting reads must not
     * return EAGAIN and be misreported to user space as EIO. */
    storage_begin_mutation();

    if (len) {
        clusters_needed = (len + g_storage.cluster_bytes - 1u) / g_storage.cluster_bytes;
        if (clusters_needed > FAT32_MAX_FILE_CLUSTERS) {
            return -28;
        }
        for (uint32_t i = 0; i < clusters_needed; ++i) {
            if (i < old_count) {
                storage_new_chain[i] = storage_old_chain[i];
            } else {
                ret = fat32_find_free_cluster(&storage_new_chain[i]);
                if (ret < 0) {
                    return ret;
                }
                if (fat32_write_fat_entry(storage_new_chain[i], FAT32_EOC) < 0) {
                    return -5;
                }
                if (fat32_note_allocated() < 0) {
                    return -5;
                }
            }
        }
        for (uint32_t i = 0; i < clusters_needed; ++i) {
            uint32_t next = (i + 1u < clusters_needed) ? storage_new_chain[i + 1u] : FAT32_EOC;
            if (fat32_write_fat_entry(storage_new_chain[i], next) < 0) {
                return -5;
            }
        }
        start_cluster = storage_new_chain[0];
        for (uint32_t i = 0; i < clusters_needed; ++i) {
            uint32_t take = min_u32(g_storage.cluster_bytes, len - data_written);
            storage_memzero(storage_cluster_buf, g_storage.cluster_bytes);
            storage_memcpy(storage_cluster_buf, (const uint8_t *)buf + data_written, take);
            if (fat32_write_cluster(storage_new_chain[i], storage_cluster_buf) < 0) {
                return -5;
            }
            data_written += take;
        }
    }

    if (len == 0 && existing.first_cluster >= 2) {
        if (fat32_free_chain(existing.first_cluster) < 0) {
            return -5;
        }
    } else if (old_count > clusters_needed) {
        if (fat32_free_chain(storage_old_chain[clusters_needed]) < 0) {
            return -5;
        }
    }

    ref.dirent.first_cluster_hi = (uint16_t)(start_cluster >> 16);
    ref.dirent.first_cluster_lo = (uint16_t)(start_cluster & 0xffffu);
    ref.dirent.size = len;
    if (creating) {
        uint32_t lfn_count = need_lfn ? fat32_lfn_entry_count(name) : 0;
        if (fat32_read_cluster(span.entry_cluster, storage_cluster_buf) < 0) {
            return -5;
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
        *(struct fat32_dirent *)(void *)(storage_cluster_buf + ref.entry_offset) = ref.dirent;
        if (fat32_write_cluster(span.entry_cluster, storage_cluster_buf) < 0) {
            return -5;
        }
    } else if (fat32_update_dirent(&ref) < 0) {
        return -5;
    }
    return 0;
}

int storage_truncate_file(const char *path, uint64_t length)
{
    struct storage_node node;
    uint64_t phys;
    uint32_t pages;
    uint32_t got = 0;
    uint32_t target;
    int ret;

    if (!path) {
        return -22;
    }
    ret = storage_lookup_path(path, &node);
    if (ret < 0) {
        return ret;
    }
    if (node.type != RELIEFOS_FS_TYPE_FILE) {
        return -21;
    }
    if (storage_ext4_is_ext_family(&g_storage)) {
        storage_begin_mutation();
        return storage_ext4_ops.truncate(&g_storage, &node, length);
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_TMPFS) {
        storage_begin_mutation();
        return tmpfs_truncate(g_storage.tmpfs, node.first_cluster, length);
    }
    if (length > 0xffffffffULL) return -22;
    if (g_storage.filesystem == STORAGE_FILESYSTEM_EXFAT) {
        char backend_path[RELIEFOS_FS_PATH_LEN];
        ret = storage_backend_path(path, backend_path, sizeof(backend_path));
        if (ret < 0) return ret;
        storage_begin_mutation();
        return exfat_truncate_file(backend_path, length);
    }
    if (g_storage.filesystem != STORAGE_FILESYSTEM_FAT32) {
        return -30;
    }
    target = (uint32_t)length;
    if (target == node.size) {
        return 0;
    }
    pages = target ? (target + 4095u) / 4096u : 1u;
    phys = mm_alloc_pages(pages);
    if (!phys) {
        return -12;
    }
    storage_memzero((void *)(uintptr_t)phys, pages * 4096u);
    if (node.size && target) {
        uint32_t copy = node.size < target ? (uint32_t)node.size : target;
        ret = storage_read_node(&node, 0, (void *)(uintptr_t)phys, copy, &got);
        if (ret < 0 || got != copy) {
            mm_free_pages(phys, pages);
            return ret < 0 ? ret : -5;
        }
    }
    ret = storage_write_file(path, (const void *)(uintptr_t)phys, target);
    mm_free_pages(phys, pages);
    return ret;
}
