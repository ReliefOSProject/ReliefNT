/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"
#include <reliefnt/swap.h>

/** @brief Borrow this operation's private, cancellation-owned result slot.
 * @param task Matching syscall owner. @param kind Expected result class.
 * @param slot Receives the borrowed slot, or NULL without a retry scope.
 * @return Zero or negative errno. No provider operation or I/O occurs here.
 */
static int metadata_result_slot(struct task *task, uint32_t kind,
                                struct task_lookup_path **slot)
{
#ifdef CONFIG_SWAP
    int rc = task_lookup_scope_next(task, slot);
    if (rc) return rc;
    if (*slot && (*slot)->name && (*slot)->flags != kind) return -RELIEFOS_EINVAL;
#else
    (void)task; (void)kind; *slot=NULL;
#endif
    return 0;
}

/** @brief Prepare a completed result's bounded output, retaining it on contention.
 * @param task Current syscall owner. @param address Output user address.
 * @param output_size Actual output bytes, never the caller's unused capacity.
 * @param slot Borrowed scope slot or NULL. @param kind Exact result class.
 * @param data Completed bytes. @param data_size Bytes to retain for retry.
 * @param owned Optional transferable allocation equal to data.
 * @return Zero, the private contention marker, or ENOMEM. Other preparation
 * errors leave the original late user-range validation to report EFAULT.
 * Caller first completes its own provider request and releases that completed
 * reservation; a pending request must never reach this function. The scope
 * owns retained bytes until final return, cancellation or exit.
 */
static int metadata_prepare_result(struct task *task, uint64_t address, size_t output_size,
                                   struct task_lookup_path *slot, uint32_t kind,
                                   const void *data, size_t data_size, char **owned)
{
    int rc = swap_prepare_user_range(task, address, output_size, true);
    if (rc != SWAP_STORAGE_CONTENDED) return 0;
    if (slot && !slot->name) {
        char *saved = owned && *owned == data ? *owned : kernel_malloc(data_size ? data_size : 1);
        if (!saved) return -RELIEFOS_ENOMEM;
        if (!owned || saved != *owned) __builtin_memcpy(saved, data, data_size);
        else *owned = NULL;
        slot->flags = kind;
        slot->dirfd = (int32_t)data_size;
        slot->name = saved;
    }
    return KERNEL_SYSCALL_SWAP_CONTENDED;
}

/** @brief Read metadata from the admitted OFD or a builtin terminal endpoint.
 * @param task Borrowed task with dispatcher-owned descriptor admission.
 * @param fd Descriptor number; a suspended call keeps its original OFD.
 * @param out Required result, owned by the caller.
 * @return Zero or negative errno. Held paths retain their providers across
 * metadata I/O; anonymous descriptions never resolve a diagnostic pathname.
 * No graph/descriptor spinlock spans a callback.
 */
static int metadata_fd_stat(struct task *task,int fd,struct linux_stat_abi *out)
{
    struct task_file *file=task_file_for_io(task,fd);
    if(file) {
        int rc;
        if(vfs_path_node(&file->path_ref))rc=linux_stat_from_path(out,&file->path_ref);
        else {
            struct reliefos_stat basic={.type=file->node.type,.size=file->node.size};
            rc=linux_stat_from_node(out,&basic,&file->node);
        }
        if(!rc)linux_stat_fd_type(out,file);
        return rc;
    }
    struct task_pty_fd *endpoint=task_pty_for_io(task,fd);
    if(endpoint && endpoint->description && vfs_path_node(&endpoint->description->path_ref))
        return linux_stat_from_path(out,&endpoint->description->path_ref);
    struct storage_node node;const char *intrinsic;
    int rc=task_pty_node_for_fd(task,fd,&node,&intrinsic);
    if(rc) {
        if(fd<0 || fd>=3 || (sched_task_fds(task)->closed_stdio_mask&(1u<<fd)))return -RELIEFOS_EBADF;
        node=(struct storage_node){.type=RELIEFOS_FS_TYPE_DEVICE,
            .flags=STORAGE_NODE_FLAG_DEV_NODE,.first_cluster=STORAGE_DEV_KIND_CONSOLE};
        intrinsic="/dev/console";
    }
    struct reliefos_stat basic={.type=node.type,.size=node.size};
    rc=linux_stat_from_node(out,&basic,&node);
    struct reliefos_permissions mode;
    if(!rc)rc=fs_virtual_permissions(intrinsic,&node,&mode,false);
    if(!rc){out->st_mode=(out->st_mode&LINUX_S_IFMT)|(mode.mode&07777);out->st_uid=mode.uid;out->st_gid=mode.gid;}
    return rc;
}

/** @brief Obtain filesystem statistics from an admitted description.
 * @param task Borrowed current task under execution ownership.
 * @param fd Descriptor number with original OFD admission where applicable.
 * @param out Required caller-owned output.
 * @return Zero or negative errno; provider I/O runs with the path retained.
 * Anonymous pipe/socket/terminal metadata is constructed without name lookup.
 */
static int metadata_fd_statfs(struct task *task,int fd,struct linux_statfs_abi *out)
{
    *out=(struct linux_statfs_abi){0};
    struct task_file *file=task_file_for_io(task,fd);
    if(file) {
        if(vfs_path_node(&file->path_ref))return vfs_path_statfs(&file->path_ref,out);
        if(file->flags&TASK_FILE_FLAG_PIPE)out->f_type=0x50495045;
        else if(file->flags&(TASK_FILE_FLAG_SOCKET_UNIX|TASK_FILE_FLAG_SOCKET_INET))out->f_type=0x534f434b;
        else return -RELIEFOS_EOPNOTSUPP;
    } else {
        struct task_pty_fd *endpoint=task_pty_for_io(task,fd);
        if(endpoint && endpoint->description && vfs_path_node(&endpoint->description->path_ref))
            return vfs_path_statfs(&endpoint->description->path_ref,out);
        if(!endpoint && task_pty_stream_for_fd(task,fd)<0)return -RELIEFOS_EBADF;
        out->f_type=0x1cd1;
    }
    out->f_bsize=out->f_frsize=4096;out->f_namelen=255;out->f_flags=LINUX_ST_VALID;
    return 0;
}

/** @brief Obtain stat metadata for one pathname or empty-path descriptor.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed calling task with retained filesystem state.
 * @param dirfd Base descriptor or AT_FDCWD.
 * @param pathname Retained kernel snapshot, including any allowed empty name.
 * @param flags Validated AT_SYMLINK_NOFOLLOW/AT_EMPTY_PATH policy.
 * @param out Caller-owned stat result.
 * @param slot Receives the optional retry-owned result slot.
 * @return Zero or negative errno. The common retry scope keeps the completed
 * operand across metadata waits. Pathless descriptors have no yielding name
 * operation and are inspected directly, never via proc display strings.
 */
static int metadata_at_stat(struct vfs_namespace *ns,struct task *task,int32_t dirfd,
                              const char *pathname,uint32_t flags,struct linux_stat_abi *out,
                              struct task_lookup_path **slot)
{
    struct vfs_lookup_result found={0};
    bool empty=(flags&LINUX_AT_EMPTY_PATH)!=0;
    uint32_t follow=flags&LINUX_AT_SYMLINK_NOFOLLOW ? 0 : VFS_LOOKUP_FOLLOW;
    int rc=resolve_kernel_object_at_flags(ns,task,dirfd,pathname,empty,false,follow,&found);
    bool fallback=rc==-RELIEFOS_EBADF && empty && dirfd!=LINUX_AT_FDCWD && !pathname[0];
    if(!rc || fallback) {
        rc=metadata_result_slot(task,METADATA_RESULT_STAT,slot);
        if(!rc && *slot && (*slot)->name) {
            if((*slot)->dirfd!=(int32_t)sizeof(*out))rc=-RELIEFOS_EINVAL;
            else __builtin_memcpy(out,(*slot)->name,sizeof(*out));
        } else if(!rc) {
            rc=fallback ? metadata_fd_stat(task,dirfd,out) : linux_stat_from_path(out,&found.path);
            if(!rc)storage_release_task_io(task->pid);
        }
    }
    vfs_path_put(&found.path);return rc;
}

/** @brief Dispatch Linux stat/statfs operations against retained objects.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed calling task; required.
 * @param number STAT/LSTAT/FSTAT/NEWFSTATAT/STATX/STATFS/FSTATFS.
 * @param a0 Path address or descriptor, according to the selected ABI.
 * @param a1 Output address or at-path address.
 * @param a2 At-family output address or statx flags.
 * @param a3 NEWFSTATAT flags or statx requested mask.
 * @param a4 Statx output address.
 * @return Zero or negative errno. Outputs are copied only after successful
 * metadata retrieval and writable-range validation. Provider waits retain the
 * original inode/OFD; no graph/module spinlock spans I/O.
 */
int64_t syscall_metadata(struct vfs_namespace *ns,struct task *task,uint64_t number,
                           uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4)
{
    if(!task)return -RELIEFOS_ESRCH;
    if(number==LINUX_SYS_STATFS || number==LINUX_SYS_FSTATFS) {
        const uint32_t result_kind=METADATA_RESULT_STATFS;
        struct linux_statfs_abi value;struct task_lookup_path *slot=NULL;
        struct task_user_text text={0};struct vfs_lookup_result found={0};int rc=0;
        if(number==LINUX_SYS_STATFS) {
            rc=task_lookup_user_text(task,a0,VFS_PATH_MAX,false,&text);
            if(!rc)rc=resolve_kernel_object_at_flags(ns,task,LINUX_AT_FDCWD,text.text,
                                                      false,false,VFS_LOOKUP_FOLLOW,&found);
        }
        if(!rc)rc=metadata_result_slot(task,result_kind,&slot);
        if(!rc && slot && slot->name) {
            if(slot->dirfd!=(int32_t)sizeof(value))rc=-RELIEFOS_EINVAL;
            else __builtin_memcpy(&value,slot->name,sizeof(value));
        } else if(!rc) {
            rc=number==LINUX_SYS_FSTATFS ? metadata_fd_statfs(task,(int32_t)a0,&value) :
                                          vfs_path_statfs(&found.path,&value);
            if(!rc)storage_release_task_io(task->pid);
        }
        vfs_path_put(&found.path);kernel_free(text.allocation);
        if(rc)return rc;
        rc=metadata_prepare_result(task,a1,sizeof(value),slot,result_kind,&value,sizeof(value),NULL);
        if(rc)return rc;
        if(!user_range_writable(a1,sizeof(value)))return -RELIEFOS_EFAULT;
        *(struct linux_statfs_abi *)(uintptr_t)a1=value;return 0;
    }
    const uint32_t result_kind=METADATA_RESULT_STAT;
    struct linux_stat_abi value;struct task_lookup_path *slot=NULL;uint64_t output=a1;int rc;
    if(number==LINUX_SYS_FSTAT) {
        rc=metadata_result_slot(task,result_kind,&slot);
        if(!rc && slot && slot->name) {
            if(slot->dirfd!=(int32_t)sizeof(value))rc=-RELIEFOS_EINVAL;
            else __builtin_memcpy(&value,slot->name,sizeof(value));
        } else if(!rc) {
            rc=metadata_fd_stat(task,(int32_t)a0,&value);
            if(!rc)storage_release_task_io(task->pid);
        }
    }
    else {
        uint32_t flags=number==LINUX_SYS_STATX ? (uint32_t)a2 :
            number==LINUX_SYS_NEWFSTATAT ? (uint32_t)a3 : number==LINUX_SYS_LSTAT ? LINUX_AT_SYMLINK_NOFOLLOW : 0;
        uint32_t supported=LINUX_AT_SYMLINK_NOFOLLOW|LINUX_AT_EMPTY_PATH|LINUX_AT_NO_AUTOMOUNT;
        if(number==LINUX_SYS_STATX) {
            supported|=LINUX_AT_STATX_SYNC_TYPE;
            if(((uint32_t)a3&0x80000000u) || (flags&LINUX_AT_STATX_SYNC_TYPE)==LINUX_AT_STATX_SYNC_TYPE)
                return -RELIEFOS_EINVAL;
        } else if(number!=LINUX_SYS_STAT && number!=LINUX_SYS_LSTAT && number!=LINUX_SYS_NEWFSTATAT)
            return -RELIEFOS_ENOSYS;
        if(flags&~supported)return -RELIEFOS_EINVAL;
        bool at=number==LINUX_SYS_NEWFSTATAT || number==LINUX_SYS_STATX;
        struct task_user_text text={0};
        rc=task_lookup_user_text(task,at ? a1 : a0,VFS_PATH_MAX,
                                 (flags&LINUX_AT_EMPTY_PATH)!=0,&text);
        if(!rc)rc=metadata_at_stat(ns,task,at ? (int32_t)a0 : LINUX_AT_FDCWD,
                                    text.text,flags,&value,&slot);
        kernel_free(text.allocation);
        if(at)output=number==LINUX_SYS_STATX ? a4 : a2;
    }
    if(rc)return rc;
    if(number==LINUX_SYS_STATX) {
        struct linux_statx extended;
        linux_statx_from_legacy(&extended,&value,LINUX_STATX_BASIC_STATS);
        rc=metadata_prepare_result(task,output,sizeof(extended),slot,result_kind,&value,sizeof(value),NULL);
        if(rc)return rc;
        if(!user_range_writable(output,sizeof(extended)))return -RELIEFOS_EFAULT;
        *(struct linux_statx *)(uintptr_t)output=extended;
    } else {
        rc=metadata_prepare_result(task,output,sizeof(value),slot,result_kind,&value,sizeof(value),NULL);
        if(rc)return rc;
        if(!user_range_writable(output,sizeof(value)))return -RELIEFOS_EFAULT;
        *(struct linux_stat_abi *)(uintptr_t)output=value;
    }
    return 0;
}

/** @brief Read a symlink's bytes from the exact retained link inode.
 * @param ns Borrowed shared namespace under execution ownership.
 * @param task Borrowed calling task with retained root/cwd.
 * @param dirfd Base descriptor or AT_FDCWD; empty input may select a link OFD.
 * @param pathname User pathname, copied once by the syscall lookup scope.
 * @param output User byte buffer; no terminating NUL is appended.
 * @param capacity Native Linux signed-int buffer capacity.
 * @param allow_empty True only for readlinkat, including O_PATH symlink FDs.
 * @return Bytes copied or negative errno. A completed bounded result may be
 * transferred to the common scope until output-page contention clears; the
 * same scope owns the original name and link across provider waits. No
 * graph/module lock spans the readlink callback or page-in.
 */
int64_t syscall_readlink_object(struct vfs_namespace *ns,struct task *task,int32_t dirfd,
                                  uint64_t pathname,uint64_t output,int32_t capacity,bool allow_empty)
{
    if(capacity<=0)return -RELIEFOS_EINVAL;
    const uint32_t result_kind = METADATA_RESULT_READLINK;
    struct task_user_text text={0};
    struct vfs_lookup_result found={0};
    struct task_lookup_path *slot=NULL;
    int rc=task_lookup_user_text(task,pathname,VFS_PATH_MAX,false,&text);
    if(!rc)rc=resolve_kernel_object_at_flags(ns,task,dirfd,text.text,allow_empty,false,0,&found);
    if(!rc)rc=metadata_result_slot(task,result_kind,&slot);
    char *buffer=NULL;const char *result=NULL;size_t length=0;
    if(!rc && slot && slot->name) {
        if(slot->dirfd<0 || (uint32_t)slot->dirfd>VFS_PATH_MAX)rc=-RELIEFOS_EINVAL;
        else {result=slot->name;length=(uint32_t)slot->dirfd;}
    } else if(!rc) {
        buffer=kernel_malloc(VFS_PATH_MAX);
        if(!buffer)rc=-RELIEFOS_ENOMEM;
        else {
            rc=vfs_path_readlink(&found.path,buffer,VFS_PATH_MAX,&length);
            if(!rc) {
                /* The callback completed this operation; no pending provider
                 * command may be discarded when freeing its reservation. */
                storage_release_task_io(task->pid);
                result=buffer;
            }
        }
    }
    if(!rc) {
        if(length>(uint32_t)capacity)length=(uint32_t)capacity;
        rc=metadata_prepare_result(task,output,length,slot,result_kind,result,length,&buffer);
        if(!rc) {
            if(!user_range_writable(output,length))rc=-RELIEFOS_EFAULT;
            else {__builtin_memcpy((void *)(uintptr_t)output,result,length);rc=(int)length;}
        }
    }
    kernel_free(buffer);vfs_path_put(&found.path);kernel_free(text.allocation);return rc;
}
