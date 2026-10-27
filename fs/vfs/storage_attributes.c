/* SPDX-License-Identifier: Apache-2.0 */
#include "mount_internal.h"
#include "storage_backend.h"
#include "../../block/core/storage_internal.h"
#include <linux/errno.h>

/** @brief Report metadata of the actual retained filesystem object.
 * @param context Borrowed pinned native volume. @param path Required retained path.
 * @param out Required caller-owned stat buffer, unspecified on provider error.
 * @return Zero or negative validation/metadata errno. Cluster timestamps and
 * permissions belong to the generation-stamped held inode, including orphans;
 * no removed directory slot or displayed name is used as identity. Caller owns
 * execution; native I/O occurs without graph/module callback locks.
 */
int storage_backend_getattr(void *context,const struct vfs_path *path,struct linux_stat_abi *out)
{
    struct storage_volume *volume=context;const struct storage_node *node=vfs_path_node(path);
    if(!volume || !node || node->volume_id!=volume->volume_id)return -EINVAL;
    if(node->flags&(STORAGE_NODE_FLAG_TMPFS|STORAGE_NODE_FLAG_EXT2))return storage_inode_stat(node,out);
    struct storage_node current=*node;int rc=storage_inode_refresh(&current);
    if(!rc)rc=vfs_stat_basic(&current,out);
    struct reliefos_permissions mode;
    if(!rc)rc=storage_backend_permissions(context,path,&mode,false);
    if(!rc) {
        out->st_dev=storage_volume_device(volume->volume_id);
        out->st_mode|=mode.mode&07777;out->st_uid=mode.uid;out->st_gid=mode.gid;
        if(storage_inode_is_unlinked(node))out->st_nlink=0;
        if(node->flags&(STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT)) {
            struct storage_inode_times times;rc=storage_cluster_node_times(node,&times);
            if(!rc) {
                out->atime_sec=times.atime;out->atime_nsec=times.atime_nsec;
                out->mtime_sec=times.mtime;out->mtime_nsec=times.mtime_nsec;
                out->ctime_sec=times.ctime;out->ctime_nsec=times.ctime_nsec;
            }
        }
    }
    return rc;
}

int storage_backend_statfs(void *context,const struct storage_node *node,struct linux_statfs_abi *out)
{
    struct storage_volume *volume=context;
    return !volume || !node || node->volume_id!=volume->volume_id ? -EINVAL : storage_statfs(node,out);
}

int storage_backend_write(void *context,struct storage_node *node,uint64_t offset,
                             const void *bytes,uint32_t length,uint32_t *written)
{
    struct storage_volume *volume=context;
    return !volume || !node || node->volume_id!=volume->volume_id ? -EINVAL :
        storage_write_held_node(node,offset,bytes,length,written);
}

int storage_backend_truncate(void *context,struct storage_node *node,uint64_t length)
{
    struct storage_volume *volume=context;
    return !volume || !node || node->volume_id!=volume->volume_id ? -EINVAL : storage_truncate_held_node(node,length);
}

int storage_backend_fallocate(void *context,struct storage_node *node,uint32_t mode,uint64_t offset,uint64_t length)
{
    struct storage_volume *volume=context;
    if(!volume || !node || node->volume_id!=volume->volume_id)return -EINVAL;
    if(node->flags&STORAGE_NODE_FLAG_TMPFS)return storage_tmpfs_fallocate(node,mode,offset,length);
    if(node->flags&STORAGE_NODE_FLAG_EXT_FAMILY)return storage_fallocate_node(node,mode,offset,length);
    /* Cluster filesystems allocate and zero every byte up to EOF already.
     * A mode-zero extension uses their existing held-inode zero-fill path. */
    if(mode)return -EOPNOTSUPP;
    int rc=storage_inode_refresh(node);
    if(!rc && offset+length>node->size)rc=storage_truncate_held_node(node,offset+length);
    return rc;
}

int storage_backend_sync(void *context,const struct storage_node *node,bool filesystem,bool data_only)
{
    struct storage_volume *volume=context;(void)filesystem;(void)data_only;
    return !volume || !node || node->volume_id!=volume->volume_id ? -EINVAL :
        storage_sync_volume(volume->volume_id);
}

int storage_backend_utimens(void *context,const struct storage_node *node,int64_t atime,int64_t mtime,
                               bool set_atime,bool set_mtime)
{
    struct storage_volume *volume=context;
    return !volume || !node || node->volume_id!=volume->volume_id ? -EINVAL :
        storage_inode_utimensat(node,atime,mtime,set_atime,set_mtime);
}

/** @brief Dispatch the existing ext allocation API through the held instance.
 * @param node Borrowed mutable retained ext inode. @param mode Linux fallocate mode.
 * @param offset Range start. @param len Range length.
 * @return Zero or unsupported/provider errno. Caller authorizes the request;
 * recursively enters execution, restores native selection and never holds a
 * graph/module lock across native allocation/cache operations.
 */
int storage_fallocate_node(struct storage_node *node,uint32_t mode,uint64_t offset,uint64_t len)
{
    if(!node || !(node->flags&STORAGE_NODE_FLAG_EXT_FAMILY))return -EOPNOTSUPP;
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    struct storage_volume *previous=NULL;int rc=storage_select_node_volume(node,&previous);
    if(!rc) {
        const struct storage_filesystem_ops *ops=g_storage.fs_ops;
        rc=ops && ops->fallocate ? ops->fallocate(&g_storage,node,mode,offset,len) : -EOPNOTSUPP;
    }
    storage_restore_volume(previous);kernel_execution_unlock_irqrestore(flags);return rc;
}
