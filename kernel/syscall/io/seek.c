/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"

/** @brief Seek one admitted ordinary inode using its shared OFD position.
 * @param task Execution-owned caller retaining the admitted OFD until cleanup.
 * @param fd Signed low-32-bit descriptor, resolved through syscall admission.
 * @param offset Signed Linux byte offset or opaque directory position cookie.
 * @param whence Linux SEEK_SET/CUR/END/DATA/HOLE selector, low 32 bits.
 * @return New position, negative errno or internal metadata retry. Directories
 * preserve backend cookies without record-size conversion. Ordinary END/DATA/
 * HOLE use fresh actual-provider metadata, with the conservative permitted
 * all-data/EOF-hole interpretation. The OFD position is reserved before any
 * callback and released by common syscall cleanup; metadata wait cancellation
 * drains I/O before releasing the provider. No graph/module lock spans I/O.
 */
int64_t syscall_seek_object(struct task *task,int32_t fd,int64_t offset,uint32_t whence)
{
    struct task_file *file=task_file_for_io(task,fd);
    if(!task || !file || (file->flags&TASK_FILE_FLAG_PATH) || task->syscall_file!=file ||
       task->syscall_file_number!=LINUX_SYS_LSEEK)return -RELIEFOS_EBADF;
    bool directory=file->node.type==RELIEFOS_FS_TYPE_DIR;
    if(!directory && file->node.type!=RELIEFOS_FS_TYPE_FILE)return -LINUX_ESPIPE;
    if(whence>4 || (directory && whence>RELIEFOS_SEEK_CUR))return -RELIEFOS_EINVAL;
    if(file->io_owner && file->io_owner!=task->pid)return -RELIEFOS_EAGAIN;
    file->io_owner=task->pid;
    int64_t base=whence==RELIEFOS_SEEK_CUR ? (int64_t)file->offset : 0;
    if(whence>=RELIEFOS_SEEK_END) {
        struct linux_stat_abi status;
        task->regular_io.active=true;
        storage_set_io_async_context(false);
        int rc=vfs_path_stat(&file->path_ref,&status);
        if(rc==-RELIEFOS_EAGAIN || rc==KERNEL_SYSCALL_BLOCKED)return rc;
        task->regular_io.active=false;
        if(rc)return rc;
        if(status.st_size<0)return -LINUX_EIO;
        if(whence==RELIEFOS_SEEK_END)base=status.st_size;
        else {
            if(offset<0)return -RELIEFOS_EINVAL;
            if(offset>=status.st_size)return -LINUX_ENXIO;
            if(whence==4)offset=status.st_size;
        }
    }
    int64_t position;
    if(__builtin_add_overflow(base,offset,&position) || position<0)return -RELIEFOS_EINVAL;
    file->offset=(uint64_t)position;file->read_cursor.valid=0;
    if(directory)file->aux=0;
    return position;
}
