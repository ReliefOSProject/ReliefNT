/* SPDX-License-Identifier: Apache-2.0 */
#include "storage_backend.h"
#include "mount_internal.h"
#include "../../block/core/storage_internal.h"
#include <linux/errno.h>

/** @brief Remove a verified retained native member through its actual provider.
 * @param context Borrowed pinned instance. @param path Actual retained victim/parent.
 * @param directory Select rmdir, otherwise unlink.
 * @return Zero or input/readonly/stale/provider errno. Caller authorizes DAC;
 * execution ownership required, inode identity revalidated before mutation.
 * Native provider retains open backing, no graph/module lock spans native I/O.
 */
int storage_backend_remove_node(void *context,const struct vfs_path *path,bool directory)
{
    struct storage_volume *volume=context;
    if(!vfs_path_valid(path) || !path->dentry->parent)return -EINVAL;
    const struct storage_node *parent=&path->dentry->parent->node,*expected=vfs_path_node(path);
    const char *name=vfs_path_name(path);
    if(!volume || !parent || !expected || parent->volume_id!=volume->volume_id ||
       expected->volume_id!=volume->volume_id)return -EINVAL;
    if(volume->mount_flags&MS_RDONLY || (volume->fs_ops && (volume->fs_ops->flags&STORAGE_FS_READONLY)))return -EROFS;
    struct storage_node current_parent=*parent,node;
    int rc=directory ? storage_backend_prime_metadata(path) : 0;if(rc)return rc;
    rc=storage_inode_refresh(&current_parent);if(rc)return rc;
    rc=storage_backend_lookup(context,&current_parent,name,&node);if(rc)return rc;
    if(!storage_backend_same_node(context,expected,&node))return -ENOENT;
    struct storage_volume *previous=NULL;
    rc=storage_select_node_volume(&current_parent,&previous);if(rc)return rc;
    storage_begin_mutation();
    const struct storage_filesystem_ops *ops=volume->fs_ops;
    rc=ops && ops->remove ? ops->remove(volume,&current_parent,name,&node,directory) : -EOPNOTSUPP;
    if(!rc)storage_cache_invalidate();
    storage_restore_volume(previous);return rc;
}

/* The graph transaction owns all source/target/provider references. Metadata
 * required by FAT-family emulation is read before native mutation. A later
 * persistence failure is EIO with committed=true: callers must still move the
 * held graph and must never retry the already-completed namespace operation. */
/** @brief Rename preheld native operands with reserved sidecar commit state.
 * @param context Borrowed pinned instance. @param source Actual source member.
 * @param parent Actual destination directory. @param name Validated new component.
 * @param target Optional retained replacement. @param committed Required native commit output.
 * @return Zero or input/readonly/provider/metadata errno. Execution ownership
 * required. Metadata is reserved before native mutation; irreversible commit
 * stays true on later persistence errors, and no callback lock spans native I/O.
 */
int storage_backend_rename_node(void *context,const struct vfs_path *source,const struct vfs_path *parent,
                                   const char *name,const struct vfs_path *target,bool *committed)
{
    if(!committed)return -EINVAL;*committed=false;
    struct storage_volume *volume=context;
    if(!volume || !vfs_path_valid(source) || !vfs_path_valid(parent) || !source->dentry->parent ||
       source->mount!=parent->mount)return -EINVAL;
    struct storage_node old_parent=source->dentry->parent->node,new_parent=*vfs_path_node(parent);
    const struct storage_node *node=vfs_path_node(source),*replaced=vfs_path_node(target);
    if(node->volume_id!=volume->volume_id || new_parent.volume_id!=volume->volume_id)return -EXDEV;
    if(volume->mount_flags&MS_RDONLY || (volume->fs_ops && (volume->fs_ops->flags&STORAGE_FS_READONLY)))return -EROFS;
    struct reliefos_permissions saved_mode;
    bool clustered=volume->fs_ops && (volume->fs_ops->flags&STORAGE_FS_SIDECAR);
    int rc=storage_inode_refresh(&old_parent);
    if(!rc)rc=storage_inode_refresh(&new_parent);
    if(!rc && clustered)rc=storage_backend_permissions(context,source,&saved_mode,false);
    if(!rc && replaced && replaced->type==RELIEFOS_FS_TYPE_DIR)rc=storage_backend_prime_metadata(target);
    struct storage_node fresh;
    if(!rc)rc=storage_backend_lookup(context,&old_parent,vfs_path_name(source),&fresh);
    if(!rc && !storage_backend_same_node(context,node,&fresh))rc=-ENOENT;
    if(!rc) {
        int found=storage_backend_lookup(context,&new_parent,name,&fresh);
        if(replaced)rc=found ? found : storage_backend_same_node(context,replaced,&fresh) ? 0 : -ENOENT;
        else if(found!=-ENOENT)rc=found ? found : -EEXIST;
    }
    if(rc)return rc;
    struct storage_volume *previous=NULL;rc=storage_select_node_volume(&old_parent,&previous);if(rc)return rc;
    struct storage_rename_metadata *metadata=NULL;
    if(clustered)rc=storage_backend_prepare_rename_metadata(volume,source,parent,name,&saved_mode,&metadata);
    if(rc){storage_restore_volume(previous);return rc;}
    storage_begin_mutation();
    const struct storage_filesystem_ops *ops=volume->fs_ops;
    struct storage_rename_args args={.source=node,.old_parent=&old_parent,.new_parent=&new_parent,.target=replaced,
        .old_name=vfs_path_name(source),.new_name=name,.committed=committed};
    rc=ops && ops->rename ? ops->rename(volume,&args) : -EOPNOTSUPP;
    if(*committed && clustered) {
        int persisted=storage_backend_commit_rename_metadata(metadata);
        if(persisted)rc=persisted;
    }
    int cleanup=storage_backend_finish_rename_metadata(metadata,*committed);
    if(cleanup)rc=cleanup;
    if(*committed)storage_cache_invalidate();
    storage_restore_volume(previous);return rc;
}

/** @brief Publish a link through the selected native provider's inode operation.
 * @param context Borrowed pinned instance. @param source Actual source inode.
 * @param parent Retained destination directory. @param name Validated component.
 * @return Zero or input/readonly/unsupported/provider errno. Caller authorizes
 * permissions/eligibility; execution ownership and admitted mutation required,
 * native selection restored and no graph/module lock spans provider I/O.
 */
int storage_backend_link_node(void *context,const struct storage_node *source,
                                 const struct storage_node *parent,const char *name)
{
    struct storage_volume *volume=context;
    if(!volume || !source || !parent || !name || source->volume_id!=volume->volume_id ||
       parent->volume_id!=volume->volume_id)return -EINVAL;
    if(volume->mount_flags&MS_RDONLY)return -EROFS;
    const struct storage_filesystem_ops *ops=volume->fs_ops;
    if(!ops || !ops->link)return -EOPNOTSUPP;
    struct storage_node inode=*source,directory=*parent;
    int rc=storage_inode_refresh(&inode);
    if(!rc)rc=storage_inode_refresh(&directory);
    if(rc)return rc;
    struct storage_volume *previous=NULL;rc=storage_select_node_volume(&directory,&previous);if(rc)return rc;
    storage_begin_mutation();
    rc=ops->link(volume,&inode,&directory,name);
    if(!rc)storage_cache_invalidate();
    storage_restore_volume(previous);return rc;
}
