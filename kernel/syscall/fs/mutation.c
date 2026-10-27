/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"

/** @brief Authorize a directory mutation and its optional sticky target.
 * @param task Borrowed caller with stable filesystem credentials.
 * @param parent Held actual containing directory, retained across metadata I/O.
 * @param member Borrowed target for sticky checks, NULL for a new name.
 * @return Zero or negative errno. Checks write/search before sticky ownership;
 * CAP_FOWNER permits sticky removal but never bypasses directory DAC. No lock
 * spans provider callbacks; diagnostic names are never used for authority.
 */
int syscall_mutation_parent(struct task *task,const struct vfs_path *parent,
                               const struct vfs_path *member)
{
    int rc=fs_permissions_check_path(task,parent,FS_ACCESS_WRITE|FS_ACCESS_EXEC,false);
    if(rc || !member || (task->cap_effective&(1ULL<<CAP_FOWNER)))return rc;
    struct reliefos_permissions directory,target;
    rc=vfs_path_permissions(parent,&directory,false);
    if(rc || !(directory.mode&01000) || task->fsuid==directory.uid)return rc;
    rc=vfs_path_permissions(member,&target,false);
    return rc ? rc : task->fsuid==target.uid ? 0 : -RELIEFOS_EPERM;
}

/** @brief Remove a directory entry through its retained task/dirfd parent.
 * @param ns Borrowed shared mount namespace under execution ownership.
 * @param task Borrowed caller with stable root/cwd and credentials.
 * @param dirfd Relative directory descriptor or AT_FDCWD.
 * @param pathname User pathname copied once by the common retry scope.
 * @param directory True for rmdir/AT_REMOVEDIR, false for ordinary unlink.
 * @return Zero or negative errno. Final symlinks are never followed, including
 * trailing slash errors; root/dot/dot-dot and mountpoints retain Linux errors.
 * Open inode lifetimes outlive successful removal. No graph lock spans I/O.
 */
int64_t syscall_remove_object(struct vfs_namespace *ns,struct task *task,int32_t dirfd,
                                 uint64_t pathname,bool directory)
{
    if(!task)return -RELIEFOS_ESRCH;
    struct vfs_lookup_result parent={0};struct vfs_path member={0};
    int rc=resolve_user_object_at_flags(ns,task,dirfd,pathname,false,false,VFS_LOOKUP_PARENT,&parent);
    if(rc)return rc;
    if(!parent.last[0] || !__builtin_strcmp(parent.last,".") || !__builtin_strcmp(parent.last,"..")) {
        rc=!directory ? -RELIEFOS_EISDIR : !parent.last[0] ? -RELIEFOS_EBUSY :
            parent.last[1] ? -RELIEFOS_ENOTEMPTY : -RELIEFOS_EINVAL;
    }
    if(!rc)rc=vfs_path_lookup_child(&parent.path,parent.last,&member);
    if(!rc && parent.trailing && vfs_path_node(&member)->type!=RELIEFOS_FS_TYPE_DIR)rc=-RELIEFOS_ENOTDIR;
    if(!rc)rc=syscall_mutation_parent(task,&parent.path,&member);
    if(!rc)rc=vfs_path_remove(ns,&member,directory);
    vfs_path_put(&member);vfs_path_put(&parent.path);return rc;
}

/** @brief Rename members between retained original task/dirfd directories.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed caller with stable filesystem paths and credentials.
 * @param oldfd Source relative directory descriptor or AT_FDCWD.
 * @param oldname Source user pathname, captured by the retry scope.
 * @param newfd Destination relative directory descriptor or AT_FDCWD.
 * @param newname Destination user pathname, captured as a separate operand.
 * @param flags Linux renameat2 flags; nonzero atomics remain unsupported.
 * @return Zero or negative errno. Final symlinks are renamed rather than
 * followed; trailing/type, parent/sticky DAC, mountpoint and cycle gates apply
 * before mutation. Both original parents survive retries. No locks span I/O.
 */
int64_t syscall_rename_object(struct vfs_namespace *ns,struct task *task,int32_t oldfd,uint64_t oldname,
                                 int32_t newfd,uint64_t newname,uint32_t flags)
{
    if(flags)return -RELIEFOS_EOPNOTSUPP;
    if(!task)return -RELIEFOS_ESRCH;
    struct vfs_lookup_result old_parent={0},new_parent={0};struct vfs_path source={0},target={0};
    int rc=resolve_user_object_at_flags(ns,task,oldfd,oldname,false,false,VFS_LOOKUP_PARENT,&old_parent);
    if(!rc && (!old_parent.last[0] || !__builtin_strcmp(old_parent.last,".") ||
       !__builtin_strcmp(old_parent.last,"..")))rc=-RELIEFOS_EBUSY;
    if(!rc)rc=vfs_path_lookup_child(&old_parent.path,old_parent.last,&source);
    if(!rc && old_parent.trailing && vfs_path_node(&source)->type!=RELIEFOS_FS_TYPE_DIR)rc=-RELIEFOS_ENOTDIR;
    if(!rc)rc=resolve_user_object_at_flags(ns,task,newfd,newname,false,false,VFS_LOOKUP_PARENT,&new_parent);
    if(!rc && (!new_parent.last[0] || !__builtin_strcmp(new_parent.last,".") ||
       !__builtin_strcmp(new_parent.last,"..")))rc=-RELIEFOS_EBUSY;
    if(!rc) {
        rc=vfs_path_lookup_child(&new_parent.path,new_parent.last,&target);
        if(rc==-RELIEFOS_ENOENT)rc=0;
    }
    if(!rc && new_parent.trailing && (vfs_path_node(&source)->type!=RELIEFOS_FS_TYPE_DIR ||
       (vfs_path_node(&target) && vfs_path_node(&target)->type!=RELIEFOS_FS_TYPE_DIR)))rc=-RELIEFOS_ENOTDIR;
    if(!rc)rc=syscall_mutation_parent(task,&old_parent.path,&source);
    if(!rc)rc=syscall_mutation_parent(task,&new_parent.path,vfs_path_node(&target) ? &target : NULL);
    if(!rc)rc=vfs_path_rename(ns,&source,&new_parent.path,new_parent.last);
    vfs_path_put(&target);vfs_path_put(&source);vfs_path_put(&new_parent.path);vfs_path_put(&old_parent.path);return rc;
}

/** @brief Add a hardlink using exact source inode and original destination parent.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed caller with stable filesystem paths and credentials.
 * @param oldfd Relative source descriptor or AT_FDCWD, ignored by absolute input.
 * @param oldname User source pathname copied once by the common operand scope.
 * @param newfd Relative destination directory descriptor or AT_FDCWD.
 * @param newname User destination pathname, a separate scoped operand.
 * @param flags AT_SYMLINK_FOLLOW/AT_EMPTY_PATH; all other bits are invalid.
 * @return Zero or negative errno. Empty-source linking requires the documented
 * Linux CAP_DAC_READ_SEARCH capability; ordinary sources do not need read DAC.
 * Directories and zero-link inodes cannot be relinked. No name replay, creation
 * metadata duplication, post-publication allocation or lock across I/O occurs.
 */
int64_t syscall_link_object(struct vfs_namespace *ns,struct task *task,int32_t oldfd,uint64_t oldname,
                               int32_t newfd,uint64_t newname,uint32_t flags)
{
    if(flags&~(LINUX_AT_EMPTY_PATH|LINUX_AT_SYMLINK_FOLLOW))return -RELIEFOS_EINVAL;
    if(!task)return -RELIEFOS_ESRCH;
    struct task_user_text text={0};
    int rc=task_lookup_user_text(task,oldname,VFS_PATH_MAX,false,&text);if(rc)return rc;
    if(!text.text[0] && (flags&LINUX_AT_EMPTY_PATH) && !(task->cap_effective&(1ULL<<CAP_DAC_READ_SEARCH)))
        rc=-RELIEFOS_ENOENT;
    struct vfs_lookup_result source={0},parent={0};struct vfs_path existing={0};
    if(!rc)rc=resolve_kernel_object_at_flags(ns,task,oldfd,text.text,flags&LINUX_AT_EMPTY_PATH,false,
        flags&LINUX_AT_SYMLINK_FOLLOW ? VFS_LOOKUP_FOLLOW : 0,&source);
    kernel_free(text.allocation);
    if(!rc)rc=resolve_user_object_at_flags(ns,task,newfd,newname,false,false,VFS_LOOKUP_PARENT,&parent);
    if(!rc && (!parent.last[0] || !__builtin_strcmp(parent.last,".") || !__builtin_strcmp(parent.last,"..")))
        rc=-RELIEFOS_EEXIST;
    if(!rc) {
        rc=vfs_path_lookup_child(&parent.path,parent.last,&existing);
        if(!rc)rc=-RELIEFOS_EEXIST;
        else if(rc==-RELIEFOS_ENOENT)rc=parent.trailing ? -RELIEFOS_ENOENT : 0;
    }
    if(!rc && vfs_path_node(&source.path)->type==RELIEFOS_FS_TYPE_DIR)rc=-RELIEFOS_EPERM;
    if(!rc)rc=syscall_mutation_parent(task,&parent.path,NULL);
    if(!rc)rc=vfs_path_link(ns,&source.path,&parent.path,parent.last);
    vfs_path_put(&existing);vfs_path_put(&parent.path);vfs_path_put(&source.path);return rc;
}
