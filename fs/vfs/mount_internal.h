/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_VFS_MOUNT_INTERNAL_H
#define RELIEFNT_VFS_MOUNT_INTERNAL_H
#include <reliefnt/mount.h>
#include <reliefnt/module.h>
#include <reliefnt/heap.h>
#include <linux/errno.h>
#include <reliefnt/fsnotify_vfs.h>
#include <linux/inotify.h>

struct vfs_lookup_cache_entry;

struct vfs_dentry {
    struct vfs_dentry *parent,*children,*next,*notify_next;
    struct vfs_dentry **notify_link;
    struct vfs_mount *notify_mount;
    struct storage_node node;
    void *backend_pin;
    uint64_t references;
    bool disconnected,notify_deleted,notify_recheck;
    char *name;
};
struct vfs_mount {
    struct vfs_lookup_cache_entry *lookup_cache;
    struct vfs_namespace *ns;
    struct vfs_mount *next,*notify_next;
    struct vfs_mount **notify_link;
    struct vfs_path point,bind_source;
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
#ifdef RELIEFNT_FSNOTIFY_VFS_IMPLEMENTATION
#define VFS_NOTIFY_INTERNAL
#else
#define VFS_NOTIFY_INTERNAL __attribute__((weak))
#endif
/** @brief Track a constructed mount without holding a reference.
 * @param mount Borrowed constructed mount with a live backend.
 * Execution writer is required; remove the weak entry before provider teardown.
 */
void vfs_notify_mount_created(struct vfs_mount *mount) VFS_NOTIFY_INTERNAL;
/** @brief Remove a final mount from the registry and invalidate its domain.
 * @param mount Borrowed retiring mount after successful provider finish.
 * Execution writer is required; invalidation occurs only for the last live
 * mount in its provider domain, before backend code or context is destroyed.
 */
void vfs_notify_mount_retired(struct vfs_mount *mount) VFS_NOTIFY_INTERNAL;
/** @brief Track a retained dentry without holding a reference.
 * @param mount Borrowed live mount owning the dentry.
 * @param dentry Borrowed retained dentry, removed before its final release.
 * Execution writer is required; the weak registry acquires no references.
 */
void vfs_notify_dentry_created(struct vfs_mount *mount,struct vfs_dentry *dentry) VFS_NOTIFY_INTERNAL;
/** @brief Invalidate a terminal inode when its final cached alias retires.
 * @param mount Borrowed live mount with provider code and context still present.
 * @param dentry Borrowed final alias immediately before backend release.
 * Execution writer is required; registry removal transfers no object ownership.
 */
void vfs_notify_dentry_retired(struct vfs_mount *mount,struct vfs_dentry *dentry) VFS_NOTIFY_INTERNAL;
/** @brief Mark all cached aliases for a last-link inode as terminal.
 * @param path Borrowed retained inode path, or NULL.
 * @param recheck True when uncertain link count needs a final nlink query.
 * Execution writer is required; marked weak aliases acquire no references.
 */
void vfs_notify_terminal(const struct vfs_path *path,bool recheck) VFS_NOTIFY_INTERNAL;
/** @brief Check whether any notification watches currently exist.
 * @return True if at least one watch is live. Execution writer is required;
 * the scalar query acquires no provider, path, mount, or inode reference.
 */
bool vfs_notify_interested(void) VFS_NOTIFY_INTERNAL;
/** @brief Emit a nameless event only to the given inode's watches.
 * @param path Borrowed retained inode path, or NULL.
 * @param mask Linux event bits to deliver.
 * Execution writer keeps the provider and path live; no reference is retained.
 */
void vfs_notify_inode(const struct vfs_path *path,uint32_t mask) VFS_NOTIFY_INTERNAL;
/** @brief Emit a named event to a directory's watches.
 * @param parent Borrowed retained parent path, or NULL.
 * @param mask Linux child event bits.
 * @param cookie Rename cookie or zero.
 * @param name Borrowed optional basename, copied during delivery.
 * @param directory True if the child is a directory.
 * Execution writer is required; input paths and names remain caller-owned.
 */
void vfs_notify_child(const struct vfs_path *parent,uint32_t mask,uint32_t cookie,
                      const char *name,bool directory) VFS_NOTIFY_INTERNAL;
/** @brief Emit committed rename events while old and new names remain available.
 * @param source Borrowed retained source with its old name.
 * @param parent Borrowed retained destination parent.
 * @param name Borrowed destination basename, copied during delivery.
 * @param target Borrowed replaced target, possibly empty.
 * Execution writer keeps paths and provider live; no reference is retained.
 */
void vfs_notify_rename(const struct vfs_path *source,const struct vfs_path *parent,
                       const char *name,const struct vfs_path *target) VFS_NOTIFY_INTERNAL;
#undef VFS_NOTIFY_INTERNAL

struct vfs_rename {
    struct vfs_path source,parent,target;
    char *name;
    bool same;
};
/* All helpers require the caller's execution transaction. No internal spinlock
 * or backend callback is hidden behind a mount graph lock. */
bool vfs_path_valid(const struct vfs_path *path);
/** @brief Resolve a child for a retained walker, preserving its private
 * pre-I/O native-workspace marker. The walker owns final normalization.
 */
int vfs_path_lookup_child_for_walk(const struct vfs_path *parent,const char *name,
                                   struct vfs_path *out);
/** @brief Recognize an immediate-commit control by retained provider/key.
 * @param path Borrowed valid inode path, protected by execution ownership.
 * @return True only when its live provider promises immediate commit. */
bool vfs_pseudo_immediate_control(const struct vfs_path *path) __attribute__((weak));
bool vfs_path_covered(const struct vfs_namespace *ns,const struct vfs_path *path);
void vfs_follow_mounts(const struct vfs_namespace *ns,struct vfs_path *path);
/* Bind metadata returned by a completed native creation without re-looking up
 * the now-published name. Retain/publish one actual child identity. */
int vfs_bind_child(const struct vfs_path *parent,const char *name,
                       const struct storage_node *value,struct vfs_path *out);
void vfs_dentry_put(struct vfs_mount *mount,struct vfs_dentry *dentry);
/* Generation-guarded borrowed metadata, never cached authorization. Get returns
 * 1 for a positive result, -ENOENT for a negative hit, or 0 for a miss. */
int vfs_lookup_cache_get(const struct vfs_path *parent,const char *name,uint64_t generation,
                          struct storage_node *out);
void vfs_lookup_cache_store(const struct vfs_path *parent,const char *name,uint64_t generation,
                             const struct storage_node *node);
void vfs_lookup_cache_purge(struct vfs_mount *mount);
void vfs_path_parent(const struct vfs_path *root,struct vfs_path *path);
/* Initialize common type/size/identity fields before provider-specific stat. */
int vfs_stat_basic(const struct storage_node *node,struct linux_stat_abi *out);
#endif
