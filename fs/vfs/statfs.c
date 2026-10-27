/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"

/** @brief Query filesystem capacity through the retained instance's provider.
 * @param node Borrowed pinned actual inode or builtin synthetic device node.
 * @param value Required caller-owned statfs output, unspecified on error.
 * @return Zero or negative input/provider errno. Recursively enters execution;
 * native selection restored, no graph/module lock spans native metadata I/O.
 * Device directories use the permanent core's synthetic ramfs description.
 */
int storage_statfs(const struct storage_node *node,struct linux_statfs_abi *value)
{
    if(!node || !value)return -EINVAL;
    *value=(struct linux_statfs_abi){.f_namelen=RELIEFOS_FS_NAME_LEN-1,.f_flags=LINUX_ST_VALID};
    if(node->flags&(STORAGE_NODE_FLAG_DEV_NODE|STORAGE_NODE_FLAG_DEV_DIR|STORAGE_NODE_FLAG_DEV_FB0)) {
        value->f_type=0x858458f6;value->f_bsize=value->f_frsize=4096;return 0;
    }
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    struct storage_volume *previous=NULL;int rc=storage_select_node_volume(node,&previous);
    if(!rc) {
        value->f_fsid[0]=node->volume_id+1;
        const struct storage_filesystem_ops *ops=g_storage.fs_ops;
        rc=ops && ops->statfs ? ops->statfs(&g_storage,value) : -EOPNOTSUPP;
        value->f_frsize=value->f_bsize;
    }
    storage_restore_volume(previous);kernel_execution_unlock_irqrestore(flags);return rc;
}
