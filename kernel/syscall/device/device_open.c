/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"
#include <reliefnt/devfs.h>

/** @brief Bind a newly created PTY description to its held device path.
 * @param task Current fd-table owner under execution ownership.
 * @param fd Newly installed PTY descriptor or a negative allocation error.
 * @param path Borrowed device path; one independent pin is retained on success.
 * @return Unchanged descriptor/error. A description retains its path through
 * dup/fork and final terminal close; no pathname lookup or backend I/O occurs.
 */
static int device_pty_path(struct task *task,int fd,const struct vfs_path *path)
{
    if(fd>=0) {
        struct task_pty_fd *entry=task_pty_fd_for_fd(task,fd);
        if(entry && entry->description)vfs_path_get(path,&entry->description->path_ref);
    }
    return fd;
}

/** @brief Open an already authorized character/block device by inode identity.
 * @param task Current task, borrowed under execution ownership.
 * @param path Borrowed held device path, retained before driver callbacks.
 * @param flags Sanitized status/descriptor flags; O_PATH is handled by caller.
 * @param diagnostic Optional display name, never used for device selection.
 * @return Descriptor or negative errno. PTY/DRM/audio ownership and failure
 * rollback use the existing device lifecycles; no namespace-prefix routing.
 * No mount/module registry spinlock spans any driver callback or wait.
 */
int task_open_device_path(struct task *task,const struct vfs_path *path,uint32_t flags,
                             const char *diagnostic)
{
    const struct storage_node *node=vfs_path_node(path);
    if(!node)return -RELIEFOS_EINVAL;
    if(vfs_path_mount_flags(path)&MS_NODEV)return -RELIEFOS_EACCES;
    struct storage_node device;bool native=(node->flags&STORAGE_NODE_FLAG_RDEV)!=0;
    if(native) {
        struct linux_stat_abi status;int rc=vfs_path_stat(path,&status);
        if(!rc)rc=devfs_lookup_rdev(status.st_mode&LINUX_S_IFMT,status.st_rdev,&device);
        if(rc)return rc;
        node=&device;
    }
    const char *name=vfs_path_name(path);
    char intrinsic[64];bool at_root=!vfs_path_backend_name(path,intrinsic,sizeof(intrinsic)) && intrinsic[0]=='/';
    if(at_root)for(size_t i=1;intrinsic[i];++i)if(intrinsic[i]=='/')at_root=false;
    if(!native && at_root && (!__builtin_strcmp(name,"stdin") || !__builtin_strcmp(name,"stdout") ||
                   !__builtin_strcmp(name,"stderr"))) {
        int source=!__builtin_strcmp(name,"stdin") ? 0 : !__builtin_strcmp(name,"stdout") ? 1 : 2;
        uint32_t cloexec=flags&LINUX_O_CLOEXEC ? RELIEFOS_FD_CLOEXEC : 0;
        if(task_file_for_fd(task,source))return task_duplicate_file_fd(task,source,0,cloexec);
        if(task_pty_stream_for_fd(task,source)>=0)return task_pty_duplicate_fd(task,source,0,cloexec);
        return -RELIEFOS_EBADF;
    }
    if(node->flags&STORAGE_NODE_FLAG_PTY) {
        int fd=task_pty_endpoint_fd(task,node->first_cluster,TASK_PTY_ENDPOINT_SLAVE,flags);
        if(fd>=0 && !(flags&LINUX_O_NOCTTY))
            pty_open_controlling(node->first_cluster,task->pid,(flags&LINUX_O_ACCMODE)!=LINUX_O_WRONLY);
        return device_pty_path(task,fd,path);
    }
    if(node->first_cluster==STORAGE_DEV_KIND_TTY && (native || (at_root && !__builtin_strcmp(name,"tty")))) {
        if(!task->controlling_pty_id)return -LINUX_ENXIO;
        return device_pty_path(task,task_pty_endpoint_fd(task,task->controlling_pty_id,TASK_PTY_ENDPOINT_SLAVE,flags),path);
    }
    if(node->first_cluster==STORAGE_DEV_KIND_PTMX) {
        int32_t id=pty_create(task->pid);if(id<0)return id;
        int fd=task_pty_endpoint_fd(task,(uint32_t)id,TASK_PTY_ENDPOINT_MASTER,flags);
        if(fd<0)(void)pty_destroy(task->pid,(uint32_t)id);
        return device_pty_path(task,fd,path);
    }
    if(node->first_cluster==STORAGE_DEV_KIND_DRM_CARD0 && (flags&LINUX_O_EXCL))return -LINUX_EBUSY;
    uint32_t kind=TASK_FILE_FLAG_DEV_NODE;
    if(node->first_cluster==STORAGE_DEV_KIND_NULL)kind|=TASK_FILE_FLAG_DEV_NULL;
    if(node->flags&STORAGE_NODE_FLAG_DEV_BLOCK) {
        if(flags&LINUX_O_EXCL) {
            int busy=storage_disk_block_busy(STORAGE_BLOCK_DISK_ID(node->volume_id),STORAGE_BLOCK_PARTITION(node->volume_id));
            if(busy)return busy<0 ? busy : -LINUX_EBUSY;
        }
        kind|=TASK_FILE_FLAG_DEV_BLOCK;
    }
    int fd=alloc_task_fd(task,node,flags|kind,diagnostic);if(fd<0)return fd;
    struct task_file *file=task_file_for_fd(task,fd);task_file_bind_path(file,path);
    int rc=0;
    if(node->flags&STORAGE_NODE_FLAG_DEV_BLOCK)
        rc=storage_disk_block_get(STORAGE_BLOCK_DISK_ID(node->volume_id),&file->block_ref);
    if(!rc && ((node->flags&(STORAGE_NODE_FLAG_AUDIO_PCM|STORAGE_NODE_FLAG_AUDIO_CONTROL|
                            STORAGE_NODE_FLAG_AUDIO_MIXER|STORAGE_NODE_FLAG_AUDIO_TIMER)) ||
               node->first_cluster==STORAGE_DEV_KIND_AUDIO ||
               node->first_cluster==STORAGE_DEV_KIND_DRM_CARD0 ||
               node->first_cluster==STORAGE_DEV_KIND_DRM_RENDERD128)) {
        /* Use the same devfs/native dev_t encoding exported by stat(2), not
         * the generated provider's undecorated generic inode attributes. */
        struct linux_stat_abi status;rc=linux_stat_from_path(&status,path);
        if(!rc)rc=character_file_open(status.st_rdev,task,file,&file->character_file);
    }
    if(!rc && node->first_cluster==STORAGE_DEV_KIND_SHM)rc=task_shm_attach(file,0);
    if(!rc && (node->first_cluster==STORAGE_DEV_KIND_KEYBOARD || node->first_cluster==STORAGE_DEV_KIND_MOUSE)) {
        file->aux=input_evdev_cursor_now();file->input_vt=task_evdev_vt_hint(task);
    }
    if(rc){task_discard_file_fd(task,fd);return rc;}
    return fd;
}
