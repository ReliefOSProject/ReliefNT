#include "../../block/core/storage_internal.h"
/* Read-only mount listing for /proc/mounts and /etc/mtab. */
int storage_node_mount_flags(const struct storage_node *node, uint64_t *mount_flags)
{
    if (!node || !mount_flags) return -22;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    /* Reading mount policy must not acquire the unrelated asynchronous disk
     * transaction; mmap needs only the already pinned volume metadata. */
    const struct storage_volume *volume = node->volume_id == STORAGE_SHMEM_VOLUME &&
        (node->flags & STORAGE_NODE_FLAG_TMPFS) ? &g_shmem_volume :
        node->volume_id < STORAGE_MAX_VOLUMES ? &g_volumes[node->volume_id] : NULL;
    int ret = volume && volume->ready ? 0 : -19;
    if (!ret) *mount_flags = volume->mount_flags;
    kernel_execution_unlock_irqrestore(flags);
    return ret;
}

/**
 * @brief Read a mounted filesystem's Linux stat device number under the mount lock.
 * @param volume_id Mounted-volume slot, distinct from an encoded block device ID.
 * @return Linux dev_t matching mountinfo and stat, or zero for an unavailable slot.
 */
uint64_t storage_volume_device(uint32_t volume_id)
{
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    uint64_t device = volume_id < STORAGE_MAX_VOLUMES && g_volumes[volume_id].ready ?
        storage_volume_device_number(&g_volumes[volume_id]) : 0;
    kernel_execution_unlock_irqrestore(flags);
    return device;
}
