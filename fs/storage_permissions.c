static int storage_ext4_permission_write(struct storage_volume *v,uint64_t ino,const struct ext4_inode_view *in)
{
    struct storage_ext4_handle h; int ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h);
    if (ret<0) return ret;
    ret=storage_ext4_write_inode(v,ino,in);
    if (ret<0) storage_ext4_journal_abort(&h,ret);
    int stopped=storage_ext4_journal_stop(&h); return ret<0?ret:stopped;
}
/* ext2 ownership and mode live in the inode, including Linux's high uid/gid
 * words. Other backends use the versioned metadata sidecar. */
int storage_inode_permissions(const struct storage_node *node,
                              struct reliefos_permissions *value, bool write)
{
    struct storage_volume *previous = NULL;
    struct ext4_inode_view inode;
    uint64_t flags;
    int ret;
    if (!node || !value) return -22;
    if (!(node->flags & (STORAGE_NODE_FLAG_EXT2 | STORAGE_NODE_FLAG_TMPFS))) return -95;
    kernel_execution_lock_irqsave(&flags);
    ret = storage_select_node_volume(node, &previous);
    if (ret < 0) goto out;
    if (node->flags & STORAGE_NODE_FLAG_TMPFS) {
        ret = tmpfs_permissions(g_storage.tmpfs, node->first_cluster, value, write);
        goto restore;
    }
    ret = storage_ext4_check_node(&g_storage, node, &inode);
    if (ret < 0) goto restore;
    if (write) {
        struct reliefos_time_info now;
        if (time_wall_clock(&now) == 0) inode.ctime = (uint32_t)now.unix_seconds;
        inode.mode = (inode.mode & 0170000u) | (value->mode & 07777u);
        inode.uid = value->uid;
        inode.gid = value->gid;
        ret = storage_ext4_permission_write(&g_storage, node->first_cluster, &inode);
    } else {
        value->mode = inode.mode & 07777u;
        value->uid = inode.uid;
        value->gid = inode.gid;
    }
restore:
    storage_restore_volume(previous);
out:
    kernel_execution_unlock_irqrestore(flags);
    return ret;
}

int storage_inode_stat(const struct storage_node *node, struct linux_stat_abi *value)
{
    struct storage_volume *previous = NULL;
    struct ext4_inode_view inode;
    uint64_t flags;
    if (!node || !value) return -22;
    if (!(node->flags & (STORAGE_NODE_FLAG_EXT2 | STORAGE_NODE_FLAG_TMPFS))) return -95;
    kernel_execution_lock_irqsave(&flags);
    int ret = storage_select_node_volume(node, &previous);
    if (!ret && (node->flags & STORAGE_NODE_FLAG_TMPFS)) {
        ret = tmpfs_stat(g_storage.tmpfs, node->first_cluster, value);
        storage_restore_volume(previous);
        kernel_execution_unlock_irqrestore(flags);
        return ret;
    }
    if (!ret) {
        ret = storage_ext4_check_node(&g_storage, node, &inode);
        if (!ret) {
            *value = (struct linux_stat_abi){0};
            value->st_dev = storage_volume_device_number(&g_storage);
            value->st_ino = node->first_cluster;
            value->st_nlink = inode.links_count;
            value->st_mode = inode.mode;
            value->st_uid = inode.uid;
            value->st_gid = inode.gid;
            value->st_size = inode.size;
            value->st_blksize = g_storage.ext4.block_size;
            value->st_blocks = inode.blocks;
            value->atime_sec = (int32_t)inode.atime;
            value->mtime_sec = (int32_t)inode.mtime;
            value->ctime_sec = (int32_t)inode.ctime;
            if ((inode.mode & LINUX_S_IFMT) == LINUX_S_IFCHR || (inode.mode & LINUX_S_IFMT) == LINUX_S_IFBLK) {
                uint32_t dev = ext4_get_le32(inode.i_block_raw);
                if (dev) value->st_rdev = dev;
                else value->st_rdev = ext4_get_le32(inode.i_block_raw+4);
            }
        }
        storage_restore_volume(previous);
    }
    kernel_execution_unlock_irqrestore(flags);
    return ret;
}

int storage_inode_utimensat(const struct storage_node *node, int64_t atime, int64_t mtime,
                            bool set_atime, bool set_mtime)
{
    struct storage_volume *previous = NULL;
    struct ext4_inode_view inode;
    uint64_t flags;
    int ret;
    if (!node || !(node->flags & (STORAGE_NODE_FLAG_EXT2 | STORAGE_NODE_FLAG_TMPFS))) return -95;
    kernel_execution_lock_irqsave(&flags);
    ret = storage_select_node_volume(node, &previous);
    if (!ret && (node->flags & STORAGE_NODE_FLAG_TMPFS)) {
        ret = tmpfs_utimens(g_storage.tmpfs, node->first_cluster, atime, mtime, set_atime, set_mtime);
        storage_restore_volume(previous);
        kernel_execution_unlock_irqrestore(flags);
        return ret;
    }
    if (!ret) ret = storage_ext4_check_node(&g_storage, node, &inode);
    if (!ret) {
        if (set_atime) inode.atime = (uint32_t)atime;
        if (set_mtime) inode.mtime = (uint32_t)mtime;
        ret = storage_ext4_permission_write(&g_storage, node->first_cluster, &inode);
    }
    storage_restore_volume(previous);
    kernel_execution_unlock_irqrestore(flags);
    return ret;
}
