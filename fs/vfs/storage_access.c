/* SPDX-License-Identifier: Apache-2.0 */
#include "storage_backend.h"
#include "kernel_path_internal.h"
#include <linux/mount.h>

/**
 * @brief Best-effort native disk access-time update for this exact attachment.
 * @param context Borrowed live native storage instance pinned by its mount.
 * @param node Borrowed actual retained regular/directory inode in that instance.
 * @param mount_flags Effective attachment and shared-superblock flags for this access.
 * @return None. Metadata failures never replace a completed read/readdir result.
 * Readonly/noatime/nodiratime suppress I/O; relatime uses mtime/ctime or a full
 * day and strict access uses each native codec's time precision. Existing tmpfs
 * uses its own inode policy; immutable ISO records are never changed.
 * Storage execution ownership is required. A synchronous nested I/O scope
 * preserves outer admission and mutation state; no graph/module callback lock
 * spans native metadata I/O and no path is reconstructed from a display name.
 */
void storage_backend_access(void *context,const struct storage_node *node,uint64_t mount_flags)
{
    struct storage_volume *volume=context;
    if(!volume || !node || node->volume_id!=volume->volume_id)return;
    bool clustered=(node->flags&(STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT))!=0;
    if(!clustered && !storage_filesystem_is_ext(volume))return;
    if(mount_flags&(MS_RDONLY|MS_NOATIME))return;
    if(node->type==RELIEFOS_FS_TYPE_DIR && (mount_flags&MS_NODIRATIME))return;
    struct reliefos_time_info clock;
    if(time_wall_clock(&clock) || clock.unix_seconds>INT64_MAX)return;
    int64_t now=(int64_t)clock.unix_seconds;
    struct vfs_kernel_io_scope scope=vfs_kernel_io_begin();
    struct linux_stat_abi status={0};int rc;
    if(clustered) {
        struct storage_inode_times times;rc=storage_cluster_node_times(node,&times);
        if(!rc){status.atime_sec=times.atime;status.mtime_sec=times.mtime;status.ctime_sec=times.ctime;}
    } else rc=storage_inode_stat(node,&status);
    if(!rc && status.atime_sec!=now &&
       (!(mount_flags&MS_RELATIME) || status.atime_sec<=status.mtime_sec ||
        status.atime_sec<=status.ctime_sec ||
        (now>=status.atime_sec && (uint64_t)now-(uint64_t)status.atime_sec>=86400)))
        (void)(clustered ? storage_cluster_touch_node(node,now) : storage_inode_utimensat(node,now,0,true,false));
    vfs_kernel_io_end(scope);
}
