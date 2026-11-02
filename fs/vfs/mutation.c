/* SPDX-License-Identifier: Apache-2.0 */
#include "mount_internal.h"
#include <linux/mount.h>

/** @brief Determine terminal deletion before a committed unlink or replacement.
 * @param path Borrowed retained target path.
 * @return Zero for a nonterminal inode or no watchers, one for a known last
 * link, two if stat failed and a final recheck
 * is needed. No result changes the mutation status. Execution writer keeps
 * the borrowed path and provider live; no reference is acquired.
 */
static unsigned vfs_notify_last_link(const struct vfs_path *path)
{
    if(!vfs_notify_interested || !vfs_notify_interested())return 0;
    if(path->dentry->node.type==RELIEFOS_FS_TYPE_DIR)return 1;
    struct linux_stat_abi status;
    int rc=vfs_path_stat(path,&status);
    return rc ? 2 : status.st_nlink<=1 ? 1 : 0;
}

int vfs_path_remove(struct vfs_namespace *ns,const struct vfs_path *path,bool directory)
{
    if(!ns || !vfs_path_valid(path))return -EINVAL;
    if(path->mount->ns!=ns)return -EXDEV;
    if(path->dentry->disconnected)return -ENOENT;
    if(path->dentry==path->mount->root || vfs_path_covered(ns,path))return -EBUSY;
    if((path->dentry->node.type==RELIEFOS_FS_TYPE_DIR)!=directory)
        return directory ? -ENOTDIR : -EISDIR;
    if(vfs_path_mount_flags(path)&MS_RDONLY)return -EROFS;
    if(!path->mount->ops->remove_node)return -EROFS;
    struct reliefos_permissions mode;
    int rc=vfs_path_permissions(path,&mode,false);
    unsigned terminal=!rc ? vfs_notify_last_link(path) : 0;
    if(!rc)rc=path->mount->ops->remove_node(path->mount->context,path,directory);
    if(!rc) {
        if(!directory && vfs_notify_inode)vfs_notify_inode(path,IN_ATTRIB);
        if(vfs_notify_child && path->dentry->parent) {
            struct vfs_path parent={path->mount,path->dentry->parent};
            vfs_notify_child(&parent,IN_DELETE,0,path->dentry->name,directory);
        }
        if(terminal && vfs_notify_terminal)vfs_notify_terminal(path,terminal==2);
        vfs_path_unlinked(path);
    }
    return rc;
}

int vfs_path_rename(struct vfs_namespace *ns,const struct vfs_path *source,
                       const struct vfs_path *parent,const char *name)
{
    struct vfs_rename *change=NULL;
    int rc=vfs_rename_prepare(ns,source,parent,name,&change);if(rc)return rc;
    if(change->same){vfs_rename_abort(change);return 0;}
    if(vfs_path_mount_flags(source)&MS_RDONLY)rc=-EROFS;
    else if(!source->mount->ops->rename_node)rc=-EROFS;
    struct reliefos_permissions mode;
    if(!rc)rc=vfs_path_permissions(source,&mode,false);
    if(!rc && vfs_path_valid(&change->target))rc=vfs_path_permissions(&change->target,&mode,false);
    unsigned terminal=!rc && vfs_path_valid(&change->target) ? vfs_notify_last_link(&change->target) : 0;
    bool committed=false;
    if(!rc)rc=source->mount->ops->rename_node(source->mount->context,source,parent,name,&change->target,&committed);
    if(committed) {
        if(vfs_notify_rename)vfs_notify_rename(source,parent,name,&change->target);
        if(terminal && vfs_notify_terminal)vfs_notify_terminal(&change->target,terminal==2);
        vfs_rename_commit(change);
        return rc==-EAGAIN ? -EIO : rc;
    }
    vfs_rename_abort(change);return rc ? rc : -EIO;
}

int vfs_path_link(struct vfs_namespace *ns,const struct vfs_path *source,
                     const struct vfs_path *parent,const char *name)
{
    if(!ns || !vfs_path_valid(source) || !vfs_path_valid(parent))return -EINVAL;
    if(source->dentry->node.type==RELIEFOS_FS_TYPE_DIR)return -EPERM;
    if(source->mount!=parent->mount || source->mount->ns!=ns)return -EXDEV;
    if(vfs_path_mount_flags(source)&MS_RDONLY)return -EROFS;
    struct vfs_path existing={0};int rc=vfs_path_lookup_child(parent,name,&existing);
    if(!rc){vfs_path_put(&existing);return -EEXIST;}
    if(rc!=-ENOENT)return rc;
    if(!source->mount->ops->link_node)return -EOPNOTSUPP;
    struct linux_stat_abi attributes;
    rc=vfs_path_stat(source,&attributes);
    if(!rc && !attributes.st_nlink)rc=-ENOENT;
    if(!rc)rc=source->mount->ops->link_node(source->mount->context,&source->dentry->node,&parent->dentry->node,name);
    if(!rc) {
        if(vfs_notify_inode)vfs_notify_inode(source,IN_ATTRIB);
        if(vfs_notify_child)vfs_notify_child(parent,IN_CREATE,0,name,false);
    }
    return rc;
}
