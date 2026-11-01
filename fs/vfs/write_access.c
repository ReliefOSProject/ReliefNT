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

/** @brief Claim a live inode against later writes without rejecting old OFDs.
 * @param reference Owned live regular-file inode retained by the claimant.
 * @return Zero, -EINVAL, -EROFS, -ETXTBSY for a read-deny guard, or -EBUSY
 * for an existing swap claim. The caller must separately exclude already
 * accepted writes, cached data and writable mappings before publication.
 */
int storage_inode_swap_claim(struct storage_inode_ref *reference)
{
    if (!reference || reference->node.type!=RELIEFOS_FS_TYPE_FILE) return -EINVAL;
    uint64_t flags; kernel_execution_lock_irqsave(&flags);
    int rc=reference->read_only ? -EROFS : reference->swap_active ? -EBUSY :
        reference->write_count<0 ? -ETXTBSY : 0;
    if (!rc) reference->swap_active=true;
    kernel_execution_unlock_irqrestore(flags); return rc;
}

/** @brief Release the unique active swap claim after all users have drained.
 * @param reference Original owned inode, nullable. Execution serializes users.
 */
void storage_inode_swap_release(struct storage_inode_ref *reference)
{
    if (!reference) return;
    uint64_t flags; kernel_execution_lock_irqsave(&flags);
    reference->swap_active=false;
    kernel_execution_unlock_irqrestore(flags);
}

/** @brief Observe whether the retained inode currently excludes swap mutations.
 * @param reference Borrowed live inode, nullable. @return True while claimed.
 */
bool storage_inode_swap_active(const struct storage_inode_ref *reference)
{
    if (!reference) return false;
    uint64_t flags; kernel_execution_lock_irqsave(&flags);
    bool active=reference->swap_active;
    kernel_execution_unlock_irqrestore(flags); return active;
}
