/* SPDX-License-Identifier: Apache-2.0 */
#include "storage_core_api.h"
#include <linux/errno.h>

/** @brief Name immutable inventory transport metadata without entering a driver.
 * @param transport Stored transport enum, not a provider identity.
 * @return Borrowed permanent diagnostic text; unknown metadata returns "block".
 * Pure observation, no locking, allocation, I/O or reference transfer.
 */
const char *storage_transport_name(uint8_t transport)
{
    if(transport==STORAGE_TRANSPORT_IDE_PIO)return "ide-pio";
    if(transport==STORAGE_TRANSPORT_NVME)return "nvme";
    if(transport==STORAGE_TRANSPORT_MEMORY)return "ram";
    if(transport==STORAGE_TRANSPORT_USB)return "usb";
    return transport==STORAGE_TRANSPORT_AHCI ? "ahci" : "block";
}

/** @brief Bind a fresh volume to the exact registered transport before first I/O.
 * @param volume Exclusive instance with an empty device token, required.
 * @param disk Borrowed scalar inventory record; never retains native pointers.
 * @return Zero or -EINVAL/-EBUSY/-ENODEV/owner errno. On failure no token is
 * retained. Execution ownership required; no hardware or module callback runs.
 * Each root/ESP/data instance owns an independent reference through final cache
 * retirement, rather than copying another volume's noncopyable ownership token.
 */
int storage_volume_from_install_disk(struct storage_volume *volume,const struct install_disk_state *disk)
{
    if(!volume || !disk || !disk->present)return -EINVAL;
    if(volume->device_ref.device)return -EBUSY;
    int rc=block_device_get(disk->device,&volume->device_ref);if(rc)return rc;
    struct block_device_info info;
    rc=block_device_info(&volume->device_ref,&info);
    if(rc){block_device_put(&volume->device_ref);return rc;}
    volume->transport=info.transport;
    volume->kind=info.transport==STORAGE_TRANSPORT_IDE_PIO ? STORAGE_VOLUME_IDE :
        info.transport==STORAGE_TRANSPORT_NVME ? STORAGE_VOLUME_NVME :
        info.transport==STORAGE_TRANSPORT_AHCI ? STORAGE_VOLUME_AHCI : STORAGE_VOLUME_BLOCK;
    volume->bus=info.bus;volume->slot=info.slot;volume->function=info.function;
    volume->port=info.port;volume->nvme_controller_index=info.controller;volume->nvme_nsid=info.namespace_id;
    storage_copy_text(volume->device_model,sizeof(volume->device_model),info.model);
    for(uint32_t i=0;i<g_install_disk_count;++i)if(disk==&g_install_disks[i])volume->source_disk_id=i;
    return 0;
}

/** @brief Drop a quiescent volume's exact block identity after native teardown.
 * @param volume Exclusive retired instance, NULL accepted; token is consumed.
 * Execution ownership required; caller already drained filesystem/cache users.
 * No provider code, hardware I/O or module destructor runs from this release.
 */
void storage_volume_put_device(struct storage_volume *volume)
{ if(volume)block_device_put(&volume->device_ref); }

/** @brief Validate an inventory device using registered immutable geometry.
 * @param disk Borrowed discovered scalar record, required.
 * @return Zero for an admitted 512-byte disk, -EOPNOTSUPP for incompatible
 * sector geometry, or lookup errno. Read-only disks permit metadata queries
 * and mounts; the block registry rejects every write with -EROFS. Task context;
 * temporary reference spans the check, no native callback or hardware reset.
 */
int storage_prepare_block_disk(struct install_disk_state *disk)
{
    if(!disk || !disk->present)return -EINVAL;
    struct block_device_ref ref={0};int rc=block_device_get(disk->device,&ref);
    if(rc)return rc;
    struct block_device_info info;rc=block_device_info(&ref,&info);
    if(!rc && info.block_size!=512u)rc=-EOPNOTSUPP;
    block_device_put(&ref);return rc;
}
