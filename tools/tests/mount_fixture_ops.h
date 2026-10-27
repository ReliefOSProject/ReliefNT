/* SPDX-License-Identifier: Apache-2.0 */
/* Trusted test conveniences over the production resolver/retained objects.
 * No prefix routing, backend selection, lexical normalization or copied
 * filesystem implementation lives in this fixture. The caller owns ns. */
#ifndef RELIEFNT_TEST_MOUNT_FIXTURE_OPS_H
#define RELIEFNT_TEST_MOUNT_FIXTURE_OPS_H
#include <assert.h>
#include <reliefnt/kernel_file.h>
#include <linux/stat.h>
#include "../../fs/vfs/storage_backend.h"

static inline int mount_fixture_begin(struct vfs_namespace *ns,struct storage_volume *volume,struct vfs_path *root)
{
    struct vfs_mount *mount=NULL;int rc=storage_create_vfs_mount(volume,NULL,&mount);
    if(!rc)rc=vfs_namespace_init(ns,mount);
    vfs_mount_put(mount);
    if(!rc)rc=vfs_namespace_root(ns,root);
    if(rc && ns->root)assert(!vfs_namespace_destroy(ns));
    return rc;
}
static inline int mount_fixture_lookup(struct vfs_namespace *ns,const char *name,struct storage_node *out)
{
    struct vfs_lookup_result found={0};int rc=vfs_kernel_lookup(ns,name,0,&found);
    if(!rc)*out=*vfs_path_node(&found.path);
    vfs_path_put(&found.path);return rc;
}
static inline int mount_fixture_write(struct vfs_namespace *ns,const char *name,uint64_t offset,
                                      const void *bytes,uint32_t length,uint32_t *written)
{
    struct vfs_lookup_result found={0};int rc=vfs_kernel_lookup(ns,name,VFS_LOOKUP_FOLLOW,&found);
    if(!rc)rc=vfs_path_write(&found.path,offset,bytes,length,written);
    vfs_path_put(&found.path);return rc;
}
static inline int mount_fixture_truncate(struct vfs_namespace *ns,const char *name,uint64_t length)
{
    struct vfs_lookup_result found={0};int rc=vfs_kernel_lookup(ns,name,VFS_LOOKUP_FOLLOW,&found);
    if(!rc)rc=vfs_path_truncate(&found.path,length);
    vfs_path_put(&found.path);return rc;
}
static inline int mount_fixture_remove(struct vfs_namespace *ns,const char *name,bool directory)
{
    struct vfs_lookup_result found={0};int rc=vfs_kernel_lookup(ns,name,0,&found);
    if(!rc)rc=vfs_path_remove(ns,&found.path,directory);
    vfs_path_put(&found.path);return rc;
}
static inline int mount_fixture_rename(struct vfs_namespace *ns,const char *source,const char *destination)
{
    struct vfs_lookup_result old={0},parent={0};int rc=vfs_kernel_lookup(ns,source,0,&old);
    if(!rc)rc=vfs_kernel_lookup(ns,destination,VFS_LOOKUP_PARENT,&parent);
    if(!rc)rc=vfs_path_rename(ns,&old.path,&parent.path,parent.last);
    vfs_path_put(&parent.path);vfs_path_put(&old.path);return rc;
}
static inline int mount_fixture_link(struct vfs_namespace *ns,const char *source,const char *destination)
{
    struct vfs_lookup_result old={0},parent={0};int rc=vfs_kernel_lookup(ns,source,VFS_LOOKUP_FOLLOW,&old);
    if(!rc)rc=vfs_kernel_lookup(ns,destination,VFS_LOOKUP_PARENT,&parent);
    if(!rc)rc=vfs_path_link(ns,&old.path,&parent.path,parent.last);
    vfs_path_put(&parent.path);vfs_path_put(&old.path);return rc;
}
static inline int mount_fixture_symlink(struct vfs_namespace *ns,const char *target,const char *destination)
{
    struct vfs_lookup_result parent={0};int rc=vfs_kernel_lookup(ns,destination,VFS_LOOKUP_PARENT,&parent);
    const struct reliefos_permissions value={0777,0,0};
    if(!rc)rc=vfs_path_create_node(&parent.path,parent.last,LINUX_S_IFLNK,&value,target,NULL);
    vfs_path_put(&parent.path);return rc;
}
static inline int mount_fixture_readlink(struct vfs_namespace *ns,const char *name,char *buffer,uint32_t capacity,uint32_t *length)
{
    struct vfs_lookup_result found={0};int rc=vfs_kernel_lookup(ns,name,0,&found);size_t bytes=0;
    if(!rc)rc=vfs_path_readlink(&found.path,buffer,capacity,&bytes);
    *length=(uint32_t)bytes;vfs_path_put(&found.path);return rc;
}
/* A mounted root necessarily pins its inode. Exclude only that known native
 * root pin when checking the independent inode users under test. The get/put
 * obtains its address; the retained root path keeps that borrowed address live. */
static inline bool mount_fixture_other_inode_users(const struct vfs_path *root)
{
    struct storage_inode_ref *pin=NULL;assert(!storage_inode_get(vfs_path_node(root),&pin));
    assert(!storage_inode_put(pin));
    return storage_inode_volume_other_users(vfs_path_node(root)->volume_id,pin);
}

#endif
