/* SPDX-License-Identifier: Apache-2.0 */
#include "storage_backend.h"
#include "../../block/core/storage_internal.h"
#include <linux/errno.h>
#include <reliefnt/fs_type.h>

/** @brief Destroy an unowned initialized instance after native inode retirement.
 * @param volume Exclusive context without mount memberships; nullable.
 * @return Zero or negative busy/purge errno. Caller completed synchronous
 * flush before final mount destruction; failed purge retains context/code.
 * Execution ownership required, no graph/module lock spans provider cleanup.
 */
static int storage_volume_discard(struct storage_volume *volume)
{
    if(!volume || !volume->ready)return 0;
    if(volume->vfs_mounts)return -EBUSY;
    int rc=storage_inode_purge_volume(volume->volume_id);if(rc)return rc;
    if(g_active_volume==volume)g_active_volume=&g_volumes[STORAGE_VOLUME_ROOT];
    storage_destroy_volume(volume);
    *volume=(struct storage_volume){0};storage_cache_invalidate();return 0;
}

void storage_volume_mount_put(struct storage_volume *volume)
{
    if(!volume || !volume->vfs_mounts)return;
    if(--volume->vfs_mounts || !volume->vfs_managed)return;
    int rc=storage_volume_discard(volume);
    if(rc)console_error("[vfs] final backend cleanup retained volume=%u error=%d\n",volume->volume_id,rc);
}

/** @brief Construct a fresh owned builtin memory filesystem and attachment.
 * @param source Borrowed optional diagnostic label. @param flags Mount policy.
 * @param options Borrowed optional tmpfs limits/mode options.
 * @param uid New root owner UID. @param gid New root owner GID.
 * @param out Required owned detached mount output, empty on failure.
 * @return Zero or negative option/capacity/allocation/provider errno. Execution
 * ownership required; all failed construction/description state is retired.
 * The builtin memory implementation remains permanent for memfd/SysV/rootfs.
 */
int storage_new_tmpfs_mount(const char *source,uint64_t flags,const char *options,
                               uint32_t uid,uint32_t gid,struct vfs_mount **out)
{
    if(!out)return -EINVAL;*out=NULL;
    const uint64_t supported=MS_RDONLY|MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_SYNCHRONOUS|MS_DIRSYNC|
        MS_NOATIME|MS_NODIRATIME|MS_RELATIME|MS_STRICTATIME|MS_LAZYTIME;
    if(flags&~supported)return -EOPNOTSUPP;
    uint32_t slot=STORAGE_VOLUME_DYNAMIC_FIRST;
    while(slot<STORAGE_MAX_VOLUMES && g_volumes[slot].ready)++slot;
    if(slot==STORAGE_MAX_VOLUMES)return -ENOSPC;
    struct tmpfs_super *fs=NULL;int rc=tmpfs_new(slot,flags,options,uid,gid,&fs);if(rc)return rc;
    struct storage_volume *volume=&g_volumes[slot];
    *volume=(struct storage_volume){.volume_id=slot,.kind=STORAGE_VOLUME_ANONYMOUS,.ready=true,
        .filesystem=STORAGE_FILESYSTEM_TMPFS,.fs_ops=&storage_tmpfs_ops,.mount_flags=flags,.tmpfs=fs,.vfs_managed=true};
    rc=storage_create_vfs_mount(volume,NULL,out);
    if(!rc)rc=vfs_mount_describe(*out,source && source[0] ? source : "none","tmpfs",storage_volume_device_number(volume));
    if(rc) {
        vfs_mount_put(*out);*out=NULL;
        int cleanup=storage_volume_discard(volume);if(cleanup)return cleanup;
    }
    return rc;
}

/** @brief Construct/reuse ISO9660 from an independently registered optical device.
 * @param disk_id Stable flagged optical ID. @param filesystem Optional requested
 * type, iso9660 when present. @param flags Requested VFS attachment policy.
 * @param out Required empty detached mount output, remains empty on error.
 * @return Zero or type/lookup/read/allocation errno. Execution ownership; exact
 * device reference precedes parser I/O, each new instance independently pins
 * its filesystem and block owner. All failed constructors retire both references
 * without touching a preexisting mounted instance. No callback lock spans I/O.
 */
static int storage_new_optical_mount(uint32_t disk_id,const char *filesystem,uint64_t flags,struct vfs_mount **out)
{
    if(filesystem && __builtin_strcmp(filesystem,"iso9660"))return -EINVAL;
    struct block_device_ref ref={0};int rc=storage_disk_block_get(disk_id,&ref);if(rc)return rc;
    struct block_device_info info;rc=block_device_info(&ref,&info);
    if(!rc && (!(info.flags&BLOCK_DEVICE_OPTICAL) || info.block_size!=2048))rc=-ENODEV;
    struct storage_volume *volume=NULL;bool created=false;
    for(uint32_t i=0;!rc && i<STORAGE_MAX_VOLUMES;++i)
        if(g_volumes[i].ready && g_volumes[i].device_ref.device==ref.device &&
           g_volumes[i].filesystem==STORAGE_FILESYSTEM_ISO9660){volume=&g_volumes[i];break;}
    if(!rc && !volume) {
        uint32_t slot=STORAGE_VOLUME_DYNAMIC_FIRST;
        while(slot<STORAGE_MAX_VOLUMES && g_volumes[slot].ready)++slot;
        if(slot==STORAGE_MAX_VOLUMES)rc=-ENOSPC;
        else {
            volume=&g_volumes[slot];*volume=(struct storage_volume){.volume_id=slot,.mount_flags=flags|MS_RDONLY};
            struct install_disk_state disk={.present=true,.device=info.device};
            rc=storage_volume_from_install_disk(volume,&disk);
            if(!rc)rc=storage_mount_filesystem(volume,"iso9660");
            if(!rc) {
                volume->ready=true;volume->vfs_managed=true;created=true;
                volume->source_disk_id=disk_id;volume->source_partition_index=UINT32_MAX;
            } else {storage_destroy_volume(volume);*volume=(struct storage_volume){0};}
        }
    }
    if(!rc)rc=storage_create_vfs_mount(volume,NULL,out);
    if(!rc)rc=vfs_mount_set_flags(*out,(flags&(VFS_MOUNT_POLICY_MASK|MS_STRICTATIME))|MS_RDONLY);
    if(rc && *out){vfs_mount_put(*out);*out=NULL;}
    if(rc && created && !volume->vfs_mounts) {
        int cleanup=storage_volume_discard(volume);if(cleanup)rc=cleanup;
    }
    block_device_put(&ref);return rc;
}

/** @brief Attach an explicitly typed filesystem to a complete registered disk.
 * @param disk Stable inventory identity. @param filesystem Required provider name.
 * @param flags Validated attachment policy. @param out Required empty mount output.
 * @return Zero or input/busy/type/readonly/provider/allocation errno. Execution
 * ownership required; an exact device pin spans all probes. Existing dev_t/type
 * instances are reused with compatible readonly policy. Failed fresh instances
 * release every native/device reference without affecting another live mount.
 */
static int storage_new_whole_mount(uint32_t disk,const char *filesystem,uint64_t flags,struct vfs_mount **out)
{
    if(!filesystem || !filesystem[0])return -EINVAL;
    int rc=storage_acquire_task_io();if(rc)return rc;
    struct block_device_ref ref={0};rc=storage_disk_block_get(disk,&ref);if(rc)return rc;
    uint64_t device=storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(disk,-1));
    struct storage_volume *volume=NULL;bool created=false;
    for(uint32_t i=0;i<STORAGE_MAX_VOLUMES;++i) {
        struct storage_volume *candidate=&g_volumes[i];
        if(!candidate->ready || candidate->tmpfs || storage_volume_device_number(candidate)!=device)continue;
        if(!candidate->fs_type.type || __builtin_strcmp(candidate->fs_type.type->name,filesystem))rc=-EINVAL;
        else if((candidate->mount_flags^flags)&MS_RDONLY)rc=-EBUSY;
        else volume=candidate;
        break;
    }
    if(!rc && !volume) {
        int busy=storage_disk_block_busy(disk,-1);
        if(busy)rc=busy<0 ? busy : -EBUSY;
        uint32_t slot=STORAGE_VOLUME_DYNAMIC_FIRST;
        while(slot<STORAGE_MAX_VOLUMES && g_volumes[slot].ready)++slot;
        if(!rc && slot==STORAGE_MAX_VOLUMES)rc=-ENOSPC;
        if(!rc) {
            volume=&g_volumes[slot];*volume=(struct storage_volume){.volume_id=slot,.mount_flags=flags};
            rc=storage_mount_whole_filesystem(volume,disk,filesystem);
            if(rc)*volume=(struct storage_volume){0};
            else created=true;
        }
    }
    if(!rc)rc=storage_create_vfs_mount(volume,NULL,out);
    if(!rc)rc=vfs_mount_set_flags(*out,flags&(VFS_MOUNT_POLICY_MASK|MS_STRICTATIME));
    if(rc && *out){vfs_mount_put(*out);*out=NULL;}
    if(rc && created && !volume->vfs_mounts) {
        int cleanup=storage_volume_discard(volume);if(cleanup)rc=cleanup;
    }
    block_device_put(&ref);return rc;
}

/** @brief Attach a VFS membership to the single physical filesystem instance.
 * @param disk Registered disk identifier. @param partition GPT entry index,
 * UINT32_MAX for a whole device (requiring explicit filesystem unless optical).
 * @param filesystem Requested type, nullable for native detection.
 * @param flags Final supported mount policy without operation selectors.
 * @param out Required owned detached mount output, empty on error.
 * @return Zero or negative backend/allocation/policy errno. Reused backends
 * keep managed ownership; failures never discard another live instance. Each
 * returned mount owns its policy; inode/cache lifetime remains shared.
 * Execution ownership required; no graph/module lock spans backend I/O.
 */
int storage_new_block_mount(uint32_t disk,uint32_t partition,const char *filesystem,
                               uint64_t flags,struct vfs_mount **out)
{
    if(!out)return -EINVAL;*out=NULL;
    const uint64_t supported=MS_RDONLY|MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_SYNCHRONOUS|MS_DIRSYNC|
        MS_NOATIME|MS_NODIRATIME|MS_RELATIME|MS_STRICTATIME|MS_LAZYTIME;
    if(flags&~supported)return -EOPNOTSUPP;
    if(disk&STORAGE_BLOCK_OPTICAL_BIT)
        return partition==UINT32_MAX ? storage_new_optical_mount(disk,filesystem,flags,out) : -EINVAL;
    if(partition==UINT32_MAX)return storage_new_whole_mount(disk,filesystem,flags,out);
    uint32_t slot=0;bool created=false;
    int rc=storage_open_block_partition(disk,partition,filesystem,flags,&slot,&created);if(rc)return rc;
    struct storage_volume *volume=&g_volumes[slot];if(created)volume->vfs_managed=true;
    rc=storage_create_vfs_mount(volume,NULL,out);
    if(!rc)rc=vfs_mount_set_flags(*out,flags&(VFS_MOUNT_POLICY_MASK|MS_STRICTATIME));
    if(rc) {
        vfs_mount_put(*out);*out=NULL;
        /* A failed final finish already owns its membership in the retirement
         * queue. Only an unowned new instance needs direct rollback. */
        if(created && !volume->vfs_mounts) {
            int cleanup=storage_volume_discard(volume);if(cleanup)return cleanup;
        }
    }
    return rc;
}
