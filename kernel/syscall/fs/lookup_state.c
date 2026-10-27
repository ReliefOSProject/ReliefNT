/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"

struct task_lookup_extra {struct task_lookup_extra *next;struct task_lookup_path path;};

/** @brief Identify syscalls whose object operations can retain lookup operands.
 * @param number Native Linux x86-64 syscall number.
 * @return True for pathname/directory-fd operations; ordinary read/write stay
 * on the zero-allocation fast path. Pure classification, no ownership change.
 */
static bool lookup_path_call(uint64_t number)
{
    switch(number) {
    case __NR_open: case __NR_openat: case __NR_openat2: case __NR_creat:
    case __NR_stat: case __NR_lstat: case __NR_fstat: case __NR_newfstatat: case __NR_statx:
    case __NR_chdir: case __NR_fchdir: case __NR_chroot:
    case __NR_mkdir: case __NR_mkdirat: case __NR_rmdir:
    case __NR_unlink: case __NR_unlinkat: case __NR_rename: case __NR_renameat: case __NR_renameat2:
    case __NR_link: case __NR_linkat: case __NR_symlink: case __NR_symlinkat:
    case __NR_readlink: case __NR_readlinkat: case __NR_truncate: case __NR_ftruncate:
    case __NR_chmod: case __NR_fchmod: case __NR_fchmodat: case __NR_fchmodat2:
    case __NR_chown: case __NR_lchown: case __NR_fchown: case __NR_fchownat:
    case __NR_access: case __NR_faccessat: case __NR_faccessat2:
    case __NR_utime: case __NR_utimes: case __NR_futimesat: case __NR_utimensat:
    case __NR_mknod: case __NR_mknodat: case __NR_mount: case __NR_umount2:
    case __NR_statfs: case __NR_fstatfs: case __NR_execve: case __NR_execveat:
    case __NR_bind: case __NR_connect: case __NR_sendto: case __NR_sendmsg: case __NR_sendmmsg:
        return true;
    default:return false;
    }
}

/** @brief Drop one retired lookup operand's copied text and object references.
 * @param path Detached owned operand, required and cleared before return.
 * Serialized task context; final backend releases may perform I/O without any
 * scheduler, graph or module-table lock. No reference is released twice.
 */
static void lookup_path_release(struct task_lookup_path *path)
{
    if(path->read_inode) {
        struct storage_inode_ref *inode=path->read_inode;path->read_inode=NULL;
        storage_inode_allow_write(inode);(void)storage_inode_put(inode);
    }
    vfs_walk_cancel(&path->walk);
    vfs_path_put(&path->result.path);
    path->complete=false;
    kernel_free(path->name);path->name=NULL;
    vfs_path_put(&path->root);vfs_path_put(&path->start);
}

/** @brief Cancel task-owned path snapshots before any final backend release.
 * @param task Owner under execution ownership, nullable.
 * An empty scope performs no work. Detaches the entire list first so nested
 * cleanup cannot release it twice. Removes a parked wait and drains owned
 * storage commands before any path/provider can retire, including direct
 * call-replacement cancellation without an admitted OFD. Backend puts may
 * perform I/O; no scheduler, graph or module-table spinlock spans this call.
 */
void task_lookup_scope_cancel(struct task *task)
{
    if(!task || !task->lookup_scope.armed)return;
    struct task_lookup_scope old=task->lookup_scope;
    task->lookup_scope=(struct task_lookup_scope){0};
    if(task->waiting_queue)kernel_wait_queue_remove(task->waiting_queue,task);
    storage_drain_task_io(task->pid);
    for(size_t i=0;i<2;++i)lookup_path_release(&old.paths[i]);
    while(old.extra) {
        struct task_lookup_extra *entry=old.extra;old.extra=entry->next;
        lookup_path_release(&entry->path);kernel_free(entry);
    }
}

/** @brief Match a retry's original registers or start a fresh lookup scope.
 * @param task Calling task under execution ownership, nullable.
 * @param frame Borrowed syscall registers, required with task.
 * Matching retries keep exact objects and copied text, resetting only the
 * operand cursor. Other calls retire prior state first. No new allocation or
 * lookup I/O; final retired puts may perform I/O without scheduler/graph locks.
 */
void task_lookup_scope_begin(struct task *task,const struct trap_frame *frame)
{
    if(!task || !frame || frame->rax==LINUX_SYS_RT_SIGRETURN)return;
    if(!lookup_path_call(frame->rax)){task_lookup_scope_cancel(task);return;}
    const uint64_t args[]={frame->rdi,frame->rsi,frame->rdx,frame->r10,frame->r8,frame->r9};
    struct task_lookup_scope *scope=&task->lookup_scope;
    if(scope->armed && scope->number==frame->rax && !__builtin_memcmp(scope->args,args,sizeof(args))) {
        scope->cursor=0;return;
    }
    task_lookup_scope_cancel(task);
    scope->armed=true;scope->number=frame->rax;
    __builtin_memcpy(scope->args,args,sizeof(args));
}

/** @brief Borrow the next inline or dynamically extended lookup operand slot.
 * @param task Current task with optional active scope, nullable.
 * @param out Required borrowed output, cleared before validation.
 * @return Zero, ENOMEM for extension allocation or EOVERFLOW for cursor wrap.
 * Unscoped callers receive NULL. No inode I/O or reference transfer occurs;
 * the task owns every returned slot until cancellation. Execution-owned.
 */
int task_lookup_scope_next(struct task *task,struct task_lookup_path **out)
{
    if(!out)return -RELIEFOS_EINVAL;*out=NULL;
    if(!task || !task->lookup_scope.armed)return 0;
    struct task_lookup_scope *scope=&task->lookup_scope;
    if(scope->cursor==SIZE_MAX)return -LINUX_EOVERFLOW;
    size_t index=scope->cursor++;
    if(index<2){*out=&scope->paths[index];return 0;}
    struct task_lookup_extra **link=&scope->extra;
    for(index-=2;index;--index) {
        if(!*link)return -RELIEFOS_EINVAL;
        link=&(*link)->next;
    }
    if(!*link) {
        *link=kernel_malloc(sizeof(**link));if(!*link)return -RELIEFOS_ENOMEM;
        **link=(struct task_lookup_extra){0};
    }
    *out=&(*link)->path;return 0;
}

/** @brief Acquire a retry-stable deny-write guard for a retained executable.
 * @param task Caller with an optional active operand scope, borrowed.
 * @param path Actual authorized regular inode retaining its provider.
 * @param temporary Required owned output for unscoped callers, otherwise NULL.
 * @return Zero or negative type/allocation/ETXTBSY/overflow errno. A scoped
 * guard retains its own path until cancellation and matching retries verify
 * the same inode without acquiring another guard. Unscoped callers allow
 * writes and put their returned inode before releasing their borrowed path.
 * Execution-owned context, without graph/module locks across inode work.
 */
int task_lookup_exec_guard(struct task *task,const struct vfs_path *path,
                             struct storage_inode_ref **temporary)
{
    if(!temporary)return -RELIEFOS_EINVAL;*temporary=NULL;
    const struct storage_node *node=vfs_path_node(path);
    if(!node || node->type!=RELIEFOS_FS_TYPE_FILE)return -RELIEFOS_EACCES;
    struct task_lookup_path *saved=NULL;int rc=task_lookup_scope_next(task,&saved);if(rc)return rc;
    const uint32_t guard_kind=0x20000000u;
    if(saved && saved->read_inode)return saved->flags==guard_kind &&
        vfs_path_same_inode(path,&saved->result.path) ? 0 : -RELIEFOS_EINVAL;
    if(saved && (saved->name || saved->walk || saved->result.path.mount))return -RELIEFOS_EINVAL;
    struct storage_inode_ref *inode=NULL;rc=storage_inode_get(node,&inode);if(rc)return rc;
    if(!inode)return -RELIEFOS_EOPNOTSUPP;
    rc=storage_inode_deny_write(inode);
    if(rc){(void)storage_inode_put(inode);return rc;}
    if(saved) {
        saved->flags=guard_kind;saved->read_inode=inode;vfs_path_get(path,&saved->result.path);
    } else *temporary=inode;
    return 0;
}

/** @brief Capture a non-path string in the current syscall operand scope.
 * @param task Borrowed caller with optional active retry scope.
 * @param user_ptr User string address checked through its terminating NUL.
 * @param maximum Buffer capacity including NUL, at most VFS_PATH_MAX.
 * @param allow_null Treat a NULL address as an empty string when true.
 * @param out Required output, borrowing scoped text or owning allocation for
 * an unscoped call. Caller kernel_free(out->allocation) on every outcome.
 * @return Zero or negative errno, with empty output on failure. Scoped copies
 * belong to the task until common cancellation and matching retries do not
 * reread user memory. No inode I/O, reference transfer or spinlock occurs.
 */
int task_lookup_user_text(struct task *task,uint64_t user_ptr,uint32_t maximum,
                            bool allow_null,struct task_user_text *out)
{
    if(!out)return -RELIEFOS_EINVAL;*out=(struct task_user_text){0};
    if(!maximum || maximum>VFS_PATH_MAX)return -RELIEFOS_EINVAL;
    const uint32_t text_kind=0x80000000u;
    struct task_lookup_path *saved=NULL;int rc=task_lookup_scope_next(task,&saved);if(rc)return rc;
    if(saved && saved->name) {
        if(saved->flags!=text_kind || saved->dirfd!=(int32_t)maximum || saved->allow_empty!=allow_null)
            return -RELIEFOS_EINVAL;
        out->text=saved->name;return 0;
    }
    char *copy=kernel_malloc(maximum);if(!copy)return -RELIEFOS_ENOMEM;
    if(!user_ptr && allow_null)copy[0]=0;
    else rc=copy_user_path(copy,maximum,user_ptr);
    if(rc){kernel_free(copy);return rc;}
    if(saved) {
        saved->name=copy;saved->flags=text_kind;saved->dirfd=(int32_t)maximum;saved->allow_empty=allow_null;
    } else out->allocation=copy;
    out->text=copy;return 0;
}

/** @brief Capture exact binary user bytes in this syscall's operand scope.
 * @param task Borrowed caller with optional armed retry scope.
 * @param user_ptr User address checked for the entire counted range.
 * @param length Count from one through INT32_MAX, including embedded NUL.
 * Callers check their own ABI limits before requesting a snapshot.
 * @param out Required output; scoped bytes are borrowed, unscoped allocation
 * is owned and must be kernel_free'd by the caller on every return.
 * @return Zero or negative errno with empty output on failure. Matching
 * retries never reread changed/unmapped input. Common cancellation owns scoped
 * storage; no namespace lookup, inode I/O or spinlock participates.
 */
int task_lookup_user_bytes(struct task *task,uint64_t user_ptr,uint32_t length,struct task_user_text *out)
{
    if(!out)return -RELIEFOS_EINVAL;*out=(struct task_user_text){0};
    if(!length || length>INT32_MAX)return -RELIEFOS_EINVAL;
    const uint32_t binary_kind=0x40000000u;
    struct task_lookup_path *saved=NULL;int rc=task_lookup_scope_next(task,&saved);if(rc)return rc;
    if(saved && saved->name) {
        if(saved->flags!=binary_kind || saved->dirfd!=(int32_t)length)return -RELIEFOS_EINVAL;
        out->text=saved->name;return 0;
    }
    if(!user_range_ok(user_ptr,length))return -RELIEFOS_EFAULT;
    char *copy=kernel_malloc(length);if(!copy)return -RELIEFOS_ENOMEM;
    __builtin_memcpy(copy,(const void *)(uintptr_t)user_ptr,length);
    if(saved){saved->name=copy;saved->flags=binary_kind;saved->dirfd=(int32_t)length;}
    else out->allocation=copy;
    out->text=copy;return 0;
}
