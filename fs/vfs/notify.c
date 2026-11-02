/* SPDX-License-Identifier: Apache-2.0 */
#define RELIEFNT_FSNOTIFY_VFS_IMPLEMENTATION
#include "mount_internal.h"
#include <reliefnt/fsnotify.h>

/* These lists borrow live objects only. They do not acquire references and
 * are accessed exclusively while the kernel execution writer is held. */
static struct vfs_mount *notify_mounts;
static struct vfs_dentry *notify_dentries;

/** @brief Capture the weak provider and inode identity of a held path.
 * @param path Borrowed retained VFS path, or NULL.
 * @param key Required weak-key output, cleared on invalid path.
 * @return Zero or -EINVAL. Execution writer is required; the caller keeps the
 * provider code/context live, and the copied key acquires no reference.
 */
int vfs_notify_key(const struct vfs_path *path,struct fsnotify_key *key)
{
    if(!key)return -EINVAL;
    *key=(struct fsnotify_key){0};
    if(!vfs_path_valid(path))return -EINVAL;
    *key=(struct fsnotify_key){.ops=path->mount->ops,.context=path->mount->context,
                               .node=path->dentry->node};
    return 0;
}

/** @brief Check whether any actual watch needs mutation notifications.
 * @return True when at least one watch exists. Execution writer is required;
 * the query transfers no path, inode, mount, or provider ownership.
 */
bool vfs_notify_interested(void)
{return fsnotify_has_watches();}

/** @brief Consult the weak watch set before evicting a FAT/exFAT inode key.
 * @param node Borrowed native storage identity, or NULL.
 * @return True when a watch needs this generation preserved. Execution writer
 * is required; no cache, inode, mount, or provider reference is acquired.
 */
bool vfs_notify_storage_node_watched(const struct storage_node *node)
{return fsnotify_storage_node_watched(node);}

/** @brief Report a direct inode event with Linux's directory activity flag.
 * @param path Borrowed retained inode path, or NULL.
 * @param mask Linux event bits without a rename cookie.
 * Execution writer keeps the path and provider live; no reference is retained.
 * Delivery failure becomes queue overflow and cannot fail the operation.
 */
void vfs_notify_inode(const struct vfs_path *path,uint32_t mask)
{
    struct fsnotify_key key;
    if(!fsnotify_has_watches() || vfs_notify_key(path,&key))return;
    if(key.node.type==RELIEFOS_FS_TYPE_DIR && !(mask & (IN_MOVE_SELF|IN_DELETE_SELF)))mask|=IN_ISDIR;
    fsnotify_emit(&key,mask,0,NULL,false);
}

/** @brief Report one named child event to the watched directory inode.
 * @param parent Borrowed retained parent path, or NULL.
 * @param mask Linux child event bits.
 * @param cookie Rename correlation cookie or zero.
 * @param name Borrowed optional basename, copied during delivery.
 * @param directory True when the child is a directory.
 * Execution writer is required; no path, inode, or provider pin is acquired.
 */
void vfs_notify_child(const struct vfs_path *parent,uint32_t mask,uint32_t cookie,
                      const char *name,bool directory)
{
    struct fsnotify_key key;
    if(!fsnotify_has_watches() || vfs_notify_key(parent,&key))return;
    fsnotify_emit(&key,mask|(directory ? IN_ISDIR : 0),cookie,name,false);
}

/** @brief Report descriptor activity to the actual inode and its old parent.
 * @param path Borrowed retained inode path, or NULL.
 * @param mask Linux activity event bits.
 * Execution writer keeps the path and provider live; parent delivery identifies
 * a disconnected child for EXCL_UNLINK and retains no additional reference.
 */
void vfs_notify_event(const struct vfs_path *path,uint32_t mask)
{
    if(!fsnotify_has_watches() || !vfs_path_valid(path))return;
    struct fsnotify_key key;
    if(vfs_notify_key(path,&key))return;
    if(key.node.type==RELIEFOS_FS_TYPE_DIR)mask|=IN_ISDIR;
    fsnotify_emit(&key,mask,0,NULL,false);
    struct vfs_dentry *child=path->dentry;
    if(!child->parent || !child->name || !child->name[0])return;
    struct vfs_path parent={path->mount,child->parent};
    if(!vfs_notify_key(&parent,&key))fsnotify_emit(&key,mask,0,child->name,child->disconnected);
}

/** @brief Record a mount without extending its provider or graph lifetime.
 * @param mount Borrowed constructed mount with live provider and root.
 * Execution writer is required. The weak registry entry must be removed before
 * provider destruction; registration itself transfers no ownership.
 */
void vfs_notify_mount_created(struct vfs_mount *mount)
{
    mount->notify_next=notify_mounts;mount->notify_link=&notify_mounts;
    if(notify_mounts)notify_mounts->notify_link=&mount->notify_next;
    notify_mounts=mount;
}

/** @brief Invalidate a domain before the last live provider is destroyed.
 * @param mount Borrowed retiring mount whose provider finish has succeeded.
 * Execution writer is required; unregisters the weak entry and emits UNMOUNT
 * only if no other registered mount shares its live operations and context.
 */
void vfs_notify_mount_retired(struct vfs_mount *mount)
{
    if(!mount->notify_link)return;
    *mount->notify_link=mount->notify_next;
    if(mount->notify_next)mount->notify_next->notify_link=mount->notify_link;
    mount->notify_link=NULL;
    for(struct vfs_mount *other=notify_mounts;other;other=other->notify_next)
        if(other->ops==mount->ops && other->context==mount->context)return;
    fsnotify_unmount(mount->ops,mount->context);
}

/** @brief Record a cached inode alias without adding an inode or mount pin.
 * @param mount Borrowed live owning mount and backend domain.
 * @param dentry Borrowed retained dentry registered until its final release.
 * Execution writer is required; neither pointer is retained by reference.
 */
void vfs_notify_dentry_created(struct vfs_mount *mount,struct vfs_dentry *dentry)
{
    dentry->notify_mount=mount;dentry->notify_next=notify_dentries;dentry->notify_link=&notify_dentries;
    if(notify_dentries)notify_dentries->notify_link=&dentry->notify_next;
    notify_dentries=dentry;
}

/** @brief Compare two live cached dentries by actual backend inode identity.
 * @param a First borrowed registered dentry.
 * @param b Second borrowed registered dentry.
 * @return True for the same inode in the same backend domain. Execution writer
 * keeps both weak registry entries and their provider callbacks live; no pin
 * is acquired or transferred during comparison.
 */
static bool vfs_notify_same_dentry(const struct vfs_dentry *a,const struct vfs_dentry *b)
{
    struct vfs_mount *left=a->notify_mount,*right=b->notify_mount;
    return left && right && left->ops==right->ops && left->context==right->context &&
        left->ops->same_node(left->context,&a->node,&b->node);
}

/** @brief Mark every live alias after a known or possible final-link removal.
 * @param path Borrowed retained path to the removed inode, or NULL.
 * @param recheck True when a failed pre-unlink stat requires an nlink recheck.
 * Execution writer is required; aliases are weak registry entries and no
 * dentry, inode, mount, or provider reference is acquired.
 */
void vfs_notify_terminal(const struct vfs_path *path,bool recheck)
{
    if(!fsnotify_has_watches() || !vfs_path_valid(path))return;
    for(struct vfs_dentry *alias=notify_dentries;alias;alias=alias->notify_next)
        if(vfs_notify_same_dentry(path->dentry,alias)) {
            alias->notify_deleted=true;alias->notify_recheck=recheck;
        }
}

/** @brief Retire a weak alias and signal terminal deletion at the last alias.
 * @param mount Borrowed live owning mount whose backend has not been released.
 * @param dentry Borrowed registered dentry immediately before backend release.
 * Execution writer is required. An uncertain last link is rechecked while the
 * provider remains live; registry removal transfers no object ownership.
 */
void vfs_notify_dentry_retired(struct vfs_mount *mount,struct vfs_dentry *dentry)
{
    if(!dentry->notify_link)return;
    *dentry->notify_link=dentry->notify_next;
    if(dentry->notify_next)dentry->notify_next->notify_link=dentry->notify_link;
    dentry->notify_link=NULL;
    if(!dentry->notify_deleted || !fsnotify_has_watches())return;
    for(struct vfs_dentry *alias=notify_dentries;alias;alias=alias->notify_next)
        if(vfs_notify_same_dentry(dentry,alias))return;
    /* A transient pre-unlink stat failure marks aliases conservatively. If the
     * inode still has a link, retain its watch after the last cached alias. */
    if(dentry->notify_recheck) {
        struct vfs_path path={mount,dentry};struct linux_stat_abi status;
        if(!vfs_path_stat(&path,&status) && status.st_nlink)return;
    }
    struct fsnotify_key key={.ops=mount->ops,.context=mount->context,.node=dentry->node};
    fsnotify_delete(&key);
}

/** @brief Publish old/new parent moves and inode events before dentry mutation.
 * @param source Borrowed retained source with its old parent and name.
 * @param parent Borrowed retained destination parent.
 * @param name Borrowed destination basename, copied during delivery.
 * @param target Borrowed replaced target path, possibly empty.
 * Execution writer keeps all paths and providers live. A nonzero shared cookie
 * correlates the parent events; no input reference is retained or transferred.
 */
void vfs_notify_rename(const struct vfs_path *source,const struct vfs_path *parent,
                       const char *name,const struct vfs_path *target)
{
    if(!fsnotify_has_watches() || !vfs_path_valid(source) || !source->dentry->parent)return;
    uint32_t cookie=fsnotify_next_cookie();
    bool directory=source->dentry->node.type==RELIEFOS_FS_TYPE_DIR;
    struct vfs_path oldparent={source->mount,source->dentry->parent};
    vfs_notify_child(&oldparent,IN_MOVED_FROM,cookie,source->dentry->name,directory);
    vfs_notify_child(parent,IN_MOVED_TO,cookie,name,directory);
    if(vfs_path_valid(target))vfs_notify_inode(target,IN_ATTRIB);
    vfs_notify_inode(source,IN_MOVE_SELF);
}
