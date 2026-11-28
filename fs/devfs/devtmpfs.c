/* SPDX-License-Identifier: Apache-2.0 */
#include "devtmpfs_internal.h"
#include "../vfs/storage_backend.h"
#include "../vfs/mount_internal.h"
#include "../../block/core/storage_internal.h"
#include <reliefnt/devfs.h>
#include <reliefnt/deferred.h>
#include <linux/errno.h>
#include <linux/mount.h>

static struct vfs_mount *devtmpfs_backing;
static struct devtmpfs_node *devtmpfs_managed;
static uint64_t devtmpfs_generation=1,devtmpfs_processed;
static bool devtmpfs_ready;

/** @brief Coalesce device inventory changes without scanning or allocating.
 * IRQ/task safe. Enumeration runs later under VFS execution ownership.
 */
void devtmpfs_changed(void)
{
    __atomic_add_fetch(&devtmpfs_generation,1,__ATOMIC_RELEASE);
    if(__atomic_load_n(&devtmpfs_ready,__ATOMIC_ACQUIRE))
        kernel_deferred_schedule(DEFERRED_VFS);
}

/** @brief Report coalesced inventory work after the first devtmpfs mount.
 * @return True when a live backing filesystem needs reconciliation.
 */
bool devtmpfs_pending(void)
{
    return __atomic_load_n(&devtmpfs_ready,__ATOMIC_ACQUIRE) &&
        devtmpfs_processed!=__atomic_load_n(&devtmpfs_generation,__ATOMIC_ACQUIRE);
}

/** @brief Refresh kernel-managed nodes while preserving user-owned entries.
 * @return Zero or negative errno. Execution ownership required. An error or an
 * event arriving during enumeration leaves work pending for a later retry.
 */
int devtmpfs_refresh(void)
{
    if(!devtmpfs_pending())return 0;
    uint64_t generation=__atomic_load_n(&devtmpfs_generation,__ATOMIC_ACQUIRE);
    struct devtmpfs_node *snapshot=NULL;
    int rc=devtmpfs_snapshot(&snapshot);
    if(rc)return rc;
    struct storage_volume *volume=devtmpfs_backing->context;
    /* Kernel device publication is independent of a userspace readonly mount.
     * No task can observe the temporary backend flag under execution ownership.
     */
    tmpfs_set_flags(volume->tmpfs,volume->mount_flags&~MS_RDONLY);
    rc=devtmpfs_reconcile(volume->tmpfs,&devtmpfs_managed,snapshot);
    tmpfs_set_flags(volume->tmpfs,volume->mount_flags);
    if(!rc)devtmpfs_processed=generation;
    return rc;
}

/** @brief Attach the permanent Linux-style shared device memory filesystem.
 * @param source Optional mount label. @param flags Linux mount flags.
 * @param options Optional tmpfs mount data. @param out Owned detached mount.
 * @return Zero or negative errno. Execution ownership serializes construction
 * and quota/policy changes. The private reference survives all public unmounts.
 */
int devtmpfs_create_vfs_mount(const char *source,uint64_t flags,const char *options,
                              struct vfs_mount **out)
{
    if(!out)return -EINVAL;
    *out=NULL;
    if(flags&~(VFS_MOUNT_POLICY_MASK|VFS_SUPER_POLICY_MASK|MS_STRICTATIME))return -EOPNOTSUPP;
    if(!devtmpfs_backing) {
        int rc=storage_new_tmpfs_mount("devtmpfs",0,"mode=0755,size=0,nr_inodes=0",0,0,&devtmpfs_backing);
        if(rc)return rc;
        __atomic_store_n(&devtmpfs_ready,true,__ATOMIC_RELEASE);
    }
    struct storage_volume *volume=devtmpfs_backing->context;
    int rc=tmpfs_reconfigure(volume->tmpfs,options,false);
    if(rc)return rc;
    rc=devtmpfs_refresh();
    if(rc)return rc;
    rc=storage_create_vfs_mount(volume,NULL,out);
    if(!rc)rc=vfs_mount_describe(*out,source && source[0] ? source : "devtmpfs",
                                 "devtmpfs",storage_volume_device_number(volume));
    if(!rc)rc=devtmpfs_backing->ops->remount_options(volume,flags&VFS_SUPER_POLICY_MASK,options);
    if(rc) {
        vfs_mount_put(*out);
        *out=NULL;
    }
    return rc;
}
