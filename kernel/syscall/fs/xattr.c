/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"
#include <reliefnt/swap.h>
#include <linux/capability.h>
#include <linux/mount.h>

#define XATTR_NAME_MAX 255u
#define XATTR_BUFFER_MAX 65536u

/** @brief Apply Linux attribute namespace visibility and inode read permission.
 * @param task Borrowed calling task with stable effective credentials.
 * @param path Borrowed retained path to a valid inode.
 * @param name Borrowed NUL-terminated name with its namespace prefix.
 * @param privileged True when the caller has effective CAP_SYS_ADMIN.
 * @return Zero on success, or negative errno or internal retry.
 */
static int xattr_get_permission(struct task *task,const struct vfs_path *path,
                                const char *name,bool privileged)
{
    if (!__builtin_strncmp(name,"trusted.",8)) return privileged ? 0 : -LINUX_ENODATA;
    if (!__builtin_strncmp(name,"security.",9)) return 0;
    if (__builtin_strncmp(name,"user.",5)) return -RELIEFOS_EOPNOTSUPP;
    const struct storage_node *node=vfs_path_node(path);
    if (node->type!=RELIEFOS_FS_TYPE_FILE && node->type!=RELIEFOS_FS_TYPE_DIR)
        return -LINUX_ENODATA;
    return fs_permissions_check_path(task,path,4,false);
}

/** @brief Resolve a descriptor using the open file description held by this syscall.
 * @param task Borrowed calling task with its descriptor scope pinned.
 * @param fd Signed Linux descriptor number.
 * @param path Required output for a borrowed path valid within the descriptor scope.
 * @return Zero, EBADF for invalid or O_PATH descriptors, or EOPNOTSUPP without a VFS path.
 */
static int xattr_fd_path(struct task *task,int fd,const struct vfs_path **path)
{
    struct task_file *file=task_file_for_io(task,fd);
    if (file) {
        if (file->flags&TASK_FILE_FLAG_PATH) return -RELIEFOS_EBADF;
        if (vfs_path_node(&file->path_ref)) {
            *path=&file->path_ref;
            return 0;
        }
    }
    struct task_pty_fd *pty=task_pty_for_io(task,fd);
    if (pty && pty->description && vfs_path_node(&pty->description->path_ref)) {
        *path=&pty->description->path_ref;
        return 0;
    }
    if (!file && !pty && task_fd_descriptor_flags(task,fd)<0) return -RELIEFOS_EBADF;
    return -RELIEFOS_EOPNOTSUPP;
}

/** @brief Read or list attributes without exposing a user pointer to filesystem providers.
 * @param ns Borrowed namespace under execution ownership.
 * @param task Borrowed calling task, or NULL if there is no task.
 * @param number Linux extended-attribute syscall number.
 * @param a0 User pathname address or descriptor.
 * @param a1 User attribute name address or list output address.
 * @param a2 User value address or list capacity in bytes.
 * @param a3 Value capacity in bytes for operations with a value argument.
 * @param a4 Flags for set operations.
 * @return Byte count or negative errno, including EROFS for mutation on readonly mounts.
 * An internal retry may be requested by path resolution or demand paging.
 */
int64_t syscall_xattr(struct vfs_namespace *ns,struct task *task,uint64_t number,
                      uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4)
{
    if (!task) return -RELIEFOS_ESRCH;
    bool fd=number==__NR_fgetxattr || number==__NR_flistxattr ||
        number==__NR_fsetxattr || number==__NR_fremovexattr;
    bool nofollow=number==__NR_lgetxattr || number==__NR_llistxattr ||
        number==__NR_lsetxattr || number==__NR_lremovexattr;
    bool list=number>=__NR_listxattr && number<=__NR_flistxattr;
    bool set=number>=__NR_setxattr && number<=__NR_fsetxattr;
    bool mutation=set || number>=__NR_removexattr;
    if (set && ((uint32_t)a4&~3u)) return -RELIEFOS_EINVAL;
    if (set && a3>XATTR_BUFFER_MAX) return -RELIEFOS_E2BIG;

    const struct vfs_path *path=NULL;
    struct task_user_text name={0};
    struct vfs_lookup_result found={0};
    int rc=0;
    if (!list) {
        rc=task_lookup_user_text(task,a1,XATTR_NAME_MAX+1,false,&name);
        if (rc==-RELIEFOS_ENAMETOOLONG || (!rc && !name.text[0])) rc=-RELIEFOS_ERANGE;
    }
    if (!rc && fd) rc=xattr_fd_path(task,(int32_t)a0,&path);
    if (!rc && !fd) {
        rc=resolve_user_object_at_flags(ns,task,LINUX_AT_FDCWD,a0,false,false,
                                        nofollow ? 0 : VFS_LOOKUP_FOLLOW,&found);
        path=&found.path;
    }
    uint64_t size=list ? a2 : a3;
    uint32_t capacity=size>XATTR_BUFFER_MAX ? XATTR_BUFFER_MAX : (uint32_t)size;
    uint32_t length=0;
    void *buffer=NULL;
    bool privileged=(task->cap_effective&(1ull<<CAP_SYS_ADMIN))!=0;
    if (!rc && mutation)
        rc=(vfs_path_mount_flags(path)&MS_RDONLY) ? -RELIEFOS_EROFS : -RELIEFOS_EOPNOTSUPP;
    if (!rc && !list) rc=xattr_get_permission(task,path,name.text,privileged);
    if (!rc && capacity) {
        buffer=kernel_malloc(capacity);
        if (!buffer) rc=-RELIEFOS_ENOMEM;
    }
    if (!rc) {
        if (list) rc=vfs_path_listxattr(path,buffer,capacity,&length,privileged);
        else rc=vfs_path_getxattr(path,name.text,buffer,capacity,&length);
        if (!rc) storage_release_task_io(task->pid);
    }
    if (!rc && capacity && length) {
        uint64_t address=list ? a1 : a2;
        int prep=swap_prepare_user_range(task,address,length,true);
        if (prep==SWAP_STORAGE_CONTENDED) rc=KERNEL_SYSCALL_SWAP_CONTENDED;
        else if (prep || !user_range_writable(address,length)) rc=-RELIEFOS_EFAULT;
        else __builtin_memcpy((void *)(uintptr_t)address,buffer,length);
    }
    kernel_free(buffer);
    kernel_free(name.allocation);
    vfs_path_put(&found.path);
    return rc ? (int64_t)rc : length;
}
