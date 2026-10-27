#include "../internal.h"

struct object_lookup_credentials {const struct task *task;bool real_ids;};

/** @brief Authorize search on the actual held directory before traversal.
 * @param context Borrowed credentials from this synchronous lookup.
 * @param directory Borrowed directory path, retained by the VFS walker.
 * @return Zero or negative errno; metadata I/O runs under execution ownership
 * with no graph lock. Neither callback nor context survives this call.
 */
static int object_lookup_search(void *context,const struct vfs_path *directory)
{
    const struct object_lookup_credentials *credentials=context;
    return fs_permissions_check_path(credentials->task,directory,FS_ACCESS_EXEC,credentials->real_ids);
}

/** @brief Resolve a retained operand or capture its input before retryable I/O.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed calling task and its stable credentials.
 * @param dirfd Directory descriptor or AT_FDCWD, ignored by absolute input.
 * @param raw Borrowed kernel input; NULL only when saved already owns its copy.
 * @param allow_empty Permit the exact cwd/OFD for empty input.
 * @param real_ids Use real instead of filesystem IDs for directory search.
 * @param lookup_flags VFS_LOOKUP_FOLLOW/PARENT/MISSING bits.
 * @param saved Optional task-owned retry operand; populated before VFS I/O.
 * @param out Empty result owning one path on success.
 * @return Zero or negative errno with empty output on failure. A populated
 * saved operand retains names and root/start paths across EAGAIN, independently
 * of later CLONE_FS changes, fd close/reuse or caller memory changes. No graph,
 * scheduler or module-table lock may span the walker/provider callbacks.
 */
static int object_lookup_run(struct vfs_namespace *ns,struct task *task,int32_t dirfd,
                                const char *raw,bool allow_empty,bool real_ids,uint32_t lookup_flags,
                                struct task_lookup_path *saved,struct vfs_lookup_result *out)
{
    if(!out)return -RELIEFOS_EINVAL;
    *out=(struct vfs_lookup_result){0};
    if(!task || lookup_flags&~(VFS_LOOKUP_FOLLOW|VFS_LOOKUP_PARENT|VFS_LOOKUP_MISSING))return -RELIEFOS_EINVAL;
    if(!ns || !ns->root)return -RELIEFOS_ENODEV;
    const struct vfs_path *root,*start;
    if(saved && saved->name) {
        if(saved->dirfd!=dirfd || saved->flags!=lookup_flags || saved->allow_empty!=allow_empty ||
           saved->real_ids!=real_ids)return -RELIEFOS_EINVAL;
        if(saved->complete) {
            *out=saved->result;out->path=(struct vfs_path){0};
            vfs_path_get(&saved->result.path,&out->path);return 0;
        }
        raw=saved->name;root=&saved->root;start=&saved->start;
    } else {
        if(!raw)return -RELIEFOS_EINVAL;
        size_t length=0;while(length<VFS_PATH_MAX && raw[length])++length;
        if(length==VFS_PATH_MAX)return -RELIEFOS_ENAMETOOLONG;
        struct task_fs_paths *paths=sched_task_paths(task);
        if(!vfs_path_node(&paths->root) || !vfs_path_node(&paths->cwd))return -RELIEFOS_ENODEV;
        if(!raw[0] && !allow_empty)return -RELIEFOS_ENOENT;
        root=&paths->root;start=&paths->cwd;
        if(raw[0]!='/' && dirfd!=LINUX_AT_FDCWD) {
            struct task_file *file=task_file_for_io(task,dirfd);
            if(file) {
                if(raw[0] && file->node.type!=RELIEFOS_FS_TYPE_DIR)return -RELIEFOS_ENOTDIR;
                start=&file->path_ref;
            }
            else {
                struct task_pty_fd *endpoint=task_pty_for_io(task,dirfd);
                if(!endpoint || !endpoint->description)return -RELIEFOS_EBADF;
                if(raw[0])return -RELIEFOS_ENOTDIR;
                start=&endpoint->description->path_ref;
            }
            if(!vfs_path_node(start))return -RELIEFOS_EBADF;
            if(raw[0] && vfs_path_node(start)->type!=RELIEFOS_FS_TYPE_DIR)return -RELIEFOS_ENOTDIR;
        }
        if(saved) {
            char *copy=kernel_malloc(length+1);if(!copy)return -RELIEFOS_ENOMEM;
            __builtin_memcpy(copy,raw,length+1);
            vfs_path_get(root,&saved->root);vfs_path_get(start,&saved->start);
            saved->dirfd=dirfd;saved->flags=lookup_flags;
            saved->allow_empty=allow_empty;saved->real_ids=real_ids;saved->name=copy;
            raw=copy;root=&saved->root;start=&saved->start;
        }
    }
    int rc=0;
    if(!raw[0])vfs_path_get(start,&out->path);
    else {
        struct object_lookup_credentials credentials={task,real_ids};
        struct vfs_lookup_policy policy={lookup_flags,object_lookup_search,&credentials};
        rc=saved ? vfs_path_resolve_resume(ns,root,start,raw,&policy,&saved->walk,out) :
            vfs_path_resolve(ns,root,start,raw,&policy,out);
    }
    if(!rc && saved) {
        saved->result=*out;saved->result.path=(struct vfs_path){0};
        vfs_path_get(&out->path,&saved->result.path);saved->complete=true;
    }
    return rc;
}

/** @brief Resolve a kernel-owned at-path against stable task/OFD objects.
 * @param ns Shared namespace under execution ownership.
 * @param task Borrowed current task with initialized root/cwd paths.
 * @param dirfd Directory descriptor or AT_FDCWD; absolute input ignores it.
 * @param raw Borrowed NUL-terminated input, bounded by VFS_PATH_MAX.
 * @param allow_empty Permit the exact cwd/OFD for an empty input.
 * @param real_ids Use real credentials for directory search.
 * @param lookup_flags VFS_LOOKUP_FOLLOW/PARENT/MISSING bits.
 * @param out Empty result receiving one owned path or creation parent.
 * @return Zero or negative errno with empty output on failure. An active
 * syscall scope owns a copy and both directory references across I/O retries;
 * unscoped trusted calls retain only the returned path. No text-prefix route
 * or diagnostic name is consulted; no graph lock spans backend callbacks.
 */
int resolve_kernel_object_at_flags(struct vfs_namespace *ns,struct task *task,int32_t dirfd,
                                     const char *raw,bool allow_empty,bool real_ids,
                                     uint32_t lookup_flags,struct vfs_lookup_result *out)
{
    if(!out)return -RELIEFOS_EINVAL;*out=(struct vfs_lookup_result){0};
    if(!raw)return -RELIEFOS_EINVAL;
    struct task_lookup_path *saved=NULL;int rc=task_lookup_scope_next(task,&saved);
    return rc ? rc : object_lookup_run(ns,task,dirfd,raw,allow_empty,real_ids,lookup_flags,saved,out);
}

/** @brief Copy user text once per syscall operand and resolve retained objects.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed current task with root/cwd and serialized descriptor state.
 * @param dirfd Directory descriptor or AT_FDCWD, ignored for absolute input.
 * @param user_ptr User pathname address, checked including terminating NUL.
 * @param allow_empty Permit the exact cwd/OFD for an empty input.
 * @param real_ids Use real credentials for directory search.
 * @param lookup_flags VFS_LOOKUP_FOLLOW/PARENT/MISSING policy.
 * @param out Empty output receiving one owned path or a creation parent.
 * @return Zero or negative errno. An active retry scope owns the checked copy
 * and original roots/dirfd until final syscall cleanup, never a user pointer.
 * On EAGAIN a retry uses that copy even if user memory was changed/unmapped.
 * Unscoped copies and all per-call temporaries are released before return.
 */
int resolve_user_object_at_flags(struct vfs_namespace *ns,struct task *task,int32_t dirfd,
                                   uint64_t user_ptr,bool allow_empty,bool real_ids,
                                   uint32_t lookup_flags,struct vfs_lookup_result *out)
{
    if(!out)return -RELIEFOS_EINVAL;*out=(struct vfs_lookup_result){0};
    struct task_lookup_path *saved=NULL;int rc=task_lookup_scope_next(task,&saved);if(rc)return rc;
    if(saved && saved->name)
        return object_lookup_run(ns,task,dirfd,NULL,allow_empty,real_ids,lookup_flags,saved,out);
    char *raw=kernel_malloc(VFS_PATH_MAX);if(!raw)return -RELIEFOS_ENOMEM;
    rc=copy_user_path(raw,VFS_PATH_MAX,user_ptr);
    if(!rc)rc=object_lookup_run(ns,task,dirfd,raw,allow_empty,real_ids,lookup_flags,saved,out);
    kernel_free(raw);return rc;
}



/**
 * Copy user path.
 * @param dst Value supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @param user_ptr Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int copy_user_path(char *dst, uint32_t cap, uint64_t user_ptr)
{
    size_t len;
    if (!dst || !cap || !user_range_ok(user_ptr, 1)) {
        return -RELIEFOS_EFAULT;
    }
    len = user_strlen((const char *)(uintptr_t)user_ptr, cap);
    if (len == cap) return user_range_ok(user_ptr, cap) ? -RELIEFOS_ENAMETOOLONG : -RELIEFOS_EFAULT;
    if (!user_range_ok(user_ptr, len + 1)) {
        return -RELIEFOS_EFAULT;
    }
    for (size_t i = 0; i <= len; ++i) {
        dst[i] = ((const char *)(uintptr_t)user_ptr)[i];
    }
    return 0;
}

/** @brief Translate native filesystem errors to the Linux syscall aliases.
 * @param ret Native status, including negative errno or successful values.
 * @return Corresponding Linux errno for recognized values, otherwise ret
 * unchanged. Pure conversion used by file-backed page faults as well as
 * syscall paths; no I/O, locks, allocation or ownership transfer.
 */
int storage_errno(int ret)
{
    if (ret == -2) {
        return -RELIEFOS_ENOENT;
    }
    if (ret == -17) {
        return -RELIEFOS_EEXIST;
    }
    if (ret == -20) {
        return -RELIEFOS_ENOTDIR;
    }
    if (ret == -21) {
        return -RELIEFOS_EISDIR;
    }
    if (ret == -39) {
        return -RELIEFOS_ENOTEMPTY;
    }
    if (ret == -18) {
        return -LINUX_EXDEV;
    }
    if (ret == -95) {
        return -RELIEFOS_EOPNOTSUPP;
    }
    return ret;
}
