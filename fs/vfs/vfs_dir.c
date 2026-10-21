#include "internal.h"

static int storage_readdir_node_unlocked(const struct storage_node *node, uint64_t *cursor,
                         struct reliefos_dir_entry *entry);


static int storage_readdir_node_unlocked(const struct storage_node *node, uint64_t *cursor,
                         struct reliefos_dir_entry *entry)
{
    struct storage_volume *old_volume = 0;
    int ret;
    if (!node || !cursor || !entry) {
        return -22;
    }
    if (node->type != RELIEFOS_FS_TYPE_DIR) {
        return -20;
    }
    if (node->flags & STORAGE_NODE_FLAG_DEV_DIR) {
        const struct storage_dev_entry *entries = storage_dev_entries;
        uint32_t count = (uint32_t)(sizeof(storage_dev_entries) /
                                     sizeof(storage_dev_entries[0]));
        if (node->first_cluster == STORAGE_DEV_KIND_INPUT_DIR) {
            entries = storage_dev_input_entries;
            count = (uint32_t)(sizeof(storage_dev_input_entries) /
                               sizeof(storage_dev_input_entries[0]));
        } else if (node->first_cluster == STORAGE_DEV_KIND_PTS_DIR) {
            return 0;
        }
        if (node->first_cluster == STORAGE_DEV_KIND_DISK_DIR) {
            if (*cursor) return 0;
            ++*cursor;
            entry->type = RELIEFOS_FS_TYPE_DIR;
            storage_copy_text(entry->name, sizeof(entry->name), "by-partuuid");
            return 1;
        }
        if (node->first_cluster == STORAGE_DEV_KIND_PARTUUID_DIR) {
            char uuid[37], target[48];
            int step = storage_devlink_next(cursor, uuid, target);
            if (step <= 0) return step;
            entry->type = RELIEFOS_FS_TYPE_SYMLINK;
            storage_copy_text(entry->name, sizeof(entry->name), uuid);
            return 1;
        }
        if (node->first_cluster == STORAGE_DEV_KIND_SND_DIR) {
            return storage_audio_snd_readdir(cursor, entry);
        }
        if (node->first_cluster == STORAGE_DEV_KIND_DRI_DIR) {
            if (!drm_device_is_registered()) return 0;
            if (*cursor == 0) {
                ++*cursor;
                entry->type = RELIEFOS_FS_TYPE_DEVICE;
                storage_copy_text(entry->name, sizeof(entry->name), "card0");
                return 1;
            }
            if (*cursor == 1 && drm_device_render_node_ready()) {
                ++*cursor;
                entry->type = RELIEFOS_FS_TYPE_DEVICE;
                storage_copy_text(entry->name, sizeof(entry->name),
                                  "renderD128");
                return 1;
            }
            return 0;
        }
        while (*cursor < count) {
            const struct storage_dev_entry *dev = &entries[*cursor];
            ++(*cursor);
            entry->type = dev->type;
            storage_copy_text(entry->name, sizeof(entry->name), dev->name);
            return 1;
        }
        if (node->first_cluster == STORAGE_DEV_KIND_DIR) {
            /* Expose the Linux block namespace generated from discovered
             * transport identities. */
            uint64_t dynamic = *cursor - count;
            if (drm_device_is_registered()) {
                if (!dynamic) {
                    ++*cursor;
                    entry->type = RELIEFOS_FS_TYPE_DIR;
                    storage_copy_text(entry->name, sizeof(entry->name), "dri");
                    return 1;
                }
                --dynamic;
            }
            for (uint32_t disk_id = 0; disk_id < g_install_disk_count;
                 ++disk_id) {
                char device_name[RELIEFOS_FS_PATH_LEN];
                uint64_t first_lba;
                uint64_t sectors;
                int block_ret = storage_disk_block_info(disk_id, -1, &first_lba, &sectors);
                if (block_ret < 0) {
                    if (block_ret == -2) continue;
                    return block_ret;
                }
                if (dynamic == 0) {
                    if (storage_disk_device_name(disk_id, -1, device_name,
                                                 sizeof(device_name)) < 0) return -22;
                    ++(*cursor);
                    entry->type = RELIEFOS_FS_TYPE_DEVICE;
                    storage_copy_text(entry->name, sizeof(entry->name), device_name + 5);
                    return 1;
                }
                --dynamic;
                for (uint32_t part = 0; part < RELIEFOS_DISK_MAX_PARTITIONS; ++part) {
                    block_ret = storage_disk_block_info(disk_id, (int32_t)part,
                                                        &first_lba, &sectors);
                    if (block_ret < 0) {
                        if (block_ret == -2) continue;
                        return block_ret;
                    }
                    if (dynamic == 0) {
                        if (storage_disk_device_name(disk_id, (int32_t)part,
                                                     device_name, sizeof(device_name)) < 0)
                            return -22;
                        ++(*cursor);
                        entry->type = RELIEFOS_FS_TYPE_DEVICE;
                        storage_copy_text(entry->name, sizeof(entry->name), device_name + 5);
                        return 1;
                    }
                    --dynamic;
                }
            }
        }
        return 0;
    }
    ret = storage_select_node_volume(node, &old_volume);
    if (ret < 0) {
        return ret;
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_ISO9660) {
        ret = iso9660_iter_dir_entry(node->first_cluster, (uint32_t)node->size,
                                     *cursor, entry);
        storage_restore_volume(old_volume);
        if (ret == 0) {
            ++(*cursor);
            return 1;
        }
        if (ret == -2) {
            return 0;
        }
        return ret;
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_TMPFS) {
        ret = tmpfs_readdir(g_storage.tmpfs, node->first_cluster, cursor, entry);
        storage_restore_volume(old_volume);
        return ret;
    }
    if (storage_ext4_is_ext_family(&g_storage)) {
        ret = storage_ext4_ops.readdir(&g_storage, node, cursor, entry);
        storage_restore_volume(old_volume);
        return ret;
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_EXFAT) {
        ret = exfat_iter_dir_entry(node->first_cluster,
                                   (node->flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                   *cursor, entry);
        storage_restore_volume(old_volume);
        if (ret == 0) {
            ++(*cursor);
            return 1;
        }
        return ret == -2 ? 0 : ret;
    }
    ret = fat32_iter_dir_entry(node->first_cluster, *cursor, entry);
    storage_restore_volume(old_volume);
    if (ret == 0) {
        ++(*cursor);
        return 1;
    }
    if (ret == -2) {
        return 0;
    }
    return ret;
}

int storage_readdir_node(const struct storage_node *node, uint64_t *cursor,
                         struct reliefos_dir_entry *entry)
{
    int ret;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    ret = storage_readdir_node_unlocked(node, cursor, entry);
    kernel_execution_unlock_irqrestore(flags);
    return ret;
}

int storage_list_dir(const char *path, struct reliefos_dir_entry *entries,
                     uint32_t capacity, uint32_t *out_count)
{
    struct storage_node node;
    char resolved[RELIEFOS_FS_PATH_LEN];
    uint64_t cursor = 0;
    uint32_t count = 0;
    if (!out_count) {
        return -22;
    }
    *out_count = 0;
    int ret = storage_lookup_path(path, &node);
    if (ret < 0) {
        return ret;
    }
    if (node.type != RELIEFOS_FS_TYPE_DIR) {
        return -20;
    }
    while (count < capacity) {
        struct reliefos_dir_entry tmp;
        int step = storage_readdir_node(&node, &cursor, &tmp);
        if (step < 0) {
            return step;
        }
        if (step == 0) {
            break;
        }
        if (entries) {
            entries[count] = tmp;
        }
        ++count;
    }
    if (g_devfs_enabled && storage_resolve_path("/", path, resolved,
                                                sizeof(resolved)) == 0 &&
        storage_text_eq_ci(resolved, "/") && count < capacity) {
        if (entries) {
            entries[count].type = RELIEFOS_FS_TYPE_DIR;
            storage_copy_text(entries[count].name, sizeof(entries[count].name), "dev");
        }
        ++count;
    }
    *out_count = count;
    return 0;
}
