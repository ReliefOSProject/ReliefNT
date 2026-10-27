/* SPDX-License-Identifier: Apache-2.0 */
#include "storage_internal.h"
#include <reliefnt/export.h>

/** @brief Compute the same block or anonymous device identity for stat/mountinfo.
 * @param volume Required stable native instance, borrowed under execution ownership.
 * @return Registered block dev_t when known, otherwise volume_id+1 anonymous
 * identity. Pure geometry/state query, no I/O, allocation or reference transfer.
 */
uint64_t storage_volume_device_number(const struct storage_volume *volume)
{
    uint64_t device = 0;
    /* ISO may live on a registered memory/loop-style block device as well as
     * an optical drive. Its held whole-device identity is authoritative. */
    if(volume->filesystem==STORAGE_FILESYSTEM_ISO9660 && volume->device_ref.device) {
        struct block_device_info info;
        return block_device_info(&volume->device_ref,&info) ? 0 : info.device;
    }
    if(volume->filesystem==STORAGE_FILESYSTEM_ISO9660 &&
       (volume->source_disk_id&STORAGE_BLOCK_OPTICAL_BIT))
        return storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(volume->source_disk_id,-1));
    if (!volume->tmpfs) {
        if (volume->data_partition_mount)
            device = storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(
                volume->source_disk_id, volume->source_partition_index));
        else if (volume->filesystem == STORAGE_FILESYSTEM_FAT32) device = volume->esp_device;
        else if (volume->filesystem == STORAGE_FILESYSTEM_EXT2 ||
                 volume->filesystem == STORAGE_FILESYSTEM_EXT4) device = volume->ext_device;
        else if (volume->filesystem == STORAGE_FILESYSTEM_EXFAT) device = volume->exfat_device;
    }
    return device ? device : (uint64_t)volume->volume_id + 1;
}

EXPORT_SYMBOL(storage_volume_device_number);

/** @brief Resolve a stable node/inventory identity without entering native code.
 * @param value Encoded inventory slot/partition, or optical-minor identity.
 * @return Linux dev_t or zero when no identity exists. Caller supplies execution
 * ownership; scalar records persist after removal so stale nodes never relabel.
 */
uint64_t storage_block_rdev(uint32_t value)
{
    uint32_t disk=STORAGE_BLOCK_DISK_ID(value);int32_t part=STORAGE_BLOCK_PARTITION(value);
    if(disk&STORAGE_BLOCK_OPTICAL_BIT)
        return part<0 ? block_device_make_number(11,disk&~STORAGE_BLOCK_OPTICAL_BIT) : 0;
    if(disk>=g_install_disk_count)return 0;
    uint64_t device=g_install_disks[disk].device;
    uint32_t major=(device>>8)&4095u,minor=(device&255u)|((device>>12)&0xfffff00u);
    return device ? block_device_make_number(major,minor+(uint32_t)(part+1)) : 0;
}
EXPORT_SYMBOL(storage_block_rdev);
