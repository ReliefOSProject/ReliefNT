/* SPDX-License-Identifier: Apache-2.0 */
#include "../core/storage_core_api.h"

/** @brief Snapshot actual disk capacity from an admitted immutable block provider.
 * @param disk Borrowed scalar inventory record. @param out_sectors Required
 * writable capacity output, valid only on success.
 * @return Zero or -EINVAL/lookup/geometry errno. Temporary exact device/code
 * ownership spans the check; no reset, IDENTIFY or optional callback is needed.
 * Caller holds execution ownership and any outer disk transaction lifetime pin.
 */
int storage_install_identify(struct install_disk_state *disk,uint64_t *out_sectors)
{
    if(!disk || !out_sectors)return -22;
    struct block_device_ref ref={0};int rc=block_device_get(disk->device,&ref);
    if(rc)return rc;
    struct block_device_info info;rc=block_device_info(&ref,&info);
    if(!rc && info.block_size!=512)rc=-95;
    if(!rc){disk->sector_count=info.blocks;*out_sectors=info.blocks;}
    block_device_put(&ref);return rc;
}
