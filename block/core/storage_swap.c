/* SPDX-License-Identifier: Apache-2.0 */
#include "storage_internal.h"
#include <reliefnt/swap_internal.h>
#include <linux/errno.h>

/* Protected by execution ownership. The token belongs to the current stack and
 * is removed before that transaction may suspend or release its owner. */
static struct swap_storage_token *swap_transaction;
static bool swap_cleanup_scope;

/** @brief Admit a bounded synchronous exit-cleanup operation, including PID 0.
 * @return Zero or EDEADLK if the outer writer or exclusive cleanup gate is absent.
 * No scope, token or raw target pointer may survive the enclosing transaction.
 */
int swap_storage_cleanup_enter(void)
{
    if (!kernel_execution_is_outermost() || swap_cleanup_scope || swap_transaction)
        return -EDEADLK;
    swap_cleanup_scope = true;
    return 0;
}

/** @brief End the current outer-writer cleanup admission. */
void swap_storage_cleanup_leave(void)
{
    swap_cleanup_scope = false;
}

/** @brief Reserve all storage transport state for a forced synchronous phase.
 * @param token Required empty caller-owned token. @return Zero or terminal errno.
 * Caller retains execution ownership without suspension through matching end.
 */
int swap_storage_begin(struct swap_storage_token *token)
{
    if (!token || token->active) return -EINVAL;
    if (swap_transaction || storage_task_io_owner) return -EBUSY;
    uint32_t pid=sched_current_pid();
    if (!pid && !swap_cleanup_scope) return -ESRCH;
    *token=(struct swap_storage_token){.pid=pid,.async=storage_io_async_context,
        .write_started=storage_io_write_started,.active=true};
    swap_transaction=token; storage_task_io_owner=pid;
    storage_io_async_context=false; storage_io_write_started=true;
    return 0;
}

/** @brief Test failed admission's competing PID under execution ownership.
 * @return True only for an existing reservation owned by another task.
 */
bool swap_storage_other_owner(void)
{
    uint32_t pid = sched_current_pid();
    return storage_task_io_owner && (swap_cleanup_scope || (pid && storage_task_io_owner != pid));
}

/** @brief Identify an ordinary provider command owned by the current task.
 * @return True only for a non-swap reservation owned by the current PID.
 */
bool swap_storage_current_owner(void)
{
    uint32_t pid = sched_current_pid();
    return !swap_transaction && pid && storage_task_io_owner == pid;
}

/** @brief Test whether the exclusive token is transferring a swap slot.
 * @return True only during the synchronous slot phase, not activation.
 */
bool swap_storage_slot_active(void)
{
    return swap_transaction && swap_transaction->active && swap_transaction->slot;
}

/** @brief Detect all storage ownership before a new pressure transaction.
 * @return True for a regular owner, any swap token, or cleanup scope.
 */
bool swap_storage_busy(void)
{ return swap_transaction || swap_cleanup_scope || storage_task_io_owner; }

/** @brief Consume the exact synchronous phase and restore inherited state.
 * @param token Original active token; invalid tokens are ignored.
 * Execution ownership remains held and no pending swap command may survive.
 */
void swap_storage_end(struct swap_storage_token *token)
{
    if (!token || !token->active || swap_transaction!=token) return;
    storage_io_async_context=token->async;
    storage_io_write_started=token->write_started;
    storage_task_io_owner=0; swap_transaction=NULL; token->active=false;
}

/** @brief Perform one complete fixed-block callback with shared transport exclusion.
 * @param token Original live synchronous phase. @param reference Owned device.
 * @param first First logical block. @param count Complete bounded block count.
 * @param buffer Stable kernel bytes. @param write True for a block write.
 * @return Zero, -EOPNOTSUPP for unsupported geometry, -EOVERFLOW for sector
 * conversion overflow, or another terminal errno. Unexpected EAGAIN is a
 * provider-contract failure, not an owned swap continuation. Before every
 * attempted write, cache callbacks evict clean aliases without allocation or I/O;
 * partial or failed provider writes cannot leave an old clean alias behind.
 */
int swap_storage_transfer(const struct swap_storage_token *token,
                          const struct block_device_ref *reference,uint64_t first,
                          uint32_t count,void *buffer,bool write)
{
    if (!token || !token->active || swap_transaction!=token ||
        token->pid!=sched_current_pid() || !reference || !buffer) return -EINVAL;
    struct block_device_info info;
    int rc=block_device_info(reference,&info); if (rc) return rc;
    if (!(info.flags & BLOCK_DEVICE_SWAP_SAFE)) return -EOPNOTSUPP;
    if (!count || count>info.max_blocks || first>=info.blocks || count>info.blocks-first)
        return -EINVAL;
    if (write) {
        uint32_t sectors_per_block=info.block_size/512u;
        if (!sectors_per_block || info.block_size%512u) return -EOPNOTSUPP;
        if (first>UINT64_MAX/sectors_per_block || count>UINT32_MAX/sectors_per_block)
            return -EOVERFLOW;
        uint64_t lba=first*sectors_per_block;
        uint32_t sectors=count*sectors_per_block;
        if (lba>UINT64_MAX-sectors) return -EOVERFLOW;
        /* A provider may write some sectors even when it returns an error.
         * Evict clean aliases first, under execution ownership and before the
         * transport lock; device/code ownership remains with reference. */
        storage_filesystem_cache_event_device(reference->device,lba,sectors);
    }
    kernel_spin_lock(&storage_transport_lock);
    rc=write ? block_device_write(reference,first,count,buffer) :
               block_device_read(reference,first,count,buffer);
    kernel_spin_unlock(&storage_transport_lock);
    return rc==-EAGAIN ? -EIO : rc;
}

/** @brief Flush an audited device without retaining an asynchronous command.
 * @param token Original active transaction. @param reference Held device/code.
 * @return Zero or terminal errno. Caller retains execution ownership.
 */
int swap_storage_flush(const struct swap_storage_token *token,
                       const struct block_device_ref *reference)
{
    if (!token || !token->active || swap_transaction!=token ||
        token->pid!=sched_current_pid() || !reference) return -EINVAL;
    struct block_device_info info;
    int rc=block_device_info(reference,&info); if (rc) return rc;
    if (!(info.flags & BLOCK_DEVICE_SWAP_SAFE)) return -EOPNOTSUPP;
    kernel_spin_lock(&storage_transport_lock);
    rc=block_device_flush(reference);
    kernel_spin_unlock(&storage_transport_lock);
    return rc==-EAGAIN ? -EIO : rc;
}

/* All records are area-owned, pinned through their held device reference, and
 * linked only while execution ownership serializes all claim users. */
static struct swap_storage_claim *swap_claims;

/** @brief Compare validated nonempty ranges without overflowing their ends.
 * @param a First start. @param an First count. @param b Second start.
 * @param bn Second count. @return True when the two ranges overlap.
 */
static bool swap_ranges_overlap(uint64_t a,uint64_t an,uint64_t b,uint64_t bn)
{
    return a<=b ? b-a<an : a-b<bn;
}

/** @brief Reserve a whole physical backing range until explicit release.
 * @param claim Empty area-owned record. @param reference Owned block reference.
 * @param first First physical block. @param count Nonzero block extent.
 * @param file Whether the range represents a filesystem with inode exclusion.
 * @return Zero or -EINVAL/-EBUSY/provider errno. Caller holds execution ownership.
 */
int swap_storage_claim_range(struct swap_storage_claim *claim,
                             const struct block_device_ref *reference,uint64_t first,
                             uint64_t count,bool file)
{
    if (!claim || claim->active || !reference || !reference->device || !count) return -EINVAL;
    struct block_device_info info; int rc=block_device_info(reference,&info); if (rc) return rc;
    if (first>=info.blocks || count>info.blocks-first) return -EINVAL;
    for (struct swap_storage_claim *at=swap_claims; at; at=at->next)
        if (at->device==reference->device && (!file || !at->file) &&
            swap_ranges_overlap(first,count,at->first,at->count)) return -EBUSY;
    *claim=(struct swap_storage_claim){.next=swap_claims,.device=reference->device,
        .first=first,.count=count,.file=file,.active=true};
    swap_claims=claim; return 0;
}

/** @brief Remove only the exact live range record.
 * @param claim Original area-owned record, NULL ignored. Execution-owned.
 */
void swap_storage_release_range(struct swap_storage_claim *claim)
{
    if (!claim || !claim->active) return;
    struct swap_storage_claim **link=&swap_claims;
    while (*link && *link!=claim) link=&(*link)->next;
    if (!*link) return;
    *link=claim->next; *claim=(struct swap_storage_claim){0};
}

/** @brief Reject raw aliases into retained storage or whole-disk table edits.
 * @param reference Borrowed exact block identity. @param first First block.
 * @param count Nonzero blocks. @param whole True for whole-disk mutation.
 * @return One if excluded, zero when clear, -EINVAL for invalid input.
 */
int swap_storage_raw_busy(const struct block_device_ref *reference,uint64_t first,
                          uint64_t count,bool whole)
{
    if (!reference || !reference->device || !count || count-1>UINT64_MAX-first) return -EINVAL;
    for (struct swap_storage_claim *at=swap_claims; at; at=at->next)
        if (at->device==reference->device &&
            (whole || swap_ranges_overlap(first,count,at->first,at->count))) return 1;
    return 0;
}

/** @brief Query one real provider's extent while its inode is claimed.
 * @param node Borrowed held inode. @param offset Logical byte offset.
 * @param out Required result. @return Zero or negative errno.
 * Caller holds execution ownership and the filesystem/code lifetime.
 */
int swap_storage_file_extent(const struct storage_node *node,uint64_t offset,
                             struct swap_storage_extent *out)
{
    if (!node || !out) return -EINVAL;
    if (!storage_inode_swap_busy(node)) return -EBUSY;
    struct storage_volume *previous=NULL;int rc=storage_select_node_volume(node,&previous);
    if (!rc) {
        const struct storage_filesystem_ops *ops=g_storage.fs_ops;
        rc=ops && ops->swap_extent ? ops->swap_extent(&g_storage,node,offset,out) : -EOPNOTSUPP;
    }
    storage_restore_volume(previous);return rc;
}

/** @brief Convert only preallocated unwritten blocks during activation.
 * @param node Borrowed claimed inode. @param offset First logical byte.
 * @param length Complete block-aligned bytes. @return Zero or provider errno.
 * Caller holds execution and a storage transaction; later sync persists changes.
 */
int swap_storage_file_initialize(const struct storage_node *node,uint64_t offset,uint64_t length)
{
    if (!node || !length) return -EINVAL;
    if (!storage_inode_swap_busy(node)) return -EBUSY;
    struct storage_volume *previous=NULL;int rc=storage_select_node_volume(node,&previous);
    if (!rc) {
        const struct storage_filesystem_ops *ops=g_storage.fs_ops;
        rc=ops && ops->swap_initialize ? ops->swap_initialize(&g_storage,node,offset,length) : -EOPNOTSUPP;
    }
    storage_restore_volume(previous);return rc;
}

/** @brief Pin the exact device and partition backing a claimed native file.
 * @param node Borrowed claimed inode. @param out Required empty device token.
 * @param first Required first-sector output. @param count Required sector count.
 * @return Zero or negative errno. Success transfers a separate device/code
 * reference while the inode/path continue retaining filesystem ownership.
 */
int swap_storage_file_device(const struct storage_node *node,struct block_device_ref *out,
                             uint64_t *first,uint64_t *count)
{
    if (!node || !out || out->device || !first || !count) return -EINVAL;
    if (!storage_inode_swap_busy(node)) return -EBUSY;
    struct storage_volume *previous=NULL;int rc=storage_select_node_volume(node,&previous);
    if (!rc) {
        struct storage_volume *volume=&g_storage;struct block_device_info info;
        const struct storage_filesystem_ops *ops=volume->fs_ops;
        if (!ops || !ops->swap_extent || !ops->swap_initialize || !volume->device_ref.device)
            rc=-EOPNOTSUPP;
        else if ((rc=block_device_info(&volume->device_ref,&info))==0) {
            uint64_t start=volume->ext_start_lba,n=volume->ext_sector_count;
            if (info.block_size!=512 || !(info.flags&BLOCK_DEVICE_SWAP_SAFE) ||
                (info.flags&BLOCK_DEVICE_READ_ONLY)) rc=-EOPNOTSUPP;
            else if (!n || start>=info.blocks || n>info.blocks-start) rc=-EINVAL;
            else {
                rc=block_device_get(info.device,out);
                if (!rc) {*first=start;*count=n;}
            }
        }
    }
    storage_restore_volume(previous);return rc;
}
