/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"

/** @brief Query/store authorized ownership and mode through the instance provider.
 * @param node Borrowed retained native inode. @param value Required metadata.
 * @param write Store an already-authorized value if true, otherwise query.
 * @return Zero or negative input/unsupported/provider errno. Recursively enters
 * execution, restores selection; provider/instance must be pinned by caller.
 * Credentials are checked by the VFS caller, no graph/module lock across I/O.
 */
int storage_inode_permissions(const struct storage_node *node,struct reliefos_permissions *value,bool write)
{
    if(!node || !value)return -EINVAL;
    if(!(node->flags&(STORAGE_NODE_FLAG_EXT2|STORAGE_NODE_FLAG_TMPFS)))return -EOPNOTSUPP;
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    struct storage_volume *previous=NULL;int rc=storage_select_node_volume(node,&previous);
    if(!rc) {
        const struct storage_filesystem_ops *ops=g_storage.fs_ops;
        rc=ops && ops->permissions ? ops->permissions(&g_storage,node,value,write) : -EOPNOTSUPP;
    }
    storage_restore_volume(previous);kernel_execution_unlock_irqrestore(flags);return rc;
}

/** @brief Report native stat metadata through the retained instance provider.
 * @param node Borrowed retained native inode. @param value Required stat output.
 * @return Zero or negative input/unsupported/provider errno. Recursively enters
 * execution, restores selection; provider/instance must be pinned by caller.
 * No graph/module lock spans metadata I/O or provider code.
 */
int storage_inode_stat(const struct storage_node *node,struct linux_stat_abi *value)
{
    if(!node || !value)return -EINVAL;
    if(!(node->flags&(STORAGE_NODE_FLAG_EXT2|STORAGE_NODE_FLAG_TMPFS)))return -EOPNOTSUPP;
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    struct storage_volume *previous=NULL;int rc=storage_select_node_volume(node,&previous);
    if(!rc) {
        const struct storage_filesystem_ops *ops=g_storage.fs_ops;
        rc=ops && ops->stat ? ops->stat(&g_storage,node,value) : -EOPNOTSUPP;
    }
    storage_restore_volume(previous);kernel_execution_unlock_irqrestore(flags);return rc;
}

/** @brief Store authorized inode timestamps through the retained instance provider.
 * @param node Borrowed retained native inode. @param atime New access seconds.
 * @param mtime New modification seconds. @param set_atime Store access if true.
 * @param set_mtime Store modification if true.
 * @return Zero or negative unsupported/provider errno. Recursively enters
 * execution, restores selection; caller pins code/inode and authorizes change.
 * No graph/module lock spans native journal/metadata I/O.
 */
int storage_inode_utimensat(const struct storage_node *node,int64_t atime,int64_t mtime,bool set_atime,bool set_mtime)
{
    if(!node || !(node->flags&(STORAGE_NODE_FLAG_EXT2|STORAGE_NODE_FLAG_TMPFS)))return -EOPNOTSUPP;
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    struct storage_volume *previous=NULL;int rc=storage_select_node_volume(node,&previous);
    if(!rc) {
        const struct storage_filesystem_ops *ops=g_storage.fs_ops;
        rc=ops && ops->utimens ? ops->utimens(&g_storage,node,atime,mtime,set_atime,set_mtime) : -EOPNOTSUPP;
    }
    storage_restore_volume(previous);kernel_execution_unlock_irqrestore(flags);return rc;
}
