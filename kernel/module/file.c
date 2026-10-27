/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/sched.h>
#include <reliefnt/storage.h>
#include <reliefnt/heap.h>
#include <linux/module.h>
#include <linux/fcntl.h>

#ifdef CONFIG_MODULES
/** @brief Retire a snapshot's independently owned inode and deny-write lease.
 * @param task Current/retiring task under the execution transaction, nullable.
 * Ownership is cleared before callbacks; final inode release may do backend I/O.
 * Repeated cancellation is harmless, including after a completed snapshot.
 */
void module_file_cancel(struct task *task)
{
    if(!task || !task->module_file_inode)return;
    struct storage_inode_ref *inode=task->module_file_inode;task->module_file_inode=NULL;
    storage_inode_allow_write(inode);(void)storage_inode_put(inode);
}
#endif

/** @brief Distinguish pre-init snapshot waits from the initializer's final errno.
 * @param error Backend I/O result before any module code has run.
 * @return Private retry token for storage EAGAIN, otherwise the original result.
 * Task context, no ownership changes; syscall entry consumes the token.
 */
static int module_file_io_error(int error)
{
    return error==-EAGAIN ? MODULE_FILE_IO_RETRY : error;
}

/** @brief Copy a leased regular file positionally into private image storage.
 * @param file Stable caller-owned readable OFD; its inode denies writes and
 * its actual path retains the filesystem provider through every callback.
 * @param out Receives a snapshot owned by the caller, NULL on failure.
 * @param length Receives snapshot byte length only on success.
 * @return Zero, Linux errno, or private MODULE_FILE_IO_RETRY before init.
 * Task context under execution ownership; no module-table lock spans I/O.
 * Kernel-buffer metadata/data I/O is synchronous so an abandoned retry never
 * leaves DMA referencing reclaimed image bytes. Positional reads use a local
 * cursor and never change the shared OFD offset or its sequential-read hint.
 */
static int module_read_snapshot(struct task_file *file,void **out,size_t *length)
{
    *out=NULL;*length=0;
    storage_set_io_async_context(false);
    struct linux_stat_abi status;
    int rc=vfs_path_stat(&file->path_ref,&status);
    if(rc) return module_file_io_error(rc);
    if(status.st_size<0)return -EIO;
    if(!status.st_size) return -EINVAL;
    if((uint64_t)status.st_size<sizeof(struct module_elf_header)) return -ENOEXEC;
    if(status.st_size>INT32_MAX) return -EFBIG; /* Linux module read buffer limit. */
    size_t size=(size_t)status.st_size;
    uint8_t *image=kernel_malloc(size);
    if(!image) return -ENOMEM;
    struct storage_read_cursor cursor={0};
    size_t position=0;
    while(position<size) {
        size_t remaining=size-position;
        uint32_t count=remaining>65536 ? 65536u : (uint32_t)remaining,got=0;
        rc=vfs_path_read_cursor(&file->path_ref,position,image+position,count,&got,&cursor);
        if(rc) {rc=module_file_io_error(rc);break;}
        if(!got || got>count) { rc=-EIO; break; }
        position+=got;
    }
    if(rc)kernel_free(image);else {*out=image;*length=size;}
    return rc;
}

/** @brief Load a private file snapshot while preserving Linux writer exclusion.
 * @param file Caller-pinned readable OFD; shared file offset is unchanged.
 * @param params Kernel-owned terminated parameters, NULL means empty.
 * @param flags Linux finit flags with explicit unsupported-mode errors.
 * @return Zero, negative Linux errno, or MODULE_FILE_IO_RETRY before init.
 * Task/execution context. A pending read owns its inode and deny-write lease
 * until retry/cancel; final reads release the lease before module init runs.
 * No module-table lock spans storage I/O, waiting or initializer execution.
 */
int module_load_file(struct task_file *file,const char *params,unsigned int flags)
{
    struct task *task=sched_current_task();
    int rc=module_file_validate(file,flags);
    if(rc){module_file_cancel(task);return rc;}
    if(!task)return -ESRCH;
    if(task->module_file_inode && task->module_file_inode!=file->inode)module_file_cancel(task);
    if(!task->module_file_inode) {
        rc=storage_inode_deny_write(file->inode);
        if(rc)return rc;
        storage_inode_retain(file->inode);task->module_file_inode=file->inode;
    }
    void *image=NULL;size_t size=0;
    rc=module_read_snapshot(file,&image,&size);
    if(rc!=MODULE_FILE_IO_RETRY)module_file_cancel(task);
    if(!rc)rc=module_load_image(image,size,params,0);
    kernel_free(image);return rc;
}

/** @brief Validate module read eligibility before promotion/allocation or usercopy.
 * @param file Borrowed OFD held stable by execution ownership or a caller pin.
 * @param flags Linux finit flags, including explicitly unsupported optional modes.
 * @return Zero or Linux errno. Pure task-context validation, no I/O, allocation,
 * callback, reference transfer, or global module lock.
 */
int module_file_validate(const struct task_file *file,unsigned int flags)
{
    if(flags&~(MODULE_INIT_IGNORE_MODVERSIONS|MODULE_INIT_IGNORE_VERMAGIC|MODULE_INIT_COMPRESSED_FILE))
        return -EINVAL;
    if(!file || (file->flags&TASK_FILE_FLAG_PATH) || (file->flags&LINUX_O_ACCMODE)==LINUX_O_WRONLY)
        return -EBADF;
    if(flags&(MODULE_INIT_IGNORE_MODVERSIONS|MODULE_INIT_IGNORE_VERMAGIC)) return -ENOEXEC;
    if(flags&MODULE_INIT_COMPRESSED_FILE) return -EOPNOTSUPP;
    if(file->node.type!=RELIEFOS_FS_TYPE_FILE || file->kind ||
       file->flags&(TASK_FILE_FLAG_PIPE|TASK_FILE_FLAG_SOCKET|TASK_FILE_FLAG_DEV_NULL|
                    TASK_FILE_FLAG_DEV_NODE|TASK_FILE_FLAG_DEV_BLOCK|TASK_FILE_FLAG_EVENTFD|
                    TASK_FILE_FLAG_EPOLL|TASK_FILE_FLAG_TIMERFD) || file->node.flags&STORAGE_NODE_FLAG_PROC)
        return -EINVAL;
    if((file->flags&LINUX_O_ACCMODE)==LINUX_O_RDWR) return -ETXTBSY;
    /* An untracked backend cannot promise a stable kernel snapshot yet. */
    if(!file->inode) return -EOPNOTSUPP;
    return 0;
}
