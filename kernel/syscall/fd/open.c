/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"

/** @brief Apply open-time truncation with inode-wide writer admission.
 * @param file Newly authorized canonical description with an owned provider path.
 * @return Zero or negative errno; a temporary lease is always released and
 * the requested access flags are never changed. Anonymous shmem uses its seal
 * checks, including shrink denial. Generated inodes delegate zero-size
 * truncation to their retained provider. No pathname replay or graph lock across I/O.
 */
static int open_truncate_inode(struct task_file *file)
{
    if(!file->inode)return vfs_path_truncate(&file->path_ref,0);
    bool temporary=!file->inode_write_access;
    int rc=temporary ? storage_inode_write_get(file->inode) : 0;if(rc)return rc;
    rc=vfs_path_truncate(&file->path_ref,0);
    if(!rc)file->node=*vfs_path_node(&file->path_ref);
    if(temporary)storage_inode_write_put(file->inode);
    return rc;
}

/** @brief Publish an authorized inode open with its mount/provider lifetime.
 * @param task Current descriptor owner, borrowed under execution ownership.
 * @param path Held regular/directory/symlink path, borrowed until fd binding.
 * @param flags Sanitized native flags, including private O_PATH translation.
 * @param diagnostic Optional display name; never used for inode I/O.
 * @return Descriptor or negative errno with complete rollback. Writer admission
 * precedes truncation, including the temporary lease for O_RDONLY|O_TRUNC.
 * Optional provider capture belongs to this canonical OFD and rolls back on
 * every failure. O_PATH never captures, truncates or acquires a writer lease.
 * No graph/module lock spans provider callbacks or I/O.
 */
static int open_inode_path(struct task *task,const struct vfs_path *path,uint32_t flags,
                              const char *diagnostic)
{
    const struct storage_node *node=vfs_path_node(path);
    int fd;
    if(node->flags&STORAGE_NODE_FLAG_MEMFD) {
        struct task_file *file=NULL;fd=task_allocate_fd(task,0,&file);if(fd<0)return fd;
        file->flags=flags;file->fd_flags=flags&LINUX_O_CLOEXEC ? RELIEFOS_FD_CLOEXEC : 0;
        int rc=task_shm_reopen(file,path);
        if(!rc && !(flags&TASK_FILE_FLAG_PATH) && (flags&LINUX_O_TRUNC))rc=open_truncate_inode(file);
        if(rc){task_discard_file_fd(task,fd);return rc;}
        return fd;
    }
    fd=alloc_task_fd(task,node,flags,diagnostic);if(fd<0)return fd;
    struct task_file *file=task_file_for_fd(task,fd);task_file_bind_path(file,path);
    if(!(flags&TASK_FILE_FLAG_PATH)) {
        int rc=vfs_file_open(path,flags,&file->provider_file);
        if(rc){task_discard_file_fd(task,fd);return rc;}
    }
    if(!(flags&TASK_FILE_FLAG_PATH) && node->type==RELIEFOS_FS_TYPE_FILE && (flags&LINUX_O_TRUNC)) {
        int rc=open_truncate_inode(file);
        if(rc){task_discard_file_fd(task,fd);return rc;}
    }
    return fd;
}

/** @brief Open one user pathname through retained task/mount/inode objects.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Current task and descriptor table, required.
 * @param dirfd Relative directory fd or AT_FDCWD; absolute input ignores it.
 * @param pathname User text, copied once by the syscall lookup scope.
 * @param flags User open/openat flags; unknown bits are ignored as on Linux.
 * @param mode Permission bits used only when creating a regular file.
 * @return Descriptor, negative errno or FIFO blocking sentinel. Existing
 * O_CREAT files keep their metadata; dangling links create at their actual
 * target, O_EXCL never follows the final link, and O_PATH avoids inode/device
 * open side effects. Every published OFD retains its mount before final puts.
 */
int64_t syscall_open_object(struct vfs_namespace *ns,struct task *task,int32_t dirfd,
                               uint64_t pathname,uint32_t flags,uint32_t mode)
{
    if(!task)return -RELIEFOS_ESRCH;
    if(task->fifo_open_file)return task_fifo_open(task,NULL,0,NULL);
    flags&=LINUX_O_ACCMODE|LINUX_O_CREAT|LINUX_O_EXCL|LINUX_O_NOCTTY|LINUX_O_TRUNC|
        LINUX_O_APPEND|LINUX_O_NONBLOCK|LINUX_O_DSYNC|LINUX_O_ASYNC|LINUX_O_DIRECT|
        LINUX_O_LARGEFILE|LINUX_O_DIRECTORY|LINUX_O_NOFOLLOW|LINUX_O_NOATIME|
        LINUX_O_CLOEXEC|LINUX_O_SYNC|LINUX_O_PATH|LINUX_O_TMPFILE;
    if(flags&LINUX_O_PATH)flags&=LINUX_O_PATH|LINUX_O_DIRECTORY|LINUX_O_NOFOLLOW|LINUX_O_CLOEXEC;
    else {
        if(flags&(LINUX_O_TMPFILE&~LINUX_O_DIRECTORY))return -RELIEFOS_EOPNOTSUPP;
        if((flags&(LINUX_O_CREAT|LINUX_O_DIRECTORY))==(LINUX_O_CREAT|LINUX_O_DIRECTORY))return -RELIEFOS_EINVAL;
    }
    flags=(flags&~(uint32_t)(LINUX_O_PATH|TASK_FILE_FLAG_PATH))|
        (flags&LINUX_O_PATH ? TASK_FILE_FLAG_PATH : 0);
    bool exclusive=(flags&(LINUX_O_CREAT|LINUX_O_EXCL))==(LINUX_O_CREAT|LINUX_O_EXCL);
    unsigned lookup=flags&LINUX_O_NOFOLLOW || exclusive ? 0 : VFS_LOOKUP_FOLLOW;
    if(flags&LINUX_O_CREAT)lookup|=VFS_LOOKUP_MISSING;
    struct vfs_lookup_result found={0};
    int rc=resolve_user_object_at_flags(ns,task,dirfd,pathname,false,false,lookup,&found);
    if(rc)return rc;
    bool created=false;
    if(found.missing) {
        struct reliefos_permissions value;
        if(found.trailing)rc=-RELIEFOS_EISDIR;
        else rc=syscall_creation_mode(task,&found.path,LINUX_S_IFREG,mode,&value);
        if(!rc) {
            struct vfs_path file={0};rc=vfs_path_new_file(&found.path,found.last,&value,&file);
            if(!rc){vfs_path_put(&found.path);found.path=file;created=true;}
        }
    } else if(exclusive)rc=-RELIEFOS_EEXIST;
    const struct storage_node *node=vfs_path_node(&found.path);
    if(!rc && (flags&LINUX_O_DIRECTORY) && node->type!=RELIEFOS_FS_TYPE_DIR)rc=-RELIEFOS_ENOTDIR;
    if(!rc && !(flags&TASK_FILE_FLAG_PATH) && node->type==RELIEFOS_FS_TYPE_SYMLINK)rc=-LINUX_ELOOP;
    if(!rc && !(flags&TASK_FILE_FLAG_PATH)) {
        if(node->type==RELIEFOS_FS_TYPE_DIR && (flags&LINUX_O_ACCMODE)!=LINUX_O_RDONLY)rc=-RELIEFOS_EISDIR;
        uint32_t access=(flags&LINUX_O_ACCMODE)==LINUX_O_RDONLY ? FS_ACCESS_READ :
            (flags&LINUX_O_ACCMODE)==LINUX_O_WRONLY ? FS_ACCESS_WRITE : FS_ACCESS_READ|FS_ACCESS_WRITE;
        if(flags&LINUX_O_TRUNC)access|=FS_ACCESS_WRITE;
        if(!rc && !created)rc=fs_permissions_check_path(task,&found.path,access,false);
        struct reliefos_permissions value;
        if(!rc)rc=vfs_path_permissions(&found.path,&value,false);
        if(!rc && (flags&LINUX_O_NOATIME) && task->fsuid!=value.uid &&
           !(task->cap_effective&(1ULL<<CAP_FOWNER)))rc=-RELIEFOS_EPERM;
        if(!rc && (node->type==RELIEFOS_FS_TYPE_SOCKET || (value.mode&LINUX_S_IFMT)==LINUX_S_IFSOCK))rc=-LINUX_ENXIO;
    }
    char diagnostic[SCHED_TASK_FILE_PATH_LEN]={0};
    if(!rc) {
        struct vfs_path root={0};
        if(!vfs_namespace_root(ns,&root)) {
            (void)vfs_path_format(ns,&root,&found.path,diagnostic,sizeof(diagnostic));vfs_path_put(&root);
        }
        if(flags&TASK_FILE_FLAG_PATH)rc=open_inode_path(task,&found.path,flags,diagnostic);
        else if(node->flags&STORAGE_NODE_FLAG_DEV_NODE)rc=task_open_device_path(task,&found.path,flags,diagnostic);
        else {
            struct reliefos_permissions value;rc=vfs_path_permissions(&found.path,&value,false);
            if(!rc && (value.mode&LINUX_S_IFMT)==LINUX_S_IFIFO) {
                rc=task_fifo_open(task,node,flags,diagnostic);
                if(task->fifo_open_file)task_file_bind_path(task->fifo_open_file,&found.path);
                else if(rc>=0)task_file_bind_path(task_file_for_fd(task,rc),&found.path);
            } else if(!rc)rc=open_inode_path(task,&found.path,flags,diagnostic);
        }
    }
    vfs_path_put(&found.path);return rc;
}
