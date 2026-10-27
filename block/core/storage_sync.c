#include <reliefnt/export.h>
#include "storage_internal.h"
/** @brief Flush a held block identity after native writeback, retaining errors.
 * @param volume Stable instance retaining its device reference, required.
 * @return Zero for tmpfs without backing, or the registered transport's flush result.
 * Execution/storage exclusion supplied by caller; no optional driver imports
 * and no registry lock spans volatile-cache completion.
 */
static int storage_flush_volume_device(const struct storage_volume *volume)
{
    if(!volume)return -19;
    return volume->tmpfs && !volume->device_ref.device ? 0 : block_device_flush(&volume->device_ref);
}

/** @brief Flush a ready native instance through its owned transport.
 * @param volume Borrowed ready instance under storage execution exclusion.
 * @return Zero or ENODEV/transport errno; memory devices follow their registered
 * write-through/flush contract, while tmpfs has no block provider to flush.
 */
int storage_flush_volume(const struct storage_volume *volume)
{
    if (!volume || !volume->ready) return -19;
    return storage_flush_volume_device(volume);
}

/** @brief Flush the ext-family transport during recovery or normal writeback.
 * @param volume Borrowed instance with a live device token; ready may be false.
 * @return Zero or transport errno. Takes only the transport lock; no filesystem
 * registry lock spans the callback, and memory devices use the same interface.
 */
int storage_ext4_device_flush(const struct storage_volume *volume)
{
    /* Mount-time recovery runs before volume->ready is published. */
    kernel_spin_lock(&storage_transport_lock);
    int ret = storage_flush_volume_device(volume);
    kernel_spin_unlock(&storage_transport_lock);
    return ret;
}

/** @brief Settle one selected native filesystem before its device cache.
 * @param volume_id Existing volume slot; this is not a disk/path identity.
 * @return First writeback/transport error or zero; absent/not-ready is ENODEV.
 * Execution exclusion spans the operation, restoring the prior async admission
 * state. Provider callbacks run outside registry locks; no mutation is replayed.
 */
int storage_sync_volume(uint32_t volume_id)
{
    if (volume_id >= STORAGE_MAX_VOLUMES) return -19;
    struct storage_volume *volume = &g_volumes[volume_id];
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    bool saved_async = storage_io_async_context;
    storage_io_async_context = false;
    /* Native writeback precedes the transport lock and physical cache flush. */
    const struct storage_filesystem_ops *ops=volume->fs_ops;
    int ret=volume->ready && ops && ops->sync ? ops->sync(volume) : 0;
    int transport_ret;
    if (volume->tmpfs && !volume->device_ref.device) transport_ret = storage_flush_volume(volume);
    else {
        kernel_spin_lock(&storage_transport_lock);
        transport_ret = storage_flush_volume(volume);
        kernel_spin_unlock(&storage_transport_lock);
    }
    if (ret == 0) ret = transport_ret;
    storage_io_async_context = saved_async;
    kernel_execution_unlock_irqrestore(flags);
    return ret;
}

/** @brief Sync every ready native instance, retaining the first observed error.
 * @return Zero or first filesystem/transport errno. Holds execution exclusion,
 * delegates per-instance ordering and never stops after an earlier failure.
 */
int storage_sync_all(void)
{
    int result = 0;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    for (uint32_t i = 0; i < STORAGE_MAX_VOLUMES; ++i) {
        if (!g_volumes[i].ready) continue;
        int ret = storage_sync_volume(i);
        if (ret < 0 && !result) result = ret;
    }
    kernel_execution_unlock_irqrestore(flags);
    return result;
}

/* Native filesystem modules share this core-owned interface; CRC/release
 * checks bind it to the exact kernel, without a stable cross-release API. */
EXPORT_SYMBOL(storage_ext4_device_flush);
