/* SPDX-License-Identifier: Apache-2.0 */
#include "mount_internal.h"
#include <linux/mount.h>

int vfs_stat_basic(const struct storage_node *node,struct linux_stat_abi *out)
{
    if(node->size>INT64_MAX)return -EOVERFLOW;
    uint32_t type=node->type==RELIEFOS_FS_TYPE_DIR ? LINUX_S_IFDIR :
        node->type==RELIEFOS_FS_TYPE_SYMLINK ? LINUX_S_IFLNK :
        node->type==RELIEFOS_FS_TYPE_SOCKET ? LINUX_S_IFSOCK :
        node->type==RELIEFOS_FS_TYPE_FIFO ? LINUX_S_IFIFO :
        node->type==RELIEFOS_FS_TYPE_DEVICE ? node->flags&STORAGE_NODE_FLAG_DEV_BLOCK ? LINUX_S_IFBLK : LINUX_S_IFCHR : LINUX_S_IFREG;
    *out=(struct linux_stat_abi){.st_mode=type,.st_nlink=1,.st_size=(int64_t)node->size,
        .st_blksize=4096,.st_blocks=(int64_t)((node->size+511)/512),
        .st_ino=node->inode_generation ? node->inode_generation : node->entry_id ? node->entry_id : node->first_cluster ? node->first_cluster : 1};
    return 0;
}

int vfs_path_stat(const struct vfs_path *path,struct linux_stat_abi *out)
{
    if(!out)return -EINVAL;*out=(struct linux_stat_abi){0};
    if(!vfs_path_valid(path))return -EINVAL;
    struct linux_stat_abi value;int rc;
    if(path->mount->ops->getattr)rc=path->mount->ops->getattr(path->mount->context,path,&value);
    else {
        struct reliefos_permissions mode;
        rc=vfs_stat_basic(&path->dentry->node,&value);
        if(!rc)rc=vfs_path_permissions(path,&mode,false);
        if(!rc){value.st_mode|=mode.mode&07777;value.st_uid=mode.uid;value.st_gid=mode.gid;}
    }
    if(!rc)*out=value;return rc;
}

/** @brief Combine filesystem capacity with flags of this exact attachment.
 * @param path Borrowed retained path, required; provider remains pinned.
 * @param out Required native statfs destination, cleared on failure.
 * @return Zero or negative validation/provider errno. f_flags is calculated
 * from actual mount/SB policy, never inherited from another attachment through
 * a backend statfs result. Execution ownership required; no graph/module lock
 * spans the capacity callback. No allocation or ownership transfer here.
 */
int vfs_path_statfs(const struct vfs_path *path,struct linux_statfs_abi *out)
{
    if(!out)return -EINVAL;*out=(struct linux_statfs_abi){0};
    if(!vfs_path_valid(path))return -EINVAL;
    if(!path->mount->ops->statfs)return -EOPNOTSUPP;
    struct linux_statfs_abi value;int rc=path->mount->ops->statfs(path->mount->context,&path->dentry->node,&value);
    if(!rc) {
        uint64_t flags=vfs_path_mount_flags(path);
        value.f_flags=LINUX_ST_VALID|(flags&(MS_RDONLY|MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_SYNCHRONOUS|MS_MANDLOCK|MS_NOATIME|MS_NODIRATIME));
        if(flags&MS_RELATIME)value.f_flags|=LINUX_ST_RELATIME;
        *out=value;
    }
    return rc;
}

int vfs_path_write(const struct vfs_path *path,uint64_t offset,const void *bytes,
                      uint32_t length,uint32_t *written)
{
    if(!written)return -EINVAL;*written=0;
    if(!vfs_path_valid(path) || (length && !bytes))return -EINVAL;
    if(path->dentry->node.type==RELIEFOS_FS_TYPE_DIR)return -EISDIR;
    if(vfs_path_mount_flags(path)&MS_RDONLY)return -EROFS;
    if(!path->mount->ops->write)return -EOPNOTSUPP;
    char empty;int rc=path->mount->ops->write(path->mount->context,&path->dentry->node,offset,
                                               bytes ? bytes : &empty,length,written);
    if(*written>length){*written=0;return -EIO;}
    return rc;
}

int vfs_path_truncate(const struct vfs_path *path,uint64_t length)
{
    if(!vfs_path_valid(path))return -EINVAL;
    if(path->dentry->node.type==RELIEFOS_FS_TYPE_DIR)return -EISDIR;
    if(path->dentry->node.type!=RELIEFOS_FS_TYPE_FILE)return -EINVAL;
    if(vfs_path_mount_flags(path)&MS_RDONLY)return -EROFS;
    return path->mount->ops->truncate ?
        path->mount->ops->truncate(path->mount->context,&path->dentry->node,length) : -EOPNOTSUPP;
}

int vfs_path_fallocate(const struct vfs_path *path,uint32_t mode,uint64_t offset,uint64_t length)
{
    if(!vfs_path_valid(path) || !length)return -EINVAL;
    if(offset>INT64_MAX || length>(uint64_t)INT64_MAX-offset)return -EFBIG;
    if(path->dentry->node.type==RELIEFOS_FS_TYPE_DIR)return -EISDIR;
    if(path->dentry->node.type==RELIEFOS_FS_TYPE_FIFO)return -ESPIPE;
    if(path->dentry->node.type!=RELIEFOS_FS_TYPE_FILE)return -ENODEV;
    if(vfs_path_mount_flags(path)&MS_RDONLY)return -EROFS;
    return path->mount->ops->fallocate ? path->mount->ops->fallocate(path->mount->context,
        &path->dentry->node,mode,offset,length) : -EOPNOTSUPP;
}

int vfs_path_sync(const struct vfs_path *path,bool filesystem,bool data_only)
{
    if(!vfs_path_valid(path))return -EINVAL;
    return path->mount->ops->sync ? path->mount->ops->sync(path->mount->context,
        &path->dentry->node,filesystem,data_only) : filesystem ? 0 : -EINVAL;
}

int vfs_namespace_sync(struct vfs_namespace *ns)
{
    if(!ns || !ns->root)return -EINVAL;
    int error=0;
    for(struct vfs_mount *mount=ns->mounts;mount;mount=mount->next) {
        struct vfs_path root={mount,mount->root},held={0};vfs_path_get(&root,&held);
        int rc=vfs_path_sync(&held,true,false);
        if(rc && !error)error=rc;
        vfs_path_put(&held);
    }
    return error;
}

int vfs_path_utimens(const struct vfs_path *path,int64_t atime,int64_t mtime,bool set_atime,bool set_mtime)
{
    if(!vfs_path_valid(path))return -EINVAL;
    if(vfs_path_mount_flags(path)&MS_RDONLY)return -EROFS;
    return path->mount->ops->utimens ?
        path->mount->ops->utimens(path->mount->context,&path->dentry->node,atime,mtime,set_atime,set_mtime) : -EOPNOTSUPP;
}

int vfs_path_readlink(const struct vfs_path *path,char *buffer,size_t capacity,size_t *length)
{
    if(!length)return -EINVAL;*length=0;
    if(!vfs_path_valid(path) || !buffer || !capacity)return -EINVAL;
    if(path->dentry->node.type!=RELIEFOS_FS_TYPE_SYMLINK || !path->mount->ops->readlink)return -EINVAL;
    int rc=path->mount->ops->readlink(path->mount->context,&path->dentry->node,buffer,capacity,length);
    if(*length>capacity){*length=0;return -EIO;}
    return rc;
}
