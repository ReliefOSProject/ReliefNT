/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include <linux/errno.h>

int storage_inode_write_get(struct storage_inode_ref *reference)
{
    if(!reference || reference->node.type!=RELIEFOS_FS_TYPE_FILE)return -EINVAL;
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    int rc=reference->read_only ? -EROFS : reference->write_count<0 ? -ETXTBSY :
        reference->write_count==INT64_MAX ? -EOVERFLOW : 0;
    if(!rc)++reference->write_count;
    kernel_execution_unlock_irqrestore(flags);return rc;
}
void storage_inode_write_put(struct storage_inode_ref *reference)
{
    if(!reference)return;
    uint64_t flags;kernel_execution_lock_irqsave(&flags);--reference->write_count;
    kernel_execution_unlock_irqrestore(flags);
}
int storage_inode_deny_write(struct storage_inode_ref *reference)
{
    if(!reference || reference->node.type!=RELIEFOS_FS_TYPE_FILE)return -EINVAL;
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    int rc=reference->write_count>0 ? -ETXTBSY :
        reference->write_count==INT64_MIN ? -EOVERFLOW : 0;
    if(!rc)--reference->write_count;
    kernel_execution_unlock_irqrestore(flags);return rc;
}
void storage_inode_allow_write(struct storage_inode_ref *reference)
{
    if(!reference)return;
    uint64_t flags;kernel_execution_lock_irqsave(&flags);++reference->write_count;
    kernel_execution_unlock_irqrestore(flags);
}
