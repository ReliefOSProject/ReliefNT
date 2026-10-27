/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"

/** @brief Derive final creation metadata from the actual authorized parent.
 * @param task Borrowed caller and stable filesystem credentials.
 * @param parent Held parent retaining the provider through metadata I/O.
 * @param type Full Linux S_IF* inode type.
 * @param mode Requested permission/special bits.
 * @param out Receives final mode, fsuid and inherited/default group.
 * @return Zero or negative errno. Applies umask, directory SGID inheritance,
 * non-directory CAP_FSETID/group rules and symlink0777. No locks span I/O.
 */
int syscall_creation_mode(struct task *task,const struct vfs_path *parent,uint32_t type,
                             uint32_t mode,struct reliefos_permissions *out)
{
    if(!task || !out)return -RELIEFOS_EINVAL;
    int rc=fs_permissions_check_path(task,parent,FS_ACCESS_WRITE|FS_ACCESS_EXEC,false);
    struct reliefos_permissions inherited;
    if(!rc)rc=vfs_path_permissions(parent,&inherited,false);
    if(rc)return rc;
    *out=(struct reliefos_permissions){type==LINUX_S_IFLNK ? 0777 : (mode&07777)&~*sched_task_umask(task),
        task->fsuid,task->fsgid};
    if(inherited.mode&02000) {
        out->gid=inherited.gid;
        if(type==LINUX_S_IFDIR)out->mode|=02000;
    }
    if(type!=LINUX_S_IFDIR && !(task->cap_effective&(1ULL<<CAP_FSETID)) &&
       !task_in_group(task,out->gid,false))out->mode&=~02000u;
    return 0;
}

/** @brief Create one member through the retained actual task/dirfd parent.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed current task with stable credentials and FS references.
 * @param dirfd Relative base descriptor or AT_FDCWD.
 * @param pathname User pathname captured once by the common retry scope.
 * @param type S_IFREG/S_IFDIR/S_IFLNK/S_IFIFO/S_IFSOCK inode type.
 * @param mode Requested permission bits subject to umask/inheritance.
 * @param target Borrowed kernel symlink payload, NULL for other types.
 * @return Zero or negative errno. Existing entries return EEXIST before parent
 * write checks. A successful create has no following VFS allocation/open step;
 * retained operands survive retries without text replay or locks across I/O.
 */
int64_t syscall_create_object(struct vfs_namespace *ns,struct task *task,int32_t dirfd,
                                uint64_t pathname,uint32_t type,uint32_t mode,const char *target)
{
    if(!task)return -RELIEFOS_ESRCH;
    if(type!=LINUX_S_IFREG && type!=LINUX_S_IFDIR && type!=LINUX_S_IFLNK &&
       type!=LINUX_S_IFIFO && type!=LINUX_S_IFSOCK)return -RELIEFOS_EOPNOTSUPP;
    struct vfs_lookup_result parent={0};
    int rc=resolve_user_object_at_flags(ns,task,dirfd,pathname,false,false,VFS_LOOKUP_PARENT,&parent);
    if(rc)return rc;
    if(!parent.last[0] || !__builtin_strcmp(parent.last,".") || !__builtin_strcmp(parent.last,".."))
        rc=-RELIEFOS_EEXIST;
    struct vfs_path existing={0};
    if(!rc) {
        rc=vfs_path_lookup_child(&parent.path,parent.last,&existing);
        if(!rc)rc=-RELIEFOS_EEXIST;
        else if(rc==-RELIEFOS_ENOENT)rc=0;
    }
    vfs_path_put(&existing);
    if(!rc && parent.trailing && type!=LINUX_S_IFDIR)rc=-RELIEFOS_ENOENT;
    struct reliefos_permissions value;
    if(!rc)rc=syscall_creation_mode(task,&parent.path,type,mode,&value);
    if(!rc)rc=vfs_path_create_node(&parent.path,parent.last,type,&value,target,NULL);
    vfs_path_put(&parent.path);return rc;
}

/** @brief Capture a symlink's target before resolving its creation operand.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed current task and optional active retry scope.
 * @param target User payload copied once, never interpreted as a lookup path.
 * @param dirfd Base descriptor for the new link or AT_FDCWD.
 * @param pathname User link pathname, captured independently by the resolver.
 * @return Zero or negative errno. Active payloads belong to common scope
 * cancellation; unscoped allocations are freed on every return. No graph or
 * module lock spans the creation callback or metadata authorization.
 */
int64_t syscall_symlink_object(struct vfs_namespace *ns,struct task *task,uint64_t target,
                                 int32_t dirfd,uint64_t pathname)
{
    struct task_user_text text={0};
    int64_t rc=task_lookup_user_text(task,target,VFS_PATH_MAX,false,&text);
    if(!rc)rc=text.text[0] ? syscall_create_object(ns,task,dirfd,pathname,LINUX_S_IFLNK,0777,text.text) : -RELIEFOS_ENOENT;
    kernel_free(text.allocation);return rc;
}
