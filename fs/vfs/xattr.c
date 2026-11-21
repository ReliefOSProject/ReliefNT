/* SPDX-License-Identifier: Apache-2.0 */
#include "mount_internal.h"

int vfs_path_getxattr(const struct vfs_path *path,const char *name,void *value,
                      uint32_t capacity,uint32_t *length)
{
    if (!vfs_path_valid(path) || !name || !length || (capacity && !value)) return -EINVAL;
    const struct vfs_mount *mount=path->mount;
    if (!mount->ops->getxattr) return -EOPNOTSUPP;
    return mount->ops->getxattr(mount->context,&path->dentry->node,name,value,capacity,length);
}

int vfs_path_listxattr(const struct vfs_path *path,void *names,uint32_t capacity,
                       uint32_t *length,bool include_trusted)
{
    if (!vfs_path_valid(path) || !length || (capacity && !names)) return -EINVAL;
    const struct vfs_mount *mount=path->mount;
    if (!mount->ops->listxattr) return -EOPNOTSUPP;
    return mount->ops->listxattr(mount->context,&path->dentry->node,names,capacity,length,include_trusted);
}
