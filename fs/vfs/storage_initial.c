/* SPDX-License-Identifier: Apache-2.0 */
#include "storage_backend.h"
#include "internal.h"
#include "../../block/core/storage_internal.h"
#include <reliefnt/devfs.h>
#include <reliefnt/procfs.h>
#include <reliefnt/inventory.h>
#include <linux/errno.h>

/* Initial attachments are explicit boot roles in the same actual namespace:
 * root, generated providers, the boot ESP, numbered optical media and shm.
 * Filesystem instances never supply a target pathname. Registered provider
 * factories acquire code ownership before constructing an attachment. */
static int storage_initial_volume(void *context,struct vfs_mount **out)
{return storage_create_vfs_mount(context,NULL,out);}
static int storage_initial_devfs(void *context,struct vfs_mount **out)
{(void)context;return devfs_create_vfs_mount(out);}
static int storage_initial_procfs(void *context,struct vfs_mount **out)
{(void)context;return proc_create_vfs_mount(out);}
static int storage_initial_sysfs(void *context,struct vfs_mount **out)
{(void)context;return sysfs_create_vfs_mount(out);}

/** @brief Allocate one fresh builtin boot-shm filesystem without a mount label.
 * @param created Required empty output receiving the owned ready volume slot.
 * @return Zero or negative allocation/quota/slot errno. Caller attaches this
 * exact context at /dev/shm or destroys it after failed assembly; it never
 * guesses identity by scanning legacy path labels. Exclusive boot execution
 * required, no graph/module lock spans allocation; no ownership on failure.
 */
static int storage_initial_shm(struct storage_volume **created)
{
    *created=NULL;
    uint32_t slot=STORAGE_VOLUME_DYNAMIC_FIRST;
    while(slot<STORAGE_MAX_VOLUMES && g_volumes[slot].ready)++slot;
    if(slot==STORAGE_MAX_VOLUMES)return -ENOSPC;
    struct tmpfs_super *fs=NULL;
    int rc=tmpfs_new(slot,MS_NOSUID|MS_NODEV,"size=0,nr_inodes=0,mode=1777",0,0,&fs);
    if(rc)return rc;
    struct storage_volume *volume=&g_volumes[slot];
    *volume=(struct storage_volume){.volume_id=slot,.kind=STORAGE_VOLUME_ANONYMOUS,.ready=true,
        .filesystem=STORAGE_FILESYSTEM_TMPFS,.fs_ops=&storage_tmpfs_ops,.mount_flags=MS_NOSUID|MS_NODEV,.tmpfs=fs};
    storage_copy_text(volume->tmpfs_source,sizeof(volume->tmpfs_source),"tmpfs");
    *created=volume;return 0;
}

/** @brief Assemble the one initial shared namespace before kernel path users.
 * @return Zero after root/providers/boot attachments and explicit fresh shm
 * are all published, or negative errno after complete assembly rollback. A
 * failed constructor/attachment never publishes a partial system namespace.
 * The caller owns exclusive boot execution; no graph/module lock spans I/O.
 * Reserved ESP identity and ready optical instances define initial roles;
 * no stored mount-path label participates in selection or routing.
 */
int storage_namespace_bootstrap(void)
{
    if(vfs_system_namespace())return -EBUSY;
    if(!g_volumes[0].ready)return -ENODEV;
    struct storage_volume *created=NULL;int rc=storage_initial_shm(&created);if(rc)return rc;
    struct vfs_boot_mount mounts[STORAGE_MAX_VOLUMES+3]={
        {"/dev",storage_initial_devfs,NULL,false},
        {"/proc",storage_initial_procfs,NULL,false},
        {"/sys",storage_initial_sysfs,NULL,false}
    };
    size_t count=3;uint32_t optical_count=0;
    char optical_targets[STORAGE_MAX_VOLUMES][32];
    if(g_volumes[STORAGE_VOLUME_BOOT].ready)
        mounts[count++]=(struct vfs_boot_mount){"/boot",storage_initial_volume,&g_volumes[STORAGE_VOLUME_BOOT],false};
    for(uint32_t i=STORAGE_VOLUME_DYNAMIC_FIRST;i<STORAGE_MAX_VOLUMES;++i) {
        if(!g_volumes[i].ready || g_volumes[i].filesystem!=STORAGE_FILESYSTEM_ISO9660)continue;
        storage_format_u32(optical_targets[optical_count],sizeof(optical_targets[0]),"/media/cdrom",optical_count,-1);
        mounts[count++]=(struct vfs_boot_mount){optical_targets[optical_count],storage_initial_volume,&g_volumes[i],true};
        ++optical_count;
    }
    mounts[count++]=(struct vfs_boot_mount){"/dev/shm",storage_initial_volume,created,false};
    rc=vfs_system_bootstrap(storage_initial_volume,&g_volumes[0],mounts,count);
    if(rc && created) {
        /* Assembly retired every owned path before returning an error. Refuse
         * destructive cleanup if a provider violated that lifetime contract. */
        if(storage_inode_volume_busy(created->volume_id))return -EBUSY;
        tmpfs_destroy(created->tmpfs);*created=(struct storage_volume){0};
        storage_cache_invalidate();
    }
    return rc;
}
