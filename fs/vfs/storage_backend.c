/* SPDX-License-Identifier: Apache-2.0 */
#include "storage_backend.h"
#include "../../block/core/storage_internal.h"
#include <linux/errno.h>
#include <reliefnt/module.h>

/* Adapters use explicit directory inodes and real backing pins. They do not
 * enter a global pathname router or consult a namespace prefix. */
/** @brief Invoke the selected native instance's one-component lookup.
 * @param context Borrowed pinned volume. @param parent Actual directory inode.
 * @param name Literal component. @param out Required metadata output.
 * @return Zero or negative input/provider errno. Caller owns execution; saves
 * and restores native selection without module/graph locks across I/O.
 */
int storage_backend_lookup(void *context,const struct storage_node *parent,
                               const char *name,struct storage_node *out)
{
    struct storage_volume *volume=context,*previous=NULL;
    if(!volume || !parent || parent->volume_id!=volume->volume_id || !out)return -EINVAL;
    if(parent->type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    int rc=storage_select_node_volume(parent,&previous);if(rc)return rc;
    rc=volume->fs_ops && volume->fs_ops->lookup ? volume->fs_ops->lookup(volume,parent,name,out) : -EOPNOTSUPP;
    storage_restore_volume(previous);return rc;
}

static int storage_vfs_retain(void *context,const struct storage_node *node,void **pin)
{
    struct storage_volume *volume=context;
    if(!pin || !volume || !node || node->volume_id!=volume->volume_id)return -EINVAL;
    struct storage_inode_ref *reference=NULL;int rc=storage_inode_get(node,&reference);
    *pin=reference;return rc;
}
static void storage_vfs_release(void *context,const struct storage_node *node,void *pin)
{
    (void)context;(void)node;(void)storage_inode_put(pin);
}
bool storage_backend_same_node(void *context,const struct storage_node *left,const struct storage_node *right)
{
    (void)context;
    return left->type==right->type && left->volume_id==right->volume_id &&
        ((left->flags & (STORAGE_NODE_FLAG_FAT32 | STORAGE_NODE_FLAG_EXFAT)) ||
         left->first_cluster==right->first_cluster) &&
        left->mount_generation==right->mount_generation && left->inode_generation==right->inode_generation;
}
/** @brief Invoke the selected provider's retained-symlink callback.
 * @param context Borrowed pinned volume. @param node Actual symlink inode.
 * @param buffer Required destination. @param capacity Available byte count.
 * @param length Required actual byte count output.
 * @return Zero or negative input/provider errno. Execution ownership required;
 * native selection is restored, no module/graph lock spans callback I/O.
 */
static int storage_vfs_readlink(void *context,const struct storage_node *node,
                                 char *buffer,size_t capacity,size_t *length)
{
    struct storage_volume *volume=context,*previous=NULL;
    if(!volume || !node || !length || !buffer || capacity>UINT32_MAX)return -EINVAL;
    *length=0;uint32_t got=0;
    int rc=storage_select_node_volume(node,&previous);if(rc)return rc;
    rc=volume->fs_ops && volume->fs_ops->readlink ?
        volume->fs_ops->readlink(volume,node,buffer,(uint32_t)capacity,&got) : -EOPNOTSUPP;
    storage_restore_volume(previous);if(!rc)*length=got;return rc;
}
/** @brief Read the retained volume inode with policy of this actual access.
 * @param context Borrowed live volume. @param node Retained actual inode.
 * @param offset Byte position. @param buffer Validated destination.
 * @param capacity Available bytes. @param read Required actual count output.
 * @param mount_flags Current attachment flags combined with shared readonly.
 * @return Zero or negative validation/provider errno. Execution ownership and
 * provider pin are required; no graph/module lock spans I/O. Each native instance consumes
 * per-access flags through its own callback; disk metadata is best-effort and cannot
 * replace completed bytes. Neither path mutates shared default flags.
 */
static int storage_vfs_read(void *context,const struct storage_node *node,uint64_t offset,
                              void *buffer,uint32_t capacity,uint32_t *read,uint64_t mount_flags)
{
    struct storage_volume *volume=context;
    if(!volume || node->volume_id!=volume->volume_id)return -EINVAL;
    int rc=storage_read_node_policy(node,offset,buffer,capacity,read,NULL,mount_flags);
    if(!rc && capacity)storage_backend_access(context,node,mount_flags);
    return rc;
}
/** @brief Read via the volume's inode cursor without losing attachment policy.
 * @param context Borrowed live volume. @param node Retained actual inode.
 * @param offset Byte position. @param buffer Validated destination.
 * @param capacity Available bytes. @param read Required actual count output.
 * @param cursor Optional OFD-owned optimization, invalidated for RAM inodes.
 * @param mount_flags Current attachment/shared flags, never stored globally.
 * @return Zero or negative validation/provider errno. Execution ownership and
 * provider pin required; no graph/module lock spans I/O or ownership transfer.
 * Successful disk reads mark access time without changing the transfer result.
 */
static int storage_vfs_read_cursor(void *context,const struct storage_node *node,uint64_t offset,
                                     void *buffer,uint32_t capacity,uint32_t *read,
                                     struct storage_read_cursor *cursor,uint64_t mount_flags)
{
    struct storage_volume *volume=context;
    if(!volume || node->volume_id!=volume->volume_id)return -EINVAL;
    int rc=storage_read_node_policy(node,offset,buffer,capacity,read,cursor,mount_flags);
    if(!rc && capacity)storage_backend_access(context,node,mount_flags);
    return rc;
}
/** @brief Enumerate one retained directory with its access-time policy.
 * @param context Borrowed live volume. @param node Retained actual directory.
 * @param cursor Required opaque cursor. @param entry Required entry output.
 * @param mount_flags Current attachment/shared flags; not global defaults.
 * @return One entry, zero EOF or negative validation/provider errno. Serialized
 * execution ownership required; no graph/module lock spans provider I/O.
 * Successful disk enumeration marks access time without replacing its result.
 */
static int storage_vfs_readdir(void *context,const struct storage_node *node,uint64_t *cursor,
                                 struct reliefos_dir_entry *entry,uint64_t mount_flags)
{
    struct storage_volume *volume=context;
    if(!volume || node->volume_id!=volume->volume_id)return -EINVAL;
    int rc=storage_readdir_node_policy(node,cursor,entry,mount_flags);
    if(rc>=0)storage_backend_access(context,node,mount_flags);
    return rc;
}
static uint64_t storage_vfs_flags(void *context)
{
    const struct storage_volume *volume=context;
    return volume->mount_flags|(((volume->fs_ops && (volume->fs_ops->flags&STORAGE_FS_READONLY)) ||
        volume->read_only_reason) ? MS_RDONLY : 0);
}
/** @brief Prepare shared backend policy while leaving attachment flags to VFS.
 * @param context Borrowed live volume under execution ownership.
 * @param flags Supported shared readonly/sync/dirsync/lazytime bits only.
 * @return Zero, unsupported flags, forced readonly or busy writers errno.
 * Failures preserve old state. Success updates shared defaults before VFS
 * publishes selected mount policy; no allocation/I/O or graph/module lock.
 */
static int storage_vfs_remount(void *context,uint64_t flags)
{
    struct storage_volume *volume=context;
    const uint64_t supported=VFS_SUPER_POLICY_MASK;
    if(flags&~supported)return -EOPNOTSUPP;
    if(!(flags&MS_RDONLY) && ((volume->fs_ops && (volume->fs_ops->flags&STORAGE_FS_READONLY)) || volume->read_only_reason))return -EROFS;
    if((flags&MS_RDONLY) && !(volume->mount_flags&MS_RDONLY) && storage_inode_volume_writers(volume->volume_id))return -EBUSY;
    volume->mount_flags=flags;if(volume->tmpfs)tmpfs_set_flags(volume->tmpfs,flags);return 0;
}
static int storage_vfs_can_unmount(void *context,void *pin)
{
    struct storage_volume *volume=context;
    return volume->vfs_managed && volume->vfs_mounts==1 &&
        storage_inode_volume_other_users(volume->volume_id,pin) ? -EBUSY : 0;
}
/** @brief Quiesce the final managed volume before its mount/code is destroyed.
 * @param context Borrowed live volume whose mount membership is still owned.
 * @return Zero when nonfinal/unmanaged or cache purge and synchronous flush
 * succeed; otherwise a negative error retaining volume/membership/provider.
 * The core has already released this mount's root. Execution ownership is
 * required; no graph/module lock spans I/O. storage_sync_volume saves/restores
 * async context, so no stack-backed DMA escapes a deferred finish attempt.
 */
static int storage_vfs_finish(void *context)
{
    struct storage_volume *volume=context;
    if(!volume->vfs_managed || volume->vfs_mounts>1)return 0;
    if(!volume->ready || volume->vfs_mounts!=1)return -EINVAL;
    int rc=storage_inode_purge_volume(volume->volume_id);
    return rc ? rc : storage_sync_volume(volume->volume_id);
}
static void storage_vfs_destroy(void *context)
{storage_volume_mount_put(context);}
static const struct vfs_backend_ops storage_vfs_ops={
    .lookup=storage_backend_lookup,.retain=storage_vfs_retain,.release=storage_vfs_release,
    .same_node=storage_backend_same_node,.readlink=storage_vfs_readlink,
    .finish=storage_vfs_finish,.destroy=storage_vfs_destroy,.can_unmount=storage_vfs_can_unmount,.remount=storage_vfs_remount,
    .read=storage_vfs_read,.read_cursor=storage_vfs_read_cursor,.readdir=storage_vfs_readdir,.permissions=storage_backend_permissions,
    .mount_flags=storage_vfs_flags,.create_node=storage_backend_create_node,
    .getattr=storage_backend_getattr,.statfs=storage_backend_statfs,
    .write=storage_backend_write,.truncate=storage_backend_truncate,.utimens=storage_backend_utimens,
    .fallocate=storage_backend_fallocate,.sync=storage_backend_sync,
    .remove_node=storage_backend_remove_node,.rename_node=storage_backend_rename_node,.link_node=storage_backend_link_node
};

/** @brief Capture diagnostic identity from the selected native implementation.
 * @param mount Owned detached attachment. @param volume Borrowed pinned instance.
 * @return Zero or negative description/allocation errno. Execution ownership
 * required, no pathname routing; copied source/type labels own no provider code.
 */
static int storage_vfs_describe(struct vfs_mount *mount,const struct storage_volume *volume)
{
    const char *filesystem=volume->filesystem==STORAGE_FILESYSTEM_EXT2 ? "ext2" :
        volume->fs_ops ? volume->fs_ops->name : NULL;
    if(!filesystem)return -EOPNOTSUPP;
    char source[RELIEFOS_FS_PATH_LEN];
    if(volume->tmpfs)storage_copy_text(source,sizeof(source),volume->tmpfs_source[0] ? volume->tmpfs_source : "tmpfs");
    else if(volume->transport==STORAGE_TRANSPORT_MEMORY)storage_copy_text(source,sizeof(source),"ramdisk");
    else if(volume->filesystem==STORAGE_FILESYSTEM_ISO9660) {
        int rc=storage_disk_device_name(volume->source_disk_id,-1,source,sizeof(source));if(rc)return rc;
    } else if(volume->data_partition_mount) {
        int rc=storage_disk_device_name(volume->source_disk_id,volume->source_partition_index,source,sizeof(source));
        if(rc)return rc;
    } else {
        const uint8_t *guid=storage_filesystem_is_ext(volume) ? volume->ext2_unique_guid :
            volume->filesystem==STORAGE_FILESYSTEM_EXFAT ? volume->exfat_unique_guid : volume->esp_unique_guid;
        if(volume->transport && volume->filesystem!=STORAGE_FILESYSTEM_ISO9660 && storage_guid_valid(guid)) {
            storage_copy_text(source,sizeof(source),"/dev/disk/by-partuuid/");
            storage_partition_guid_text(guid,source+22);
        } else storage_copy_text(source,sizeof(source),"rootfs");
    }
    return vfs_mount_describe(mount,source,filesystem,storage_volume_device_number(volume));
}

/** @brief Pin an initialized native instance and construct its root attachment.
 * @param volume Borrowed ready instance with a persistent operations table.
 * @param owner Optional explicit admission owner; otherwise selected type/table owner.
 * @param out Required owned mount output, empty on failure.
 * @return Zero or negative admission/provider/allocation errno. Execution
 * ownership required; admission precedes the first callback. The resulting
 * mount pins its owner independently of the instance's constructor reference;
 * failed final cleanup transfers both to the normal retirement queue.
 */
int storage_create_vfs_mount(struct storage_volume *volume,struct module *owner,
                               struct vfs_mount **out)
{
    if(!out)return -EINVAL;*out=NULL;
    if(!volume || !volume->ready)return -ENODEV;
    if(volume->vfs_mounts==UINT64_MAX)return -EOVERFLOW;
    /* Pin before the first provider callback; vfs_mount_new transfers its own
     * independent pin to the resulting mount, then this admission pin drops. */
    if(!owner && volume->fs_type.type)owner=volume->fs_type.type->owner;
    if(!owner && volume->fs_ops)owner=volume->fs_ops->owner;
    if(!try_module_get(owner))return -ENODEV;
    struct storage_node root;
    int rc=volume->fs_ops && volume->fs_ops->root ? volume->fs_ops->root(volume,&root) : -EOPNOTSUPP;
    if(!rc)rc=vfs_mount_new(&storage_vfs_ops,volume,owner,&root,out);
    if(!rc) {
        ++volume->vfs_mounts;
        rc=storage_vfs_describe(*out,volume);
        if(rc){vfs_mount_put(*out);*out=NULL;}
    }
    module_put(owner);return rc;
}
