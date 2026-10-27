/* SPDX-License-Identifier: Apache-2.0 */
#include "mount_internal.h"

/** @brief Test whether a name is an attachment point, without following it.
 * @param ns Execution-owned namespace, borrowed.
 * @param path Held dentry identity, borrowed.
 * @return True when a mount covers this exact mount/dentry pair.
 */
bool vfs_path_covered(const struct vfs_namespace *ns,const struct vfs_path *path)
{
    for(const struct vfs_mount *mount=ns->mounts;mount;mount=mount->next)
        if(vfs_path_equal(&mount->point,path))return true;
    return false;
}

void vfs_path_unlinked(const struct vfs_path *path)
{
    if(!vfs_path_valid(path) || path->dentry==path->mount->root)return;
    path->dentry->disconnected=true;
}

void vfs_rename_abort(struct vfs_rename *change)
{
    if(!change)return;
    vfs_path_put(&change->source);vfs_path_put(&change->parent);vfs_path_put(&change->target);
    kernel_free(change->name);kernel_free(change);
}

int vfs_rename_prepare(struct vfs_namespace *ns,const struct vfs_path *source,
                         const struct vfs_path *parent,const char *name,
                         struct vfs_rename **out)
{
    if(!out)return -EINVAL;*out=NULL;
    if(!ns || !vfs_path_valid(source) || !vfs_path_valid(parent) || !name)return -EINVAL;
    if(source->mount->ns!=ns || parent->mount->ns!=ns || source->mount!=parent->mount)return -EXDEV;
    if(parent->dentry->node.type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    if(source->dentry->disconnected || parent->dentry->disconnected)return -ENOENT;
    if(source->dentry==source->mount->root || vfs_path_covered(ns,source))return -EBUSY;
    size_t length=0;
    for(;name[length];++length) {
        if(length==VFS_NAME_MAX)return -ENAMETOOLONG;
        if(name[length]=='/')return -EINVAL;
    }
    if(!length || (name[0]=='.' && (!name[1] || (name[1]=='.' && !name[2]))))return -EINVAL;
    if(source->dentry->node.type==RELIEFOS_FS_TYPE_DIR)
        for(struct vfs_dentry *ancestor=parent->dentry;ancestor;ancestor=ancestor->parent)
            if(ancestor==source->dentry)return -EINVAL;
    struct vfs_rename *change=kernel_malloc(sizeof(*change));if(!change)return -ENOMEM;
    *change=(struct vfs_rename){0};change->name=kernel_malloc(length+1);
    if(!change->name){kernel_free(change);return -ENOMEM;}
    __builtin_memcpy(change->name,name,length+1);
    vfs_path_get(source,&change->source);vfs_path_get(parent,&change->parent);
    int rc=vfs_path_lookup_child(parent,name,&change->target);
    if(rc==-ENOENT)rc=0;
    if(!rc && vfs_path_covered(ns,&change->target))rc=-EBUSY;
    if(rc){vfs_rename_abort(change);return rc;}
    change->same=vfs_path_valid(&change->target) &&
        source->mount->ops->same_node(source->mount->context,&source->dentry->node,&change->target.dentry->node);
    *out=change;return 0;
}

void vfs_rename_commit(struct vfs_rename *change)
{
    if(!change)return;
    if(!change->same) {
        struct vfs_dentry *source=change->source.dentry,*oldparent=source->parent;
        struct vfs_dentry **link=&oldparent->children;
        while(*link && *link!=source)link=&(*link)->next;
        if(*link)*link=source->next;
        vfs_path_unlinked(&change->target);
        source->parent=change->parent.dentry;++source->parent->references;
        source->next=source->parent->children;source->parent->children=source;
        kernel_free(source->name);source->name=change->name;change->name=NULL;
        vfs_dentry_put(change->source.mount,oldparent);
    }
    vfs_rename_abort(change);
}
