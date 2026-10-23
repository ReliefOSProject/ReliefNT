#include "../../block/core/storage_internal.h"
int storage_tmpfs_get_page(const struct storage_node *node, uint64_t offset, uint64_t *phys)
{
    if (!node || !(node->flags & STORAGE_NODE_FLAG_TMPFS))
        return -22;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    struct storage_volume *previous = NULL;
    int ret = storage_select_node_volume(node, &previous);
    if (!ret) ret = g_storage.tmpfs ? tmpfs_get_page(g_storage.tmpfs, node->first_cluster, offset, phys) : -2;
    storage_restore_volume(previous);
    kernel_execution_unlock_irqrestore(flags);
    return ret;
}

/**
 * @brief Create an internal anonymous file and transfer its inode lifetime to the caller.
 * @param uid File owner UID.
 * @param gid File owner GID.
 * @param node Required output for the newly allocated backing node.
 * @param reference Required output receiving one owned inode reference.
 * @return Zero on success or negative errno. Serializes through the execution
 * lock; the internal filesystem consumes no user mount slot or disk owner.
 */
int storage_shmem_create(uint32_t uid, uint32_t gid, struct storage_node *node,
                          struct storage_inode_ref **reference)
{
    if (!node || !reference) return -22;
    *reference = NULL;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    int ret = 0;
    if (!g_shmem_volume.ready) {
        struct tmpfs_super *fs = NULL;
        ret = tmpfs_new(STORAGE_SHMEM_VOLUME, 0, "size=0,nr_inodes=0", 0, 0, &fs);
        if (ret < 0) goto out;
        g_shmem_volume = (struct storage_volume){.volume_id = STORAGE_SHMEM_VOLUME,
            .kind = STORAGE_VOLUME_RAM, .filesystem = STORAGE_FILESYSTEM_TMPFS,
            .tmpfs = fs, .ready = true};
    }
    ret = tmpfs_create_anonymous(g_shmem_volume.tmpfs, uid, gid, node);
    if (!ret) {
        ret = storage_inode_get(node, reference);
        tmpfs_drop(g_shmem_volume.tmpfs, node->first_cluster);
    }
out:
    kernel_execution_unlock_irqrestore(flags);
    return ret;
}

int storage_mount_tmpfs(const char *source, const char *target, uint64_t mount_flags, const char *options,
                        uint32_t uid, uint32_t gid)
{
    if (!target || !source || storage_strlen(source) >= RELIEFOS_FS_PATH_LEN)
        return -22;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    struct storage_node node;
    struct storage_volume *previous = g_active_volume;
    int ret = storage_lookup_path(target, &node);
    if (ret < 0)
        goto out;
    if (node.type != RELIEFOS_FS_TYPE_DIR) {
        ret = -20;
        goto out;
    }
    for (uint32_t i = 0; i < STORAGE_MAX_VOLUMES; ++i)
        if (g_volumes[i].ready && storage_text_eq(g_volumes[i].mount_path, target)) {
            ret = -16;
            goto out;
        }
    uint32_t slot = STORAGE_VOLUME_DYNAMIC_FIRST;
    while (slot < STORAGE_MAX_VOLUMES && g_volumes[slot].ready)
        ++slot;
    if (slot == STORAGE_MAX_VOLUMES) {
        ret = -28;
        goto out;
    }
    struct tmpfs_super *fs = NULL;
    ret = tmpfs_new(slot, mount_flags, options, uid, gid, &fs);
    if (ret < 0)
        goto out;
    struct storage_volume *volume = &g_volumes[slot];
    storage_memzero(volume, sizeof(*volume));
    volume->volume_id = slot;
    volume->kind = STORAGE_VOLUME_RAM;
    volume->filesystem = STORAGE_FILESYSTEM_TMPFS;
    volume->mount_flags = mount_flags;
    volume->tmpfs = fs;
    storage_copy_text(volume->mount_path, sizeof(volume->mount_path), target);
    storage_copy_text(volume->tmpfs_source, sizeof(volume->tmpfs_source), source[0] ? source : "none");
    volume->ready = true;
    storage_cache_invalidate();
out:
    storage_restore_volume(previous);
    kernel_execution_unlock_irqrestore(flags);
    return ret;
}
