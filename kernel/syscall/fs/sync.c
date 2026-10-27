/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"

/** @brief Synchronize the actual admitted inode, superblock or mount tree.
 * @param ns Shared initialized namespace under execution ownership.
 * @param task Borrowed caller with dispatcher-owned original OFD admission.
 * @param number Linux SYNC, SYNCFS, FSYNC or FDATASYNC number.
 * @param fd Signed user descriptor; ignored by SYNC.
 * @return Zero or negative errno. SYNC attempts every mounted provider and
 * always returns zero per Linux/POSIX. Other calls propagate device/cache
 * errors. O_PATH is accepted only by SYNCFS. No text lookup or graph/module
 * spinlock spans provider I/O, and held paths keep callback code resident.
 */
int64_t syscall_sync_object(struct vfs_namespace *ns,struct task *task,uint64_t number,int32_t fd)
{
    if(number==LINUX_SYS_SYNC) {
        int rc=vfs_namespace_sync(ns);
        if(rc)console_error("[vfs] sync failed ret=%d\n",rc);
        return 0;
    }
    struct task_file *file=task_file_for_io(task,fd);
    if(!file)return -RELIEFOS_EBADF;
    bool filesystem=number==LINUX_SYS_SYNCFS;
    if(!filesystem && number!=LINUX_SYS_FSYNC && number!=LINUX_SYS_FDATASYNC)return -RELIEFOS_ENOSYS;
    if(!filesystem && (file->flags&TASK_FILE_FLAG_PATH))return -RELIEFOS_EBADF;
    if(!filesystem && (file->flags&TASK_FILE_FLAG_DEV_BLOCK))
        return storage_sync_disk(STORAGE_BLOCK_DISK_ID(file->node.volume_id));
    if(vfs_path_node(&file->path_ref))return vfs_path_sync(&file->path_ref,filesystem,number==LINUX_SYS_FDATASYNC);
    /* Anonymous pipes/socket/event descriptions belong to builtin pseudo
     * superblocks with no persistent state, but have no inode fsync method. */
    return filesystem ? 0 : -RELIEFOS_EINVAL;
}
