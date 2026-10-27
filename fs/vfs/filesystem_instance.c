/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include "../operations.h"
#include <reliefnt/fs_type.h>
#include <reliefnt/export.h>

/** @brief Retire a fully quiesced native codec before dropping its code owner.
 * @param volume Exclusive instance, nullable; no mounts or external inodes.
 * @return None. Sync/purge already succeeded. Execution ownership required;
 * destruction is synchronous and no registry/module lock spans provider code.
 */
void storage_destroy_filesystem(struct storage_volume *volume)
{
    if(!volume)return;
    if(volume->fs_ops && volume->fs_ops->destroy)volume->fs_ops->destroy(volume);
    volume->fs_ops=NULL;fs_type_put(&volume->fs_type);
    volume->filesystem=STORAGE_FILESYSTEM_NONE;
    volume->vfs_managed=false;
}

/** @brief Retire a complete quiescent native instance and its device ownership.
 * @param volume Exclusive unpublished/unmounted instance, NULL accepted.
 * Caller settled sync/journal/inode users and holds execution ownership.
 * Native destruction runs before its filesystem and block code references
 * drop; no graph/module lock spans callbacks. Failed parser retries instead
 * use storage_destroy_filesystem, preserving the same underlying device pin.
 */
void storage_destroy_volume(struct storage_volume *volume)
{
    if(!volume)return;
    storage_destroy_filesystem(volume);storage_volume_put_device(volume);
}

/** @brief Acquire a registered provider before entering its native disk parser.
 * @param volume Exclusive empty instance with borrowed transport geometry.
 * @param name Required registered filesystem name.
 * @return Zero with one instance-owned type/code reference, otherwise negative
 * errno after codec cleanup. Execution ownership required across native I/O;
 * native selection is restored and no registry/module lock spans callbacks.
 */
int storage_mount_filesystem(struct storage_volume *volume,const char *name)
{
    if(!volume || !name)return -EINVAL;
    if(volume->ready || volume->fs_type.type || volume->fs_ops)return -EBUSY;
    struct fs_type_ref ref={0};int rc=fs_type_get(name,&ref);if(rc)return rc;
    const struct storage_filesystem_ops *ops=ref.type->storage;
    if(!ops || !ops->mount || !ops->root || !ops->lookup){fs_type_put(&ref);return -EOPNOTSUPP;}
    volume->fs_ops=ops;volume->fs_type=ref;
    struct storage_volume *previous=g_active_volume;g_active_volume=volume;
    rc=ops->mount(volume);
    if(rc)storage_destroy_filesystem(volume);
    else volume->vfs_managed=true;
    g_active_volume=previous;return rc;
}

/** @brief Build one whole-device filesystem with exact transport and type ownership.
 * @param volume Exclusive zeroed context with its volume_id/mount_flags set.
 * @param disk Registered inventory slot. @param name Required explicit filesystem.
 * @return Zero with a ready context, or validation/geometry/readonly/provider errno.
 * Execution ownership required through synchronous parsing. Every failure releases
 * partial codec/device state; no registry/graph lock spans provider code or I/O.
 */
int storage_mount_whole_filesystem(struct storage_volume *volume,uint32_t disk,const char *name)
{
    if(!volume || !name || !name[0] || volume->ready || volume->device_ref.device ||
       volume->fs_type.type || volume->fs_ops)return -EINVAL;
    if(disk>=g_install_disk_count || !g_install_disks[disk].present)return -ENODEV;
    int rc=storage_volume_from_install_disk(volume,&g_install_disks[disk]);
    struct block_device_info info;
    if(!rc)rc=block_device_info(&volume->device_ref,&info);
    if(!rc && info.block_size!=512)rc=-EOPNOTSUPP;
    if(!rc && (info.flags&BLOCK_DEVICE_READ_ONLY) && !(volume->mount_flags&MS_RDONLY))rc=-EROFS;
    if(!rc) {
        volume->source_disk_id=disk;volume->source_partition_index=UINT32_MAX;
        volume->data_partition_mount=true;
        volume->esp_sector_count=volume->exfat_sector_count=volume->ext_sector_count=info.blocks;
        volume->esp_device=volume->exfat_device=volume->ext_device=info.device;
        rc=storage_mount_filesystem(volume,name);
    }
    if(rc)storage_destroy_volume(volume);
    else volume->ready=true;
    return rc;
}

/** @brief Bind a filesystem factory to an already resolved standard block node.
 * @param name Registered provider name, required.
 * @param request Borrowed validated kernel operands with actual block source.
 * @param out Required owned detached mount output, cleared on failure.
 * @return Zero or negative type/unsupported/provider errno. Task context under
 * execution ownership; no pathname-to-device parsing or registry callback lock.
 */
int storage_block_get_tree(const char *name,const struct fs_mount_request *request,
                             struct vfs_mount **out)
{
    if(!out)return -EINVAL;*out=NULL;
    if(!name || !request || !request->source_node)return -EINVAL;
    if(request->options && request->options[0])return -EOPNOTSUPP;
    const struct storage_node *node=request->source_node;
    if(!(node->flags&STORAGE_NODE_FLAG_DEV_BLOCK))return -ENOTBLK;
    int32_t partition=STORAGE_BLOCK_PARTITION(node->volume_id);
    return storage_new_block_mount(STORAGE_BLOCK_DISK_ID(node->volume_id),(uint32_t)partition,
        name,request->flags,out);
}
EXPORT_SYMBOL(storage_mount_filesystem);
EXPORT_SYMBOL(storage_destroy_filesystem);
EXPORT_SYMBOL(storage_block_get_tree);
