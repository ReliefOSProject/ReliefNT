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

int storage_tmpfs_fallocate(struct storage_node *node,uint32_t mode,uint64_t offset,uint64_t length)
{
    if(!node || !(node->flags&STORAGE_NODE_FLAG_TMPFS))return -22;
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    struct storage_volume *previous=NULL;int rc=storage_select_node_volume(node,&previous);
    if(!rc)rc=g_storage.tmpfs ? tmpfs_fallocate(g_storage.tmpfs,node->first_cluster,mode,offset,length) : -2;
    if(!rc) {
        struct linux_stat_abi value;rc=tmpfs_stat(g_storage.tmpfs,node->first_cluster,&value);
        if(!rc)node->size=value.st_size;
    }
    storage_restore_volume(previous);kernel_execution_unlock_irqrestore(flags);return rc;
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
            .kind = STORAGE_VOLUME_ANONYMOUS, .filesystem = STORAGE_FILESYSTEM_TMPFS,.fs_ops=&storage_tmpfs_ops,
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
