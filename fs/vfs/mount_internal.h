/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_VFS_MOUNT_INTERNAL_H
#define RELIEFNT_VFS_MOUNT_INTERNAL_H
#include <reliefnt/mount.h>
#include <reliefnt/module.h>
#include <reliefnt/heap.h>
#include <linux/errno.h>

struct vfs_dentry {
    struct vfs_dentry *parent,*children,*next;
    struct storage_node node;
    void *backend_pin;
    uint64_t references;
    bool disconnected;
    char *name;
};
struct vfs_mount {
    struct vfs_namespace *ns;
    struct vfs_mount *next;
    struct vfs_path point;
    struct vfs_dentry *root;
    const struct vfs_backend_ops *ops;
    void *context;
    struct module *owner;
    uint64_t references,id,policy;
    char *source,*filesystem;
    uint64_t device;
};
/** @brief Normalize supported attachment input independently of its backend.
 * @param flags Native MS inputs; unsupported bits have no representation here.
 * @return Canonical mount bits. Strictatime clears both noatime and relatime,
 * matching Linux; nodiratime remains independent. Pure execution-owned helper.
 */
static inline uint64_t vfs_policy_bits(uint64_t flags)
{
    uint64_t policy=flags&VFS_MOUNT_POLICY_MASK;
    if(flags&MS_STRICTATIME)policy&=~(MS_NOATIME|MS_RELATIME);
    return policy;
}
/** @brief Query shared backend flags without importing attachment policy.
 * @param mount Borrowed valid mount retaining its provider code/context.
 * @return Supported shared flags, or zero without a callback. Pure callback
 * query under execution ownership; no allocation, I/O or ownership change.
 */
static inline uint64_t vfs_super_flags(const struct vfs_mount *mount)
{
    return mount->ops->mount_flags ? mount->ops->mount_flags(mount->context)&VFS_SUPER_POLICY_MASK : 0;
}
struct vfs_rename {
    struct vfs_path source,parent,target;
    char *name;
    bool same;
};
/* All helpers require the caller's execution transaction. No internal spinlock
 * or backend callback is hidden behind a mount graph lock. */
bool vfs_path_valid(const struct vfs_path *path);
bool vfs_path_covered(const struct vfs_namespace *ns,const struct vfs_path *path);
void vfs_follow_mounts(const struct vfs_namespace *ns,struct vfs_path *path);
/* Bind metadata returned by a completed native creation without re-looking up
 * the now-published name. Retain/publish one actual child identity. */
int vfs_bind_child(const struct vfs_path *parent,const char *name,
                       const struct storage_node *value,struct vfs_path *out);
void vfs_dentry_put(struct vfs_mount *mount,struct vfs_dentry *dentry);
void vfs_path_parent(const struct vfs_path *root,struct vfs_path *path);
/* Initialize common type/size/identity fields before provider-specific stat. */
int vfs_stat_basic(const struct storage_node *node,struct linux_stat_abi *out);
#endif
