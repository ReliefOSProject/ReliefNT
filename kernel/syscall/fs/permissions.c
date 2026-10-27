/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"

/** @brief Apply chmod/chown to the original descriptor without name replay.
 * @param task Borrowed calling task with dispatcher-owned OFD/PTY admission.
 * @param fd Descriptor number, possibly closed/reused since admission.
 * @param chmod Select mode change instead of ownership change.
 * @param value Mode or user ID.
 * @param gid Group ID for ownership changes; UINT32_MAX preserves it.
 * @return Zero or negative errno. O_PATH is rejected for descriptor variants.
 * Builtin implicit terminals and materialized builtin device descriptions use
 * their admitted inode identity, independent of a displayed pathname;
 * all opened path objects retain the provider across metadata I/O.
 */
static int permissions_fd(struct task *task,int fd,bool chmod,uint32_t value,uint32_t gid)
{
    struct task_file *file=task_file_for_io(task,fd);
    const struct vfs_path *path=NULL;
    if(file) {
        if(file->flags&TASK_FILE_FLAG_PATH)return -RELIEFOS_EBADF;
        path=&file->path_ref;
    } else {
        struct task_pty_fd *endpoint=task_pty_for_io(task,fd);
        if(endpoint && endpoint->description)path=&endpoint->description->path_ref;
    }
    if(path && vfs_path_node(path))return chmod ? fs_permissions_chmod_path(task,path,value) :
        fs_permissions_chown_path(task,path,value,gid);
    if(file) {
        if(!(file->flags&(TASK_FILE_FLAG_DEV_NODE|TASK_FILE_FLAG_DEV_NULL)) ||
           !(file->node.flags&STORAGE_NODE_FLAG_DEV_NODE))return -RELIEFOS_EOPNOTSUPP;
        return chmod ? fs_virtual_chmod(task,"builtin-device",&file->node,value) :
            fs_virtual_chown(task,"builtin-device",&file->node,value,gid);
    }
    struct storage_node node;const char *intrinsic;
    int rc=task_pty_node_for_fd(task,fd,&node,&intrinsic);
    if(rc)return rc;
    return chmod ? fs_virtual_chmod(task,intrinsic,&node,value) :
        fs_virtual_chown(task,intrinsic,&node,value,gid);
}

/** @brief Dispatch Linux DAC checks and permission/ownership changes.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed calling task with stable credentials.
 * @param number ACCESS/FACCESSAT/FACCESSAT2 or chmod/chown family number.
 * @param a0 Path address or descriptor, according to the Linux ABI.
 * @param a1 Mode/user ID or at-path address.
 * @param a2 Group ID, at-mode or at-user ID.
 * @param a3 At-group ID or extended chmod/access flags.
 * @param a4 FCHOWNAT flags.
 * @return Zero or negative errno. Search checks and final authorization use
 * actual retained objects; real IDs are used by access unless AT_EACCESS.
 * Common resolver state retains operands across I/O, with no graph/module
 * spinlock held by this dispatcher or across provider callbacks.
 */
int64_t syscall_permissions(struct vfs_namespace *ns,struct task *task,uint64_t number,
                              uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4)
{
    if(!task)return -RELIEFOS_ESRCH;
    if(number==LINUX_SYS_FCHMOD || number==LINUX_SYS_FCHOWN)
        return permissions_fd(task,(int32_t)a0,number==LINUX_SYS_FCHMOD,(uint32_t)a1,(uint32_t)a2);
    bool access=number==LINUX_SYS_ACCESS || number==LINUX_SYS_FACCESSAT || number==LINUX_SYS_FACCESSAT2;
    bool chmod=number==LINUX_SYS_CHMOD || number==LINUX_SYS_FCHMODAT || number==LINUX_SYS_FCHMODAT2;
    bool at=number==LINUX_SYS_FACCESSAT || number==LINUX_SYS_FACCESSAT2 ||
        number==LINUX_SYS_FCHMODAT || number==LINUX_SYS_FCHMODAT2 || number==LINUX_SYS_FCHOWNAT;
    if(!access && !chmod && number!=LINUX_SYS_CHOWN && number!=LINUX_SYS_LCHOWN && number!=LINUX_SYS_FCHOWNAT)
        return -RELIEFOS_ENOSYS;
    uint32_t flags=number==LINUX_SYS_FCHOWNAT ? (uint32_t)a4 :
        number==LINUX_SYS_FACCESSAT2 || number==LINUX_SYS_FCHMODAT2 ? (uint32_t)a3 : 0;
    uint32_t supported=LINUX_AT_SYMLINK_NOFOLLOW|LINUX_AT_EMPTY_PATH;
    if(access)supported|=LINUX_AT_EACCESS;
    if(flags&~supported)return -RELIEFOS_EINVAL;
    uint32_t value=(uint32_t)(at ? a2 : a1),gid=(uint32_t)(at ? a3 : a2);
    if(access && (value&~7u))return -RELIEFOS_EINVAL;
    bool real_ids=access && !(flags&LINUX_AT_EACCESS);
    uint32_t follow=number==LINUX_SYS_LCHOWN || (flags&LINUX_AT_SYMLINK_NOFOLLOW) ? 0 : VFS_LOOKUP_FOLLOW;
    struct vfs_lookup_result found={0};
    int rc=resolve_user_object_at_flags(ns,task,at ? (int32_t)a0 : LINUX_AT_FDCWD,
        at ? a1 : a0,(flags&LINUX_AT_EMPTY_PATH)!=0,real_ids,follow,&found);
    if(!rc)rc=access ? fs_permissions_check_path(task,&found.path,value,real_ids) :
        chmod ? fs_permissions_chmod_path(task,&found.path,value) :
        fs_permissions_chown_path(task,&found.path,value,gid);
    vfs_path_put(&found.path);return rc;
}
