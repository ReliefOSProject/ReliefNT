/* SPDX-License-Identifier: Apache-2.0 */
#include "../core/storage_internal.h"
#include <reliefnt/export.h>
#include <reliefnt/heap.h>
#include <linux/errno.h>

static uint32_t next_sd_name;

/** @brief Grow permanent scalar identity slots without copying native ownership.
 * @return Zero, -ENOMEM or -ENOSPC at the encoded node-ID namespace boundary.
 * Execution ownership; no record pointer may escape its transaction. Allocation
 * occurs outside registry/module locks, old metadata/cache pointers survive OOM
 * and every successful growth retires exactly the old core-owned heap array.
 */
static int storage_grow_inventory(void)
{
    if(g_install_disk_count<g_install_disk_capacity)return 0;
    if(g_install_disk_capacity>=STORAGE_BLOCK_OPTICAL_BIT)return -ENOSPC;
    uint32_t capacity=g_install_disk_capacity*2u;
    if(capacity>STORAGE_BLOCK_OPTICAL_BIT)capacity=STORAGE_BLOCK_OPTICAL_BIT;
    struct install_disk_state *grown=kernel_malloc((size_t)capacity*sizeof(*grown));
    if(!grown)return -ENOMEM;
    storage_memzero(grown,(size_t)capacity*sizeof(*grown));
    __builtin_memcpy(grown,g_install_disks,(size_t)g_install_disk_count*sizeof(*grown));
    struct install_disk_state *previous=g_install_disks;uint32_t old_capacity=g_install_disk_capacity;
    g_install_disks=grown;g_install_disk_capacity=capacity;
    if(old_capacity!=STORAGE_INSTALL_DISK_INITIAL_CAPACITY)kernel_free(previous);
    return 0;
}

/** @brief Refresh scalar disk inventory from independent admitted block devices.
 * Execution ownership required; no registration self-pins or driver callbacks.
 * Old slots/names are never reused or reordered after publication. Absence
 * clears only present, preserving stale-node identity without aliasing reload.
 * Optical devices are enumerated directly in their major-11 namespace.
 * @return Zero or first metadata-allocation/namespace errno; prior identities
 * remain intact on failure and temporary block references are always consumed.
 */
int storage_refresh_disks(void)
{
    int result=0;
    for(uint32_t i=0;i<g_install_disk_count;++i) {
        struct block_device_ref ref={0};
        g_install_disks[i].present=block_device_get(g_install_disks[i].device,&ref)==0;
        block_device_put(&ref);
        if(!g_install_disks[i].present) {
            kernel_free(g_install_disks[i].block_cache);g_install_disks[i].block_cache=NULL;
        }
    }
    uint64_t cursor=0;struct block_device_ref ref={0};
    while(!block_device_next(&cursor,&ref)) {
        struct block_device_info info;int rc=block_device_info(&ref,&info);
        if(!rc && !(info.flags&BLOCK_DEVICE_OPTICAL)) {
            uint32_t i=0;
            while(i<g_install_disk_count && g_install_disks[i].device!=info.device)++i;
            if(i==g_install_disk_count) {
                int grown=storage_grow_inventory();
                if(grown){if(!result)result=grown;block_device_put(&ref);continue;}
                struct install_disk_state *disk=&g_install_disks[g_install_disk_count++];
                *disk=(struct install_disk_state){.present=true,.device=info.device,
                    .transport=info.transport,.bus=info.bus,.slot=info.slot,.function=info.function,
                    .port=info.port,.nvme_controller_index=info.controller,.nvme_nsid=info.namespace_id,
                    .sector_count=info.blocks};
                if(info.transport==STORAGE_TRANSPORT_MEMORY)disk->name_index=info.controller;
                else if(info.transport!=STORAGE_TRANSPORT_NVME) {
                    bool known=false;
                    for(uint32_t old=0;old<i;++old) {
                        const struct install_disk_state *previous=&g_install_disks[old];
                        if(!previous->present && previous->transport==info.transport && previous->bus==info.bus &&
                           previous->slot==info.slot && previous->function==info.function && previous->port==info.port) {
                            disk->name_index=previous->name_index;known=true;break;
                        }
                    }
                    if(!known)disk->name_index=next_sd_name++;
                }
                storage_copy_text(disk->device_model,sizeof(disk->device_model),info.model);
            }
        }
        block_device_put(&ref);
    }
    return result;
}

/** @brief Hold the original block/code identity through an OFD or disk transaction.
 * @param disk_id Stable inventory slot, or flagged optical minor.
 * @param out Required empty owned block token, unchanged on failure.
 * @return Zero or -ENODEV/registry errno. Execution ownership; never callbacks
 * or I/O, and old slots retain their dev_t rather than resolving a replacement.
 */
int storage_disk_block_get(uint32_t disk_id,struct block_device_ref *out)
{
    int refreshed=storage_refresh_disks();
    uint64_t device=storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(disk_id,-1));
    return device ? block_device_get(device,out) : refreshed ? refreshed : -ENODEV;
}
EXPORT_SYMBOL(storage_disk_block_get);

/** @brief Snapshot scalar registered block identities for observation or installation.
 * @param disks Caller array, nullable only at zero capacity.
 * @param capacity Entries available; excess present devices are still counted.
 * @param out_count Required complete matching count, set after refresh succeeds.
 * @param installable Exclude ephemeral memory images from destructive target lists.
 * @return Zero or EINVAL/execution/inventory-allocation errno. Execution ownership
 * is acquired before refresh; no device callback, filesystem probe or I/O occurs.
 * All copied data is scalar; no inventory pointer or code ownership escapes.
 */
int storage_inventory_snapshot(struct reliefos_install_disk *disks,uint32_t capacity,
                                  uint32_t *out_count,bool installable)
{
    if(!out_count || (capacity && !disks))return -EINVAL;
    int rc=storage_acquire_task_io();if(rc)return rc;
    rc=storage_refresh_disks();if(rc)return rc;
    uint32_t count=0;
    for(uint32_t i=0;i<g_install_disk_count;++i) {
        const struct install_disk_state *disk=&g_install_disks[i];
        if(!disk->present || (installable && disk->transport==STORAGE_TRANSPORT_MEMORY))continue;
        if(count<capacity) {
            struct reliefos_install_disk *out=&disks[count];
            *out=(struct reliefos_install_disk){.id=i,.port=disk->transport==STORAGE_TRANSPORT_NVME ?
                disk->nvme_nsid : disk->port,.sector_size=512,.sector_count=disk->sector_count,
                .flags=disk->boot_root ? RELIEFOS_INSTALL_DISK_FLAG_BOOT_ROOT : 0};
            storage_copy_text(out->name,sizeof(out->name),disk->transport==STORAGE_TRANSPORT_MEMORY ? "Memory Disk" :
                disk->transport==STORAGE_TRANSPORT_IDE_PIO ? "IDE/PATA Disk" :
                disk->transport==STORAGE_TRANSPORT_NVME ? "NVMe Namespace" :
                disk->transport==STORAGE_TRANSPORT_AHCI ? "SATA/AHCI Disk" : "Block Disk");
        }
        ++count;
    }
    *out_count=count;return 0;
}

/** @brief Copy all present 512-byte disk identities, including reserved RAM media.
 * @param disks Caller array, nullable at zero capacity. @param capacity Entry capacity.
 * @param out_count Required full present count, including omitted entries.
 * @return Zero or snapshot errno. Execution-owned observation; no transport I/O
 * or ownership transfer. Installer target selection uses its filtering wrapper.
 */
int storage_list_block_disks(struct reliefos_install_disk *disks,uint32_t capacity,uint32_t *out_count)
{ return storage_inventory_snapshot(disks,capacity,out_count,false); }

/** @brief Enumerate independently admitted optical block identities.
 * @param cursor Required monotonic registration cursor, zero begins.
 * @param disk_id Required optical node ID output, valid when one is returned.
 * @return One entry, zero at end, or negative geometry/registry errno.
 * Execution ownership; snapshots scalar metadata then drops temporary code pin,
 * never reads media or requires an ISO9660 parser to exist.
 */
int storage_optical_next(uint64_t *cursor,uint32_t *disk_id)
{
    if(!cursor || !disk_id)return -22;
    struct block_device_ref ref={0};int rc;
    while(!(rc=block_device_next(cursor,&ref))) {
        struct block_device_info info;rc=block_device_info(&ref,&info);
        block_device_put(&ref);if(rc)return rc;
        if(!(info.flags&BLOCK_DEVICE_OPTICAL))continue;
        uint32_t major=(info.device>>8)&4095u;
        uint32_t minor=(info.device&255u)|((info.device>>12)&0xfffff00u);
        if(major!=11 || minor>=STORAGE_BLOCK_OPTICAL_BIT)return -75;
        *disk_id=STORAGE_BLOCK_OPTICAL_BIT|minor;return 1;
    }
    return rc==-2 ? 0 : rc;
}
