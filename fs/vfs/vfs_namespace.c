#include "internal.h"



int storage_mkdir(const char *path)
{
    char resolved[RELIEFOS_FS_PATH_LEN];
    char backend_path[RELIEFOS_FS_PATH_LEN];
    char parent[RELIEFOS_FS_PATH_LEN];
    char name[RELIEFOS_FS_NAME_LEN];
    struct storage_node parent_node;
    struct storage_node existing;
    uint32_t cluster = 0;
    int ret;
    if (!path) {
        return -22;
    }
    if (!storage_ready()) {
        return -2;
    }
    if (storage_resolve_path("/", path, resolved, sizeof(resolved)) < 0) {
        return -22;
    }
    /* A mounted directory resolves to the backend root, which has no final
     * component to create. Test existence in the global namespace first. */
    ret = storage_lookup_path(resolved, &existing);
    if (ret == 0) {
        return -17;
    }
    if (ret != -2) {
        return ret;
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
        return storage_ext4_ops.mkdir(&g_storage, backend_path);
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_TMPFS) {
        ret = storage_backend_path(resolved, backend_path, sizeof(backend_path));
        if (ret < 0) return ret;
        storage_begin_mutation();
        return tmpfs_create(g_storage.tmpfs, backend_path, LINUX_S_IFDIR | 0777, NULL, NULL);
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_EXFAT) {
        ret = storage_backend_path(resolved, backend_path, sizeof(backend_path));
        if (ret < 0) return ret;
        storage_begin_mutation();
        return exfat_mkdir(backend_path);
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
    ret = fat32_find_free_cluster(&cluster);
    if (ret < 0) {
        return ret;
    }
    storage_begin_mutation();
    if (fat32_write_fat_entry(cluster, FAT32_EOC) < 0) {
        return -5;
    }
    if (fat32_note_allocated() < 0) {
        return -5;
    }
    storage_memzero(storage_cluster_buf, g_storage.cluster_bytes);
    {
        struct fat32_dirent *dot = (struct fat32_dirent *)(void *)storage_cluster_buf;
        struct fat32_dirent *dotdot =
            (struct fat32_dirent *)(void *)(storage_cluster_buf + sizeof(struct fat32_dirent));
        storage_memzero(dot, sizeof(*dot));
        storage_memzero(dotdot, sizeof(*dotdot));
        dot->name[0] = '.';
        for (uint32_t i = 1; i < 11; ++i) {
            dot->name[i] = ' ';
        }
        dot->attr = FAT32_ATTR_DIRECTORY;
        dot->first_cluster_hi = (uint16_t)(cluster >> 16);
        dot->first_cluster_lo = (uint16_t)(cluster & 0xffffu);
        dotdot->name[0] = '.';
        dotdot->name[1] = '.';
        for (uint32_t i = 2; i < 11; ++i) {
            dotdot->name[i] = ' ';
        }
        dotdot->attr = FAT32_ATTR_DIRECTORY;
        /* FAT32 encodes the parent of a root child as cluster zero, not the
         * root directory's physical cluster number. */
        uint32_t dotdot_cluster = (parent_node.flags & STORAGE_NODE_FLAG_ROOT)
                                      ? 0u : parent_node.first_cluster;
        dotdot->first_cluster_hi = (uint16_t)(dotdot_cluster >> 16);
        dotdot->first_cluster_lo = (uint16_t)(dotdot_cluster & 0xffffu);
    }
    if (fat32_write_cluster(cluster, storage_cluster_buf) < 0) {
        (void)fat32_write_fat_entry(cluster, 0);
        (void)fat32_note_freed();
        return -5;
    }
    ret = fat32_create_dirent(parent_node.first_cluster, name, FAT32_ATTR_DIRECTORY, cluster, 0);
    if (ret < 0) {
        (void)fat32_free_chain(cluster);
        return ret;
    }
    return 0;
}

int storage_unlink(const char *path)
{
    char resolved[RELIEFOS_FS_PATH_LEN];
    char backend_path[RELIEFOS_FS_PATH_LEN];
    char parent[RELIEFOS_FS_PATH_LEN];
    char name[RELIEFOS_FS_NAME_LEN];
    struct storage_node parent_node;
    struct storage_node node;
    struct fat32_dirent deleted;
    uint32_t first_cluster;
    int ret;
    if (!path) {
        return -22;
    }
    if (!storage_ready()) {
        return -2;
    }
    storage_write_chain_cache.valid = 0;
    if (storage_resolve_path("/", path, resolved, sizeof(resolved)) < 0 ||
        storage_parent_path(resolved, parent, sizeof(parent), name, sizeof(name)) < 0) {
        return -22;
    }
    ret = storage_lookup_path(parent, &parent_node);
    if (ret < 0) {
        return ret;
    }
    if (parent_node.type != RELIEFOS_FS_TYPE_DIR || (parent_node.flags & STORAGE_NODE_FLAG_DEV_DIR)) {
        return -20;
    }
    ret = storage_lookup_path(resolved, &node);
    if (ret < 0) {
        return ret;
    }
    if (storage_ext4_is_ext_family(&g_storage)) {
        if (node.type == RELIEFOS_FS_TYPE_DIR) return -21;
        if (storage_backend_path(resolved, backend_path, sizeof(backend_path)) < 0) {
            return -22;
        }
        storage_begin_mutation();
        return storage_ext4_ops.unlink(&g_storage, backend_path);
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_TMPFS) {
        ret = storage_backend_path(resolved, backend_path, sizeof(backend_path));
        if (ret < 0) return ret;
        storage_begin_mutation();
        return tmpfs_unlink(g_storage.tmpfs, backend_path, false);
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_EXFAT) {
        if (node.type == RELIEFOS_FS_TYPE_DIR) return -21;
        ret = storage_backend_path(resolved, backend_path, sizeof(backend_path));
        if (ret < 0) return ret;
        storage_begin_mutation();
        return exfat_unlink(backend_path);
    }
    if (g_storage.filesystem != STORAGE_FILESYSTEM_FAT32) {
        return -30;
    }
    if (node.type == RELIEFOS_FS_TYPE_DIR) {
        return -21;
    }
    storage_begin_mutation();
    ret = fat32_delete_dirent(parent_node.first_cluster, name, &deleted);
    if (ret < 0) {
        return ret;
    }
    first_cluster = ((uint32_t)deleted.first_cluster_hi << 16) | deleted.first_cluster_lo;
    if (first_cluster >= 2) {
        ret = fat32_free_chain(first_cluster);
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

int storage_write_boot_esp_file(const char *path, const void *buf, uint32_t len)
{
    if (!path || !storage_mount_path_matches(path, "/boot")) {
        return -22;
    }
    (void)storage_mkdir("/boot/reliefos");
    (void)storage_mkdir("/boot/reliefos/state");
    return storage_write_file(path, buf, len);
}

int storage_unlink_boot_esp_file(const char *path)
{
    int ret;
    if (!path || !storage_mount_path_matches(path, "/boot")) {
        return -22;
    }
    ret = storage_unlink(path);
    return ret == -2 ? 0 : ret;
}

int storage_rmdir(const char *path)
{
    char resolved[RELIEFOS_FS_PATH_LEN];
    char backend_path[RELIEFOS_FS_PATH_LEN];
    char parent[RELIEFOS_FS_PATH_LEN];
    char name[RELIEFOS_FS_NAME_LEN];
    struct storage_node parent_node;
    struct storage_node node;
    struct fat32_dirent deleted;
    int empty;
    int ret;
    if (!path) {
        return -22;
    }
    if (!storage_ready()) {
        return -2;
    }
    storage_write_chain_cache.valid = 0;
    if (storage_resolve_path("/", path, resolved, sizeof(resolved)) < 0 ||
        storage_parent_path(resolved, parent, sizeof(parent), name, sizeof(name)) < 0) {
        return -22;
    }
    if (storage_text_eq_ci(resolved, "/") ||
        (g_devfs_enabled && storage_text_eq_ci(resolved, "/dev"))) {
        return -22;
    }
    ret = storage_lookup_path(parent, &parent_node);
    if (ret < 0) {
        return ret;
    }
    if (parent_node.type != RELIEFOS_FS_TYPE_DIR || (parent_node.flags & STORAGE_NODE_FLAG_DEV_DIR)) {
        return -20;
    }
    ret = storage_lookup_path(resolved, &node);
    if (ret < 0) {
        return ret;
    }
    if (storage_ext4_is_ext_family(&g_storage)) {
        if (node.type != RELIEFOS_FS_TYPE_DIR) return -20;
        if (storage_backend_path(resolved, backend_path, sizeof(backend_path)) < 0) {
            return -22;
        }
        storage_begin_mutation();
        return storage_ext4_ops.rmdir(&g_storage, backend_path);
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_TMPFS) {
        ret = storage_backend_path(resolved, backend_path, sizeof(backend_path));
        if (ret < 0) return ret;
        storage_begin_mutation();
        return tmpfs_unlink(g_storage.tmpfs, backend_path, true);
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_EXFAT) {
        if (node.type != RELIEFOS_FS_TYPE_DIR) return -20;
        ret = storage_backend_path(resolved, backend_path, sizeof(backend_path));
        if (ret < 0) return ret;
        storage_begin_mutation();
        return exfat_rmdir(backend_path);
    }
    if (g_storage.filesystem != STORAGE_FILESYSTEM_FAT32) {
        return -30;
    }
    if (node.type != RELIEFOS_FS_TYPE_DIR || (node.flags & STORAGE_NODE_FLAG_DEV_DIR)) {
        return -20;
    }
    empty = fat32_dir_is_empty(node.first_cluster);
    if (empty < 0) {
        return empty;
    }
    if (!empty) {
        return -39;
    }
    storage_begin_mutation();
    ret = fat32_delete_acl_metadata_file(node.first_cluster);
    if (ret < 0) {
        return ret;
    }
    ret = fat32_delete_dirent(parent_node.first_cluster, name, &deleted);
    if (ret < 0) {
        return ret;
    }
    if (node.first_cluster >= 2) {
        ret = fat32_free_chain(node.first_cluster);
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

int storage_rename(const char *old_path, const char *new_path)
{
    char old_resolved[RELIEFOS_FS_PATH_LEN];
    char new_resolved[RELIEFOS_FS_PATH_LEN];
    char old_backend_path[RELIEFOS_FS_PATH_LEN];
    char new_backend_path[RELIEFOS_FS_PATH_LEN];
    char old_parent[RELIEFOS_FS_PATH_LEN];
    char new_parent[RELIEFOS_FS_PATH_LEN];
    char old_name[RELIEFOS_FS_NAME_LEN];
    char new_name[RELIEFOS_FS_NAME_LEN];
    struct storage_node parent_node;
    struct storage_node node;
    struct storage_node existing;
    struct fat32_dirent deleted;
    uint32_t first_cluster;
    int ret;
    if (!old_path || !new_path) {
        return -22;
    }
    if (!storage_ready()) {
        return -2;
    }
    if (storage_resolve_path("/", old_path, old_resolved, sizeof(old_resolved)) < 0 ||
        storage_resolve_path("/", new_path, new_resolved, sizeof(new_resolved)) < 0 ||
        storage_parent_path(old_resolved, old_parent, sizeof(old_parent), old_name, sizeof(old_name)) < 0 ||
        storage_parent_path(new_resolved, new_parent, sizeof(new_parent), new_name, sizeof(new_name)) < 0) {
        return -22;
    }
    struct storage_volume *old_tmp_volume, *new_tmp_volume;
    ret = storage_route_path(old_resolved, &old_tmp_volume, old_backend_path, sizeof(old_backend_path));
    if (ret < 0) return ret;
    ret = storage_route_path(new_resolved, &new_tmp_volume, new_backend_path, sizeof(new_backend_path));
    if (ret < 0) return ret;
    if (old_tmp_volume != new_tmp_volume) return -18;
    if (old_tmp_volume->filesystem == STORAGE_FILESYSTEM_TMPFS) {
        storage_begin_mutation();
        return tmpfs_rename(old_tmp_volume->tmpfs, old_backend_path, new_backend_path);
    }
    if (storage_ext4_is_ext_family(old_tmp_volume)) {
        storage_begin_mutation();
        return storage_ext4_ops.rename(old_tmp_volume, old_backend_path, new_backend_path);
    }
    if (!storage_text_eq_ci(old_parent, new_parent)) {
        return -22;
    }
    ret = fat32_validate_name(new_name);
    if (ret < 0) {
        return ret;
    }
    ret = storage_lookup_path(old_parent, &parent_node);
    if (ret < 0) {
        return ret;
    }
    if (parent_node.type != RELIEFOS_FS_TYPE_DIR || (parent_node.flags & STORAGE_NODE_FLAG_DEV_DIR)) {
        return -20;
    }
    ret = storage_lookup_path(old_resolved, &node);
    if (ret < 0) return ret;
    if (storage_ext4_is_ext_family(&g_storage)) {
        struct storage_volume *old_volume;
        struct storage_volume *new_volume;
        if (storage_route_path(old_resolved, &old_volume, old_backend_path,
                               sizeof(old_backend_path)) < 0 ||
            storage_route_path(new_resolved, &new_volume, new_backend_path,
                               sizeof(new_backend_path)) < 0 ||
            old_volume->volume_id != new_volume->volume_id) {
            return -18;
        }
        storage_begin_mutation();
        return storage_ext4_ops.rename(&g_storage, old_backend_path, new_backend_path);
    }
    if (g_storage.filesystem == STORAGE_FILESYSTEM_EXFAT) {
        struct storage_volume *old_volume;
        struct storage_volume *new_volume;
        ret = storage_route_path(old_resolved, &old_volume, old_backend_path,
                                 sizeof(old_backend_path));
        if (ret < 0) return ret;
        ret = storage_route_path(new_resolved, &new_volume, new_backend_path,
                                 sizeof(new_backend_path));
        if (ret < 0) return ret;
        if (old_volume->volume_id != new_volume->volume_id) return -18;
        ret = storage_select_volume(old_volume->volume_id);
        if (ret < 0) return ret;
        storage_begin_mutation();
        return exfat_rename(old_backend_path, new_backend_path);
    }
    if (g_storage.filesystem != STORAGE_FILESYSTEM_FAT32) {
        return -30;
    }
    if (storage_text_eq_ci(old_name, new_name)) return 0;
    ret = storage_lookup_path(old_resolved, &node);
    if (ret < 0) {
        return ret;
    }
    ret = storage_lookup_path(new_resolved, &existing);
    if (ret == 0) {
        struct fat32_dir_ref target;
        if (node.type != existing.type) return node.type == RELIEFOS_FS_TYPE_DIR ? -20 : -21;
        if (existing.type == RELIEFOS_FS_TYPE_DIR) {
            ret = fat32_dir_is_empty(existing.first_cluster);
            if (ret <= 0) return ret < 0 ? ret : -39;
        }
        ret = fat32_find_dirent_ref_in_dir(parent_node.first_cluster, new_name, &target);
        if (ret < 0) return ret;
        struct fat32_dirent saved = target.dirent;
        target.dirent.first_cluster_hi = (uint16_t)(node.first_cluster >> 16);
        target.dirent.first_cluster_lo = (uint16_t)node.first_cluster;
        target.dirent.size = node.type == RELIEFOS_FS_TYPE_FILE ? (uint32_t)node.size : 0;
        storage_begin_mutation();
        ret = fat32_update_dirent(&target);
        if (ret < 0) return ret;
        ret = fat32_delete_dirent(parent_node.first_cluster, old_name, &deleted);
        if (ret < 0) {
            target.dirent = saved;
            (void)fat32_update_dirent(&target);
            return ret;
        }
        if (existing.type == RELIEFOS_FS_TYPE_DIR) ret = fat32_delete_acl_metadata_file(existing.first_cluster);
        if (!ret && existing.first_cluster >= 2) ret = fat32_free_chain(existing.first_cluster);
        storage_cache_invalidate();
        return ret;
    }
    if (ret != -2) {
        return ret;
    }
    first_cluster = node.first_cluster;
    storage_begin_mutation();
    ret = fat32_create_dirent(parent_node.first_cluster, new_name,
                              node.type == RELIEFOS_FS_TYPE_DIR ? FAT32_ATTR_DIRECTORY : FAT32_ATTR_ARCHIVE,
                              first_cluster, node.type == RELIEFOS_FS_TYPE_FILE ? (uint32_t)node.size : 0);
    if (ret < 0) {
        return ret;
    }
    ret = fat32_delete_dirent(parent_node.first_cluster, old_name, &deleted);
    if (ret < 0) {
        (void)fat32_delete_dirent(parent_node.first_cluster, new_name, 0);
        return ret;
    }
    return 0;
}

int storage_link(const char *old_path, const char *new_path)
{
    char old_resolved[RELIEFOS_FS_PATH_LEN];
    char new_resolved[RELIEFOS_FS_PATH_LEN];
    char old_backend_path[RELIEFOS_FS_PATH_LEN];
    char new_backend_path[RELIEFOS_FS_PATH_LEN];
    struct storage_volume *old_volume;
    struct storage_volume *new_volume;
    int ret;
    if (!old_path || !new_path || !storage_ready()) return -22;
    ret = storage_resolve_path("/", old_path, old_resolved, sizeof(old_resolved));
    if (ret < 0) return ret;
    ret = storage_resolve_path("/", new_path, new_resolved, sizeof(new_resolved));
    if (ret < 0) return ret;
    ret = storage_route_path(old_resolved, &old_volume, old_backend_path,
                             sizeof(old_backend_path));
    if (ret < 0) return ret;
    ret = storage_route_path(new_resolved, &new_volume, new_backend_path,
                             sizeof(new_backend_path));
    if (ret < 0) return ret;
    if (old_volume->volume_id != new_volume->volume_id) return -18;
    ret = storage_select_volume(old_volume->volume_id);
    if (ret < 0) return ret;
    if (g_storage.filesystem == STORAGE_FILESYSTEM_TMPFS) {
        storage_begin_mutation();
        return tmpfs_link(g_storage.tmpfs, old_backend_path, new_backend_path);
    }
    if (!storage_ext4_is_ext_family(&g_storage)) return -95;
    storage_begin_mutation();
    return storage_ext4_ops.link(&g_storage, old_backend_path, new_backend_path);
}

int storage_symlink(const char *target, const char *path)
{
    char resolved[RELIEFOS_FS_PATH_LEN];
    char backend_path[RELIEFOS_FS_PATH_LEN];
    struct storage_volume *volume;
    struct storage_volume *previous;
    uint64_t irq_flags;
    int ret;
    if (!target || !path || !storage_ready()) return -22;
    kernel_execution_lock_irqsave(&irq_flags);
    previous = g_active_volume;
    ret = storage_resolve_path("/", path, resolved, sizeof(resolved));
    if (ret < 0) goto out;
    ret = storage_route_path(resolved, &volume, backend_path, sizeof(backend_path));
    if (ret < 0) goto out;
    ret = storage_select_volume(volume->volume_id);
    if (ret < 0) goto out;
    if (g_storage.filesystem == STORAGE_FILESYSTEM_TMPFS) {
        storage_begin_mutation();
        ret = tmpfs_create(g_storage.tmpfs, backend_path, LINUX_S_IFLNK | 0777, target, NULL);
        goto out;
    }
    if (!storage_ext4_is_ext_family(&g_storage)) { ret = -1; goto out; }
    storage_begin_mutation();
    ret = storage_ext4_ops.symlink(&g_storage, target, backend_path);
    if (!ret) {
        struct reliefos_time_info now;
        struct storage_node node;
        if (time_wall_clock(&now) == 0 && storage_ext4_ops.lookup(&g_storage, backend_path, &node) == 0)
            ret = storage_inode_utimensat(&node, now.unix_seconds, now.unix_seconds, true, true);
        if (ret < 0) (void)storage_ext4_ops.unlink(&g_storage, backend_path);
    }
out:
    storage_restore_volume(previous);
    kernel_execution_unlock_irqrestore(irq_flags);
    return ret;
}

int storage_readlink(const char *path, char *buffer, uint32_t capacity, uint32_t *out_len)
{
    struct storage_node node;
    struct storage_volume *previous;
    uint64_t irq_flags;
    int ret;
    if (out_len) *out_len = 0;
    if (!path || !buffer || !capacity) return -22;
    kernel_execution_lock_irqsave(&irq_flags);
    previous = g_active_volume;
    ret = storage_lookup_path_unlocked(path, &node);
    if (!ret && node.type != RELIEFOS_FS_TYPE_SYMLINK) ret = -22;
    if (!ret && (node.flags & STORAGE_NODE_FLAG_DEV_LINK)) {
        char target[48];
        ret = storage_devlink_target(path, target);
        if (!ret) {
            uint32_t length = storage_strlen(target);
            if (length > capacity) length = capacity;
            __builtin_memcpy(buffer, target, length);
            if (out_len) *out_len = length;
        }
    } else if (!ret && (node.flags & STORAGE_NODE_FLAG_TMPFS))
        ret = tmpfs_readlink(g_storage.tmpfs, node.first_cluster, buffer, capacity, out_len);
    else if (!ret) ret = storage_ext4_ops.readlink(&g_storage, &node, buffer, capacity, out_len);
    storage_restore_volume(previous);
    kernel_execution_unlock_irqrestore(irq_flags);
    return ret;
}
