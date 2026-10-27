/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Select and enumerate one retained native or builtin device directory.
 * @param node Borrowed actual directory. @param cursor Required opaque cursor.
 * @param entry Required metadata output. @param policy Optional attachment-local
 * flags; NULL selects the native instance defaults for internal consumers.
 * @return One entry, zero EOF or negative errno. Execution ownership required;
 * no graph/module lock across provider I/O; native selection always restored.
 */
static int storage_readdir_node_unlocked(const struct storage_node *node,uint64_t *cursor,
                                           struct reliefos_dir_entry *entry,const uint64_t *policy)
{
    if(!node || !cursor || !entry)return -EINVAL;
    if(node->type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    if(node->flags&STORAGE_NODE_FLAG_DEV_DIR)return storage_devfs_readdir_node(node,cursor,entry);
    struct storage_volume *previous=NULL;int rc=storage_select_node_volume(node,&previous);if(rc)return rc;
    const struct storage_filesystem_ops *ops=g_storage.fs_ops;
    rc=ops && ops->readdir ? ops->readdir(&g_storage,node,cursor,entry,
        policy ? *policy : g_storage.mount_flags) : -EOPNOTSUPP;
    storage_restore_volume(previous);return rc;
}

/** @brief Enumerate a retained directory with its native instance's defaults.
 * @param node Borrowed directory. @param cursor Required opaque position.
 * @param entry Required output metadata.
 * @return One entry, zero EOF or negative errno. Recursively enters execution;
 * caller pins inode/provider, no graph/module lock spans callback I/O.
 */
int storage_readdir_node(const struct storage_node *node,uint64_t *cursor,struct reliefos_dir_entry *entry)
{
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    int rc=storage_readdir_node_unlocked(node,cursor,entry,NULL);
    kernel_execution_unlock_irqrestore(flags);return rc;
}

/** @brief Enumerate a retained directory with its actual attachment's policy.
 * @param node Borrowed directory. @param cursor Required opaque position.
 * @param entry Required metadata output. @param mount_flags Effective flags.
 * @return One entry, zero EOF or negative errno. Recursively enters execution;
 * caller pins inode/provider, native selection restored and no callback lock.
 */
int storage_readdir_node_policy(const struct storage_node *node,uint64_t *cursor,
                                  struct reliefos_dir_entry *entry,uint64_t mount_flags)
{
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    int rc=storage_readdir_node_unlocked(node,cursor,entry,&mount_flags);
    kernel_execution_unlock_irqrestore(flags);return rc;
}
