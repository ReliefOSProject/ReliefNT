/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"
#include <reliefnt/swap.h>
#include <reliefnt/export.h>
#ifdef CONFIG_SWAP
#include <reliefnt/memory_pressure.h>
#include <reliefnt/lock.h>
/** @brief Cancel the task's pressure wait and forget its stall accounting.
 * @param task Execution-owned task, nullable; borrowed without ownership transfer.
 * Only a link owned by the pressure queue is removed; unrelated waits remain.
 * The native reclaim producer supplies the hook, while standalone lookup
 * fixtures may omit it. MinCore and open-path continuations check it on cancellation.
 */
extern void memory_wait_cancel(struct task *) __attribute__((weak));
#endif

struct task_lookup_extra {struct task_lookup_extra *next;struct task_lookup_path path;};

/* Binary operands retain their copied prefix across a prerequisite wait. */
struct lookup_bytes_snapshot {
    uint64_t user_ptr;
    uint32_t length, done;
    char bytes[];
};

/* A partially copied string is owned until its NUL or terminal cleanup. */
struct lookup_text_snapshot {
    uint64_t user_ptr;
    uint32_t maximum,done;
    bool allow_null,complete;
    char bytes[];
};

/** @brief Identify syscalls whose object operations can retain lookup operands.
 * @param number Native Linux x86-64 syscall number.
 * @return True for pathname/directory-fd and native message operations.
 * Ordinary file read/write stay on the zero-allocation fast path. Pure
 * classification, no ownership change.
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
#ifdef CONFIG_SWAP
    case __NR_recvfrom: case __NR_recvmsg: case __NR_recvmmsg: case __NR_mincore:
    case __NR_prlimit64: case __NR_rt_sigprocmask: case __NR_timerfd_settime:
    case __NR_memfd_create: case __NR_swapon: case __NR_swapoff: case __NR_clone3:
    case __NR_process_vm_readv: case __NR_process_vm_writev:
    case __NR_poll: case __NR_ppoll: case __NR_select: case __NR_pselect6:
    case __NR_getdents: case __NR_getdents64:
    case __NR_msgsnd: case __NR_msgrcv: case __NR_msgctl:
    case __NR_semop: case __NR_semtimedop: case __NR_semctl:
    case __NR_init_module: case __NR_finit_module: case __NR_delete_module:
    case __NR_accept: case __NR_accept4: case __NR_getsockname: case __NR_getpeername:
    case __NR_getsockopt: case __NR_setsockopt: case __NR_socketpair:
#endif
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
#ifdef CONFIG_SWAP
    if(path->name && path->flags==TASK_LOOKUP_PROCESS_VM_KIND)task_process_vm_scope_release(path->name);
    if(path->name && path->flags==TASK_LOOKUP_PRLIMIT_IDENTITY_KIND) {
        sched_task_identity_put((struct task_identity *)path->name);path->name=NULL;
    }
#endif
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
#ifdef CONFIG_SWAP
    /* A canceled pselect owns its temporary mask until completion or signal. */
    if(old.number==__NR_pselect6 && task->sigsuspend_active) {
        task->blocked_signals=task->sigsuspend_saved_mask;
        task->sigsuspend_active=0;
    }
#endif
#ifdef CONFIG_SWAP
    if(memory_wait_cancel &&
       (old.number==__NR_mincore ||
        ((old.number==__NR_open || old.number==__NR_openat) &&
         (old.paths[0].flags==TASK_LOOKUP_PATH_IMPORT_KIND ||
          (old.paths[0].flags&TASK_LOOKUP_OPEN_WALK_RECLAIM_FLAG)) &&
         old.paths[0].root.mount && old.paths[0].start.mount)))
        memory_wait_cancel(task);
#endif
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
    bool scoped=lookup_path_call(frame->rax);
#ifdef CONFIG_SWAP
    /* Special-device vectors retain the admitted metadata across a
     * prerequisite page-in wait, just as ordinary files and sockets do. */
    if(frame->rax==LINUX_SYS_READV || frame->rax==LINUX_SYS_WRITEV ||
       frame->rax==LINUX_SYS_PREADV || frame->rax==LINUX_SYS_PWRITEV ||
       frame->rax==LINUX_SYS_PREADV2 || frame->rax==LINUX_SYS_PWRITEV2)scoped=true;
    /* UNIX writes can atomically publish a bounded packet after a wait. */
    if((frame->rax==LINUX_SYS_WRITE || frame->rax==LINUX_SYS_READ ||
        frame->rax==LINUX_SYS_WRITEV || frame->rax==LINUX_SYS_READV) && task->syscall_file &&
       (task->syscall_file->flags & TASK_FILE_FLAG_SOCKET_UNIX))scoped=true;
    if ((frame->rax==LINUX_SYS_READV || frame->rax==LINUX_SYS_PREADV ||
         frame->rax==LINUX_SYS_PREADV2) && task->syscall_file &&
        task->syscall_file->character_file && task->syscall_file->drm_file) scoped=true;
    if(frame->rax==LINUX_SYS_IOCTL && ((uint32_t)frame->rsi==RELIEFOS_FBIOBLIT ||
       (uint32_t)frame->rsi==FBIOGETCMAP || (uint32_t)frame->rsi==FBIOPUTCMAP ||
       (uint32_t)frame->rsi==BLKGETSIZE || (uint32_t)frame->rsi==BLKGETSIZE64))scoped=true;
    if(frame->rax==LINUX_SYS_IOCTL && task->syscall_file &&
       task->syscall_file->character_file &&
       (task->syscall_file->drm_file || task->syscall_file->drm_prime_file))scoped=true;
    if(frame->rax==LINUX_SYS_IOCTL && task->syscall_file &&
       task->syscall_file->character_file &&
       (task->syscall_file->audio_file || task->syscall_file->audio_control_file ||
        task->syscall_file->audio_mixer_file || task->syscall_file->audio_timer_file ||
        task->syscall_file->audio_oss_file))scoped=true;
    if(frame->rax==LINUX_SYS_IOCTL && task->syscall_file &&
       (task->syscall_file->flags & TASK_FILE_FLAG_SOCKET_INET))scoped=true;
#endif
    if(!scoped){task_lookup_scope_cancel(task);return;}
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
        *link=kernel_malloc(sizeof(**link));
        if(!*link){return -RELIEFOS_ENOMEM;}
        **link=(struct task_lookup_extra){0};
    }
    *out=&(*link)->path;return 0;
}

/** @brief Borrow an already allocated operand slot without changing its cursor.
 * @param task Execution-owned task. @param index Zero-based scoped slot.
 * @return Borrowed slot, or NULL if it was not allocated or no scope exists.
 * No allocation, user access, reference transfer or I/O occurs.
 */
struct task_lookup_path *task_lookup_scope_at(struct task *task,uint64_t index)
{
    if(!task || !task->lookup_scope.armed)return NULL;
    if(index<2)return &task->lookup_scope.paths[index];
    struct task_lookup_extra *entry=task->lookup_scope.extra;
    for(index-=2;entry && index;--index)entry=entry->next;
    return entry?&entry->path:NULL;
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
    const uint32_t guard_kind=TASK_LOOKUP_EXEC_GUARD_KIND;
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

#ifdef CONFIG_SWAP
/** @brief Pin open's root/cwd before an allocation-free pressure wait.
 * @param task Current scoped open caller. @param saved Its first lookup slot.
 * @param user_ptr Original pathname operand. @param kind Expected import kind.
 * @return True only for open or openat(AT_FDCWD) before any pathname copy,
 * lookup, provider callback or side effect. Existing paths remain task-owned
 * across waits and are released by common lookup cancellation.
 */
static bool open_import_pin_paths(struct task *task,struct task_lookup_path *saved,
                                  uint64_t user_ptr,uint32_t kind)
{
    if(!task || task!=sched_current_task() || !saved ||
       kind!=TASK_LOOKUP_PATH_IMPORT_KIND || !task->lookup_scope.armed ||
       saved!=&task->lookup_scope.paths[0] || saved->name || saved->walk ||
       user_ptr<RELIEFNT_USER_BASE || user_ptr>=RELIEFNT_USER_TOP)
        return false;
    struct task_lookup_scope *scope=&task->lookup_scope;
    if(scope->number==__NR_open) {
        if(scope->args[0]!=user_ptr)return false;
    } else if(scope->number==__NR_openat) {
        if((int32_t)scope->args[0]!=LINUX_AT_FDCWD || scope->args[1]!=user_ptr)return false;
    } else return false;
    if(saved->root.mount || saved->start.mount)
        return saved->flags==kind && saved->root.mount && saved->start.mount;
    struct task_fs_paths *paths=sched_task_paths(task);
    if(!vfs_path_node(&paths->root) || !vfs_path_node(&paths->cwd))return false;
    vfs_path_get(&paths->root,&saved->root);vfs_path_get(&paths->cwd,&saved->start);
    saved->flags=kind;saved->dirfd=LINUX_AT_FDCWD;
    return saved->root.mount && saved->start.mount;
}
#endif

#ifdef CONFIG_SWAP
/** @brief Reclaim before an open's import, initial walk or native workspace.
 * @param task Current execution-owned task. @param frame Original registers.
 * @return Zero for retry, blocked marker, or terminal ENOMEM. For a retained
 * walk the current native lookup has not submitted I/O. Completed earlier
 * components, text and root/cwd remain scoped and are never replayed.
 */
int64_t syscall_open_path_reclaim(struct task *task,const struct trap_frame *frame)
{
    if(!task || task!=sched_current_task() || !frame || frame->rip<2 ||
       (frame->rax!=LINUX_SYS_OPEN && frame->rax!=LINUX_SYS_OPENAT) ||
       !kernel_execution_is_outermost() || !task->lookup_scope.armed ||
       task->lookup_scope.number!=frame->rax)
        {return -RELIEFOS_ENOMEM;}
    const uint64_t args[]={frame->rdi,frame->rsi,frame->rdx,frame->r10,frame->r8,frame->r9};
    struct task_lookup_path *slot=task_lookup_scope_at(task,0);
    if(__builtin_memcmp(task->lookup_scope.args,args,sizeof(args)) || !slot ||
       !slot->root.mount || !slot->start.mount)
        {return -RELIEFOS_ENOMEM;}
    bool importing=slot->flags==TASK_LOOKUP_PATH_IMPORT_KIND && !slot->name && !slot->walk;
    bool walking=(slot->flags&TASK_LOOKUP_OPEN_WALK_RECLAIM_FLAG) && slot->name &&
        !(slot->flags&~(TASK_LOOKUP_OPEN_WALK_RECLAIM_FLAG|VFS_LOOKUP_FOLLOW|VFS_LOOKUP_MISSING)) &&
        !slot->complete && !slot->allow_empty && !slot->real_ids && slot->dirfd==LINUX_AT_FDCWD &&
        (frame->rax==LINUX_SYS_OPEN || (int32_t)frame->rdi==LINUX_AT_FDCWD);
    if(!importing && !walking){return -RELIEFOS_ENOMEM;}
    uint64_t pathname=frame->rax==LINUX_SYS_OPEN ? frame->rdi : frame->rsi;
    memory_reclaim_fault_scope_begin(task,frame->rip-2,pathname);
    int rc=memory_fault_oom(task,pathname);
    memory_reclaim_fault_scope_end();
    if(rc==-EAGAIN)return KERNEL_SYSCALL_BLOCKED;
    if(rc){
        return -RELIEFOS_ENOMEM;
    }
    task_lookup_scope_begin(task,frame);
    return 0;
}
#endif

/** @brief Resume one bounded pathname or text import from owned bytes.
 * @param task Borrowed caller under execution ownership.
 * @param saved Optional owner slot, retained through a prerequisite wait.
 * @param user_ptr User string address checked through its terminating NUL.
 * @param maximum Buffer capacity including NUL, at most VFS_PATH_MAX.
 * @param allow_null Treat a NULL address as an empty string when true.
 * @param kind Private discriminator for the saved operand.
 * @param out Required output, borrowing scoped text or owning allocation for
 * an unscoped call. Caller kernel_free(out->allocation) on every outcome.
 * @return Zero or negative errno, with empty output on failure. Scoped copies
 * belong to the task until common cancellation and matching retries do not
 * reread user memory. No inode I/O, reference transfer or spinlock occurs.
 */
int task_lookup_text_resume(struct task *task,struct task_lookup_path *saved,
                            uint64_t user_ptr,uint32_t maximum,bool allow_null,
                            uint32_t kind,struct task_user_text *out)
{
    if(!out)return -RELIEFOS_EINVAL;*out=(struct task_user_text){0};
    if(!maximum || maximum>VFS_PATH_MAX)return -RELIEFOS_EINVAL;
    if(kind!=TASK_LOOKUP_TEXT_IMPORT_KIND && kind!=TASK_LOOKUP_PATH_IMPORT_KIND)return -RELIEFOS_EINVAL;
    struct lookup_text_snapshot *snapshot;
    if(saved && saved->name) {
        if(saved->flags!=kind)return -RELIEFOS_EINVAL;
        snapshot=(struct lookup_text_snapshot *)saved->name;
        if(snapshot->user_ptr!=user_ptr || snapshot->maximum!=maximum ||
           snapshot->allow_null!=allow_null || snapshot->done>maximum)
            return -RELIEFOS_EINVAL;
    } else {
        snapshot=kernel_malloc(sizeof(*snapshot)+(size_t)maximum);
        if(!snapshot) {

#ifdef CONFIG_SWAP
            if(open_import_pin_paths(task,saved,user_ptr,kind)) {

                return KERNEL_SYSCALL_OPEN_PATH_RECLAIM;
            }
#endif
            return -RELIEFOS_ENOMEM;
        }
        *snapshot=(struct lookup_text_snapshot){.user_ptr=user_ptr,.maximum=maximum,.allow_null=allow_null};
        if(saved){saved->name=(char *)snapshot;saved->flags=kind;}
    }
    if(!user_ptr && allow_null){snapshot->bytes[0]=0;snapshot->complete=true;}
    int rc=0;
    while(!snapshot->complete && snapshot->done<maximum) {
        uint32_t done=snapshot->done;
        if(user_ptr>UINT64_MAX-done){rc=-RELIEFOS_EFAULT;break;}
        uint64_t address=user_ptr+done;
        uint32_t chunk=4096u-(uint32_t)(address&4095u);
        if(chunk>maximum-done)chunk=maximum-done;
        int prep=swap_prepare_user_range(task,address,chunk,false);
        if(prep || !user_range_ok(address,chunk)) {
            rc=prep==SWAP_STORAGE_CONTENDED ? KERNEL_SYSCALL_SWAP_CONTENDED : -RELIEFOS_EFAULT;
            break;
        }
        for(uint32_t i=0;i<chunk;++i) {
            char ch=((const char *)(uintptr_t)address)[i];
            snapshot->bytes[snapshot->done++]=ch;
            if(!ch){snapshot->complete=true;break;}
        }
    }
    if(!rc && !snapshot->complete)rc=-RELIEFOS_ENAMETOOLONG;
    if(rc) {
        if(rc!=KERNEL_SYSCALL_SWAP_CONTENDED || !saved) {
            if(saved)saved->name=NULL;
            kernel_free(snapshot);
        }
        return rc;
    }
    if(!saved)out->allocation=(char *)snapshot;
    out->text=snapshot->bytes;return 0;
}

/** @brief Capture one bounded text operand in the task's retry scope.
 * @param task Current caller. @param user_ptr User string or permitted NULL.
 * @param maximum Capacity including NUL. @param allow_null Permit NULL.
 * @param out Borrowed scoped text or owned unscoped allocation.
 * @return Zero, terminal errno or prerequisite contention.
 */
int task_lookup_user_text(struct task *task,uint64_t user_ptr,uint32_t maximum,
                            bool allow_null,struct task_user_text *out)
{
    struct task_lookup_path *saved=NULL;int rc=task_lookup_scope_next(task,&saved);
    return rc ? rc : task_lookup_text_resume(task,saved,user_ptr,maximum,allow_null,
                                             TASK_LOOKUP_TEXT_IMPORT_KIND,out);
}

/** @brief Capture exact binary user bytes in this syscall's operand scope.
 * @param task Borrowed caller with optional armed retry scope.
 * @param user_ptr User address checked for the entire counted range.
 * @param length Count from one through INT32_MAX, including embedded NUL.
 * Callers check their own ABI limits before requesting a snapshot.
 * @param out Required output; scoped bytes are borrowed, unscoped allocation
 * is owned and must be kernel_free'd by the caller on every return.
 * @return Zero or negative errno with empty output on failure. Matching
 * retries resume after the copied prefix and never reread it, even when the
 * caller changes or unmaps those pages. Common cancellation owns scoped
 * storage; no namespace lookup, inode I/O or spinlock participates.
 */
int task_lookup_user_bytes(struct task *task,uint64_t user_ptr,uint32_t length,struct task_user_text *out)
{
    if(!out)return -RELIEFOS_EINVAL;*out=(struct task_user_text){0};
    if(!length || length>INT32_MAX)return -RELIEFOS_EINVAL;
    const uint32_t binary_kind=TASK_LOOKUP_BINARY_IMPORT_KIND;
    struct task_lookup_path *saved=NULL;int rc=task_lookup_scope_next(task,&saved);if(rc)return rc;
    if(user_ptr>UINT64_MAX-length)return -RELIEFOS_EFAULT;
    struct lookup_bytes_snapshot *snapshot;
    if(saved && saved->name) {
        if(saved->flags!=binary_kind || saved->dirfd!=(int32_t)length)return -RELIEFOS_EINVAL;
        snapshot=(struct lookup_bytes_snapshot *)saved->name;
        if(snapshot->user_ptr!=user_ptr || snapshot->length!=length || snapshot->done>length)
            return -RELIEFOS_EINVAL;
    } else {
        snapshot=kernel_malloc(sizeof(*snapshot)+(size_t)length);
        if(!snapshot)return -RELIEFOS_ENOMEM;
        *snapshot=(struct lookup_bytes_snapshot){.user_ptr=user_ptr,.length=length};
        if(saved) {
            saved->name=(char *)snapshot;saved->flags=binary_kind;saved->dirfd=(int32_t)length;
        }
    }
    while(snapshot->done<length) {
        uint32_t done=snapshot->done,chunk=length-done;if(chunk>32768u)chunk=32768u;
        int prep=swap_prepare_user_range(task,user_ptr+done,chunk,false);
        if(prep || !user_range_ok(user_ptr+done,chunk)) {
            if(prep!=SWAP_STORAGE_CONTENDED || !saved) {
                if(saved)saved->name=NULL;
                kernel_free(snapshot);
            }
            return prep==SWAP_STORAGE_CONTENDED ? KERNEL_SYSCALL_SWAP_CONTENDED : -RELIEFOS_EFAULT;
        }
        __builtin_memcpy(snapshot->bytes+done,(const void *)(uintptr_t)(user_ptr+done),chunk);
        snapshot->done+=chunk;
    }
    if(!saved)out->allocation=(char *)snapshot;
    out->text=snapshot->bytes;return 0;
}
#ifdef CONFIG_SWAP
EXPORT_SYMBOL(task_lookup_user_bytes);
#endif
