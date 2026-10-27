/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"

/** @brief Refresh diagnostic directory names from authoritative task objects.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Current task with owned root/cwd references.
 * Formatting never resolves a path or changes ownership. Removed/unrepresentable
 * names become empty; callers must use the actual references for filesystem I/O.
 */
static void directory_display_names(struct vfs_namespace *ns,struct task *task)
{
    struct vfs_path anchor={0};
    if(vfs_namespace_root(ns,&anchor))return;
    struct task_fs_paths *paths=sched_task_paths(task);
    if(vfs_path_format(ns,&anchor,&paths->root,sched_task_root_dir(task),RELIEFOS_FS_PATH_LEN))
        sched_task_root_dir(task)[0]=0;
    if(vfs_path_format(ns,&anchor,&paths->cwd,sched_task_cwd(task),RELIEFOS_FS_PATH_LEN))
        sched_task_cwd(task)[0]=0;
    vfs_path_put(&anchor);
}

/** @brief Implement directory syscalls using held mount/dentry objects.
 * @param ns Borrowed system namespace under the execution transaction.
 * @param task Current task; CLONE_FS users update the shared filesystem state.
 * @param number GETCWD, CHDIR, FCHDIR or CHROOT Linux syscall number.
 * @param a0 User pathname/output pointer or FCHDIR descriptor number.
 * @param a1 GETCWD output capacity in bytes; ignored for other operations.
 * @return Raw Linux getcwd length including NUL, zero for directory changes,
 * or negative errno. Resolution/search may yield EAGAIN; no graph/scheduler
 * lock spans I/O. An error never replaces root/cwd. chroot preserves cwd.
 */
int64_t syscall_directory(struct vfs_namespace *ns,struct task *task,uint64_t number,
                            uint64_t a0,uint64_t a1)
{
    if(!task)return -RELIEFOS_ESRCH;
    struct task_fs_paths *paths=sched_task_paths(task);
    if(number==LINUX_SYS_GETCWD) {
        if(!a1)return -RELIEFOS_ERANGE;
        char *buffer=kernel_malloc(VFS_PATH_MAX);if(!buffer)return -RELIEFOS_ENOMEM;
        int64_t rc=vfs_path_getcwd(ns,&paths->root,&paths->cwd,buffer,VFS_PATH_MAX);
        if(!rc) {
            size_t length=__builtin_strlen(buffer)+1;
            if(length>a1)rc=-RELIEFOS_ERANGE;
            else if(!user_range_writable(a0,length))rc=-RELIEFOS_EFAULT;
            else {__builtin_memcpy((void *)(uintptr_t)a0,buffer,length);rc=(int64_t)length;}
        }
        kernel_free(buffer);return rc;
    }
    if(number!=LINUX_SYS_CHDIR && number!=LINUX_SYS_FCHDIR && number!=LINUX_SYS_CHROOT)
        return -RELIEFOS_ENOSYS;
    struct vfs_lookup_result found={0};
    int rc=number==LINUX_SYS_FCHDIR ?
        resolve_kernel_object_at_flags(ns,task,(int32_t)a0,"",true,false,0,&found) :
        resolve_user_object_at_flags(ns,task,LINUX_AT_FDCWD,a0,false,false,VFS_LOOKUP_FOLLOW,&found);
    if(rc)return rc;
    if(vfs_path_node(&found.path)->type!=RELIEFOS_FS_TYPE_DIR)rc=-RELIEFOS_ENOTDIR;
    if(!rc)rc=fs_permissions_check_path(task,&found.path,FS_ACCESS_EXEC,false);
    if(!rc && number==LINUX_SYS_CHROOT && !(task->cap_effective&(1ULL<<CAP_SYS_CHROOT)))
        rc=-RELIEFOS_EPERM;
    if(!rc) {
        struct vfs_path *target=number==LINUX_SYS_CHROOT ? &paths->root : &paths->cwd;
        struct vfs_path old=*target;*target=found.path;found.path=(struct vfs_path){0};
        vfs_path_put(&old);directory_display_names(ns,task);
    }
    vfs_path_put(&found.path);return rc;
}
