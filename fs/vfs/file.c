/* SPDX-License-Identifier: Apache-2.0 */
#include "mount_internal.h"

struct vfs_namespace *vfs_path_namespace(const struct vfs_path *path)
{return vfs_path_valid(path) ? path->mount->ns : NULL;}

const char *vfs_path_name(const struct vfs_path *path)
{return vfs_path_valid(path) ? path->dentry->name : NULL;}

bool vfs_path_same_inode(const struct vfs_path *left,const struct vfs_path *right)
{
    return vfs_path_valid(left) && vfs_path_valid(right) && left->mount->ops==right->mount->ops &&
        left->mount->context==right->mount->context &&
        left->mount->ops->same_node(left->mount->context,&left->dentry->node,&right->dentry->node);
}

bool vfs_path_same_filesystem(const struct vfs_path *left,const struct vfs_path *right)
{
    return vfs_path_valid(left) && vfs_path_valid(right) &&
        left->mount->ops==right->mount->ops && left->mount->context==right->mount->context;
}

/** @brief Read an actual inode with its current attachment access policy.
 * @param path Borrowed retained regular path, required.
 * @param offset Byte position. @param buffer Destination, NULL only at capacity0.
 * @param capacity Available bytes. @param read Required actual count output.
 * @return Provider status or negative input/type/unsupported/count errno.
 * Execution ownership required; provider remains pinned, no graph/module lock
 * spans I/O. No allocation or ownership change; policy is passed per access.
 */
int vfs_path_read(const struct vfs_path *path,uint64_t offset,void *buffer,
                    uint32_t capacity,uint32_t *read)
{
    if(!read)return -EINVAL;
    *read=0;
    if(!vfs_path_valid(path) || (capacity && !buffer))return -EINVAL;
    if(path->dentry->node.type==RELIEFOS_FS_TYPE_DIR)return -EISDIR;
    if(!path->mount->ops->read)return -EOPNOTSUPP;
    char empty;
    if(!buffer)buffer=&empty;
    int rc=path->mount->ops->read(path->mount->context,&path->dentry->node,offset,buffer,capacity,read,vfs_path_mount_flags(path));
    if(*read>capacity){*read=0;return -EIO;}
    return rc;
}

/** @brief Read through an optional OFD cursor with current attachment policy.
 * @param path Borrowed retained regular path, required.
 * @param offset Byte position. @param buffer Destination, NULL at capacity0.
 * @param capacity Available bytes. @param read Required actual count output.
 * @param cursor Borrowed optional cursor; invalidated on fallback/count error.
 * @return Provider status or negative validation/type/unsupported/count errno.
 * Execution ownership required; no graph/module lock spans I/O, no allocation
 * or ownership change. A cursor never substitutes for the actual inode/policy.
 */
int vfs_path_read_cursor(const struct vfs_path *path,uint64_t offset,void *buffer,
                            uint32_t capacity,uint32_t *read,struct storage_read_cursor *cursor)
{
    if(!read)return -EINVAL;*read=0;
    if(!vfs_path_valid(path) || (capacity && !buffer))return -EINVAL;
    if(path->dentry->node.type==RELIEFOS_FS_TYPE_DIR)return -EISDIR;
    if(!cursor || !path->mount->ops->read_cursor) {
        if(cursor)cursor->valid=0;
        return vfs_path_read(path,offset,buffer,capacity,read);
    }
    char empty;
    int rc=path->mount->ops->read_cursor(path->mount->context,&path->dentry->node,offset,
        buffer ? buffer : &empty,capacity,read,cursor,vfs_path_mount_flags(path));
    if(*read>capacity){cursor->valid=0;*read=0;return -EIO;}
    return rc;
}

/** @brief Enumerate an actual directory with its selected attachment policy.
 * @param path Borrowed retained directory path, required.
 * @param cursor Required provider-owned opaque cursor value.
 * @param entry Required entry output.
 * @return One entry, zero EOF or negative validation/type/unsupported/provider
 * errno. Execution ownership required; provider stays pinned, no graph/module
 * lock spans I/O. No allocation or ownership transfer by this adapter.
 */
int vfs_path_readdir(const struct vfs_path *path,uint64_t *cursor,struct reliefos_dir_entry *entry)
{
    if(!vfs_path_valid(path) || !cursor || !entry)return -EINVAL;
    if(path->dentry->node.type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    if(!path->mount->ops->readdir)return -EOPNOTSUPP;
    return path->mount->ops->readdir(path->mount->context,&path->dentry->node,cursor,entry,vfs_path_mount_flags(path));
}

int vfs_path_permissions(const struct vfs_path *path,struct reliefos_permissions *value,bool write)
{
    if(!vfs_path_valid(path) || !value)return -EINVAL;
    if(!path->mount->ops->permissions)return -EOPNOTSUPP;
    return path->mount->ops->permissions(path->mount->context,path,value,write);
}

/** @brief Read attachment policy combined with current shared backing flags.
 * @param path Borrowed retained path, nullable; execution ownership required.
 * @return Supported MS bits or zero for invalid input. Readonly from either
 * owner remains effective. Pure query; no allocation/I/O/ownership transfer.
 */
uint64_t vfs_path_mount_flags(const struct vfs_path *path)
{
    return vfs_path_valid(path) ? path->mount->policy|vfs_super_flags(path->mount) : 0;
}

/** @brief Configure a detached attachment without mutating its shared backend.
 * @param mount Owned unpublished mount, required.
 * @param flags Only per-mount bits and optional strictatime selector.
 * @return Zero or negative state/unsupported errno, preserving state on error.
 * Pure execution-owned publication, no callbacks/allocation/ownership change.
 */
int vfs_mount_set_flags(struct vfs_mount *mount,uint64_t flags)
{
    if(!mount || !mount->references)return -EINVAL;
    if(mount->ns || mount->point.mount)return -EBUSY;
    if(flags&~(VFS_MOUNT_POLICY_MASK|MS_STRICTATIME))return -EOPNOTSUPP;
    mount->policy=vfs_policy_bits(flags);return 0;
}

/** @brief Reconfigure the selected attachment after shared backend preparation.
 * @param path Borrowed actual attached filesystem root, required.
 * @param flags Requested supported MS flags; omitted atime preserves old bits.
 * @return Zero or input/unsupported/readonly/backend errno. Failed preparation
 * leaves mount policy unchanged. Execution ownership required; no graph/module
 * lock spans the optional shared-policy callback. Strictatime clears noatime
 * and relatime. All other attachment policy changes affect this mount alone.
 */
int vfs_path_remount(const struct vfs_path *path,uint64_t flags)
{
    if(!vfs_path_valid(path) || !path->mount->ns || path->dentry!=path->mount->root)return -EINVAL;
    if(flags&~(VFS_MOUNT_POLICY_MASK|VFS_SUPER_POLICY_MASK|MS_STRICTATIME))return -EOPNOTSUPP;
    struct vfs_mount *mount=path->mount;
    const uint64_t atime=MS_NOATIME|MS_NODIRATIME|MS_RELATIME;
    uint64_t policy=vfs_policy_bits(flags),shared=flags&VFS_SUPER_POLICY_MASK;
    if(!(flags&(atime|MS_STRICTATIME)))policy|=mount->policy&atime;
    int rc=0;
    if(mount->ops->remount)rc=mount->ops->remount(mount->context,shared);
    else if(shared!=vfs_super_flags(mount))rc=(vfs_super_flags(mount)&MS_RDONLY) && !(shared&MS_RDONLY) ? -EROFS : -EOPNOTSUPP;
    if(!rc)mount->policy=policy;return rc;
}

int vfs_path_dirent_stat(const struct vfs_path *directory,const char *name,struct linux_stat_abi *out)
{
    if(!out)return -EINVAL;*out=(struct linux_stat_abi){0};
    if(!vfs_path_valid(directory) || !name || !name[0])return -EINVAL;
    if(directory->dentry->node.type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    struct vfs_path child={0};int rc=0;
    if(!__builtin_strcmp(name,"."))vfs_path_get(directory,&child);
    else if(!__builtin_strcmp(name,"..")) {
        struct vfs_path parent={directory->mount,directory->dentry->parent ? directory->dentry->parent : directory->dentry};
        vfs_path_get(&parent,&child);
    } else rc=vfs_path_lookup_child(directory,name,&child);
    if(!rc)rc=vfs_path_stat(&child,out);
    vfs_path_put(&child);return rc;
}

int vfs_path_new_file(const struct vfs_path *parent,const char *name,
                         const struct reliefos_permissions *value,struct vfs_path *out)
{return !out ? -EINVAL : vfs_path_create_node(parent,name,LINUX_S_IFREG,value,NULL,out);}

int vfs_path_create_node(const struct vfs_path *parent,const char *name,uint32_t type,
                            const struct reliefos_permissions *value,const char *target,struct vfs_path *out)
{
    if(out)*out=(struct vfs_path){0};
    if(!vfs_path_valid(parent) || !name || !name[0] || !value ||
       !__builtin_strcmp(name,".") || !__builtin_strcmp(name,".."))return -EINVAL;
    size_t length=0;
    for(;name[length];++length) {
        if(name[length]=='/')return -EINVAL;
        if(length>=VFS_NAME_MAX)return -ENAMETOOLONG;
    }
    if(parent->dentry->node.type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    if(parent->dentry->disconnected)return -ENOENT;
    if(type!=LINUX_S_IFREG && type!=LINUX_S_IFDIR && type!=LINUX_S_IFLNK &&
       type!=LINUX_S_IFIFO && type!=LINUX_S_IFSOCK)return -EOPNOTSUPP;
    if(type==LINUX_S_IFLNK && (!target || !target[0]))return -ENOENT;
    if(type!=LINUX_S_IFLNK && target)return -EINVAL;
    if(!parent->mount->ops->create_node)return -EROFS;
    struct vfs_dentry *reserved=NULL;
    if(out) {
        reserved=kernel_malloc(sizeof(*reserved));if(!reserved)return -ENOMEM;
        *reserved=(struct vfs_dentry){.parent=parent->dentry};
        reserved->name=kernel_malloc(length+1);
        if(!reserved->name){kernel_free(reserved);return -ENOMEM;}
        __builtin_memcpy(reserved->name,name,length+1);
    }
    struct storage_node node;void *pin=NULL;
    int rc=parent->mount->ops->create_node(parent->mount->context,&parent->dentry->node,name,type,value,target,&node,
        reserved ? &pin : NULL);
    if(rc) {
        if(reserved){kernel_free(reserved->name);kernel_free(reserved);}
        return rc;
    }
    if(!out)return 0;
    reserved->node=node;reserved->backend_pin=pin;
    ++parent->dentry->references;
    reserved->next=parent->dentry->children;parent->dentry->children=reserved;
    struct vfs_path created={parent->mount,reserved};vfs_path_get(&created,out);return 0;
}
