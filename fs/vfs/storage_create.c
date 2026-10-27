/* SPDX-License-Identifier: Apache-2.0 */
#include "storage_backend.h"
#include "../../block/core/storage_internal.h"
#include "../cluster.h"
#include <linux/errno.h>

/** @brief Create a native empty member after verified selected-parent lookup.
 * @param volume Borrowed pinned instance. @param parent Actual retained directory.
 * @param name Literal non-special component. @param out Required metadata output.
 * @return Zero or negative input/existence/provider errno. Execution ownership
 * required, mutation admitted before callback; native insertion rolls back if
 * stable inode allocation fails. Internal sidecars use the same primitive
 * without recursively storing metadata for themselves.
 */
int storage_cluster_new_file(struct storage_volume *volume,const struct storage_node *parent,
                                const char *name,struct storage_node *out)
{
    if(!volume || !parent || !name || !out || parent->volume_id!=volume->volume_id)return -EINVAL;
    struct storage_node directory=*parent;
    int rc=storage_inode_refresh(&directory);if(rc)return rc;
    if(directory.type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    rc=storage_backend_lookup(volume,&directory,name,out);
    if(!rc)return -EEXIST;
    if(rc!=-ENOENT)return rc;
    struct storage_volume *previous=NULL;
    rc=storage_select_node_volume(&directory,&previous);if(rc)return rc;
    storage_begin_mutation();
    const struct storage_cluster_ops *ops=volume->fs_ops ? volume->fs_ops->cluster : NULL;
    rc=ops && ops->new_file ? ops->new_file(volume,&directory,name,out) : -EOPNOTSUPP;
    storage_restore_volume(previous);return rc;
}

/** @brief Roll back only a just-created unpublished cluster member.
 * @param volume Borrowed pinned instance. @param parent Actual retained directory.
 * @param name Newly inserted component. @param node Its stable native metadata.
 * @return Zero or negative input/provider errno. Execution/mutation admission
 * required; selected native callback disconnects the key before backing reuse.
 * No old member or display pathname may be passed; no callback lock spans I/O.
 */
int storage_cluster_drop_new(struct storage_volume *volume,const struct storage_node *parent,
                               const char *name,const struct storage_node *node)
{
    if(!volume || !parent || !name || !node || parent->volume_id!=volume->volume_id)return -EINVAL;
    struct storage_volume *previous=NULL;int rc=storage_select_node_volume(parent,&previous);if(rc)return rc;
    const struct storage_cluster_ops *ops=volume->fs_ops ? volume->fs_ops->cluster : NULL;
    rc=ops && ops->drop_new ? ops->drop_new(volume,parent,name,node) : -EOPNOTSUPP;
    storage_restore_volume(previous);return rc;
}

/** @brief Create an authorized inode and reserve its native/publication metadata.
 * @param context Borrowed pinned native instance. @param parent Actual directory.
 * @param name Validated literal component. @param type Authorized Linux inode type.
 * @param value Borrowed authorized ownership/mode. @param target Optional symlink target.
 * @param out Required native node output. @param pin Optional owned inode-pin output.
 * @return Zero or input/readonly/provider/metadata errno. Execution ownership
 * required. Provider creation, sidecar persistence and final publication pins
 * roll back before exposing a VFS dentry; no graph/module lock spans native I/O.
 */
int storage_backend_create_node(void *context,const struct storage_node *parent,const char *name,uint32_t type,
                                   const struct reliefos_permissions *value,const char *target,struct storage_node *out,void **pin)
{
    if(pin)*pin=NULL;
    struct storage_volume *volume=context;
    if(!volume || !parent || !name || !value || !out || parent->volume_id!=volume->volume_id)return -EINVAL;
    const struct storage_filesystem_ops *ops=volume->fs_ops;
    if(volume->mount_flags&MS_RDONLY || (ops && (ops->flags&STORAGE_FS_READONLY)))return -EROFS;
    if(!ops || !ops->create)return -EOPNOTSUPP;
    bool sidecar=ops->flags&STORAGE_FS_SIDECAR;
    if(sidecar && type!=LINUX_S_IFREG && type!=LINUX_S_IFDIR)return -EOPNOTSUPP;
    if(sidecar && storage_text_eq_ci(name,"LEONACL.SYS"))return -EPERM;
    struct storage_node directory=*parent;int rc=storage_inode_refresh(&directory);if(rc)return rc;
    parent=&directory;
    struct storage_volume *previous=NULL;rc=storage_select_node_volume(parent,&previous);if(rc)return rc;
    storage_begin_mutation();
    rc=ops->create(volume,parent,name,type|(value->mode&07777),value,target,out);
    if(!rc && sidecar) {
        struct storage_inode_ref *metadata_pin=NULL;rc=storage_inode_get(out,&metadata_pin);
        if(!rc)rc=storage_backend_store_child(volume,parent,name,value);
        if(!rc){metadata_pin->permissions=*value;metadata_pin->permissions.mode&=07777;metadata_pin->permissions_valid=true;}
        else {int cleanup=storage_cluster_drop_new(volume,parent,name,out);if(cleanup)rc=cleanup;}
        (void)storage_inode_put(metadata_pin);
    }
    if(!rc && pin) {
        struct storage_inode_ref *reference=NULL;rc=storage_inode_get(out,&reference);
        if(!rc)*pin=reference;
        else {
            int cleanup=sidecar ? storage_cluster_drop_new(volume,parent,name,out) :
                ops->remove ? ops->remove(volume,parent,name,out,type==LINUX_S_IFDIR) : -EOPNOTSUPP;
            if(cleanup)rc=cleanup;storage_cache_invalidate();
        }
    }
    storage_restore_volume(previous);return rc;
}
