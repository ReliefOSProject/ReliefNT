/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include "../../block/core/storage_internal.h"
#include <reliefnt/devfs.h>
#include <reliefnt/pseudo_fs.h>
#include <reliefnt/permissions.h>
#include <linux/errno.h>

static int devfs_node_lookup(const char *name,struct storage_node *out)
{
    if(!__builtin_strcmp(name,"/dev/shm")) {
        if(out)*out=(struct storage_node){.type=RELIEFOS_FS_TYPE_DIR,
            .flags=STORAGE_NODE_FLAG_DEV_DIR,.first_cluster=DEVFS_SHM_DIRECTORY};
        return 0;
    }
    return storage_devfs_lookup(name,out);
}
static int devfs_node_readdir(const char *name,uint64_t *cursor,struct reliefos_dir_entry *entry)
{
    struct storage_node node;int rc=devfs_node_lookup(name,&node);
    return rc ? rc : storage_devfs_readdir_node(&node,cursor,entry);
}
static int devfs_node_readlink(const char *name,char *buffer,uint32_t capacity)
{
    struct storage_node node;int rc=devfs_node_lookup(name,&node);
    if(rc)return rc;
    if(node.type!=RELIEFOS_FS_TYPE_SYMLINK)return -EINVAL;
    char target[48];rc=storage_devlink_target(name,target);if(rc)return rc;
    size_t length=__builtin_strlen(target);if(length>capacity)length=capacity;
    __builtin_memcpy(buffer,target,length);return (int)length;
}
int devfs_create_vfs_mount(struct vfs_mount **out)
{
    static const struct vfs_pseudo_provider provider={.root="/dev",.filesystem="devfs",.lookup=devfs_node_lookup,
        .readdir=devfs_node_readdir,.readlink=devfs_node_readlink,.permissions=fs_virtual_permissions};
    return vfs_pseudo_mount_new(&provider,out);
}
