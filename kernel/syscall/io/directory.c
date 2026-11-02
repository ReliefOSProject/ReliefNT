/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"
#include <reliefnt/swap.h>

#ifdef CONFIG_SWAP
struct dirent_receipt {
    uint64_t next;
    uint16_t length, name_length, name_offset;
    bool named, ready;
    struct reliefos_dir_entry entry;
    uint8_t bytes[256];
};

/** @brief Return records through bounded completed-provider receipts.
 * @param task Current execution-owned task. @param number Directory ABI.
 * @param fd Admitted descriptor. @param buffer User output address.
 * @param capacity Declared byte capacity. @return Committed prefix, errno or
 * private wait. A completed record is owned by the scope until copied; its
 * cookie is committed only after output succeeds. No user page is held over a
 * pending provider request or scheduling boundary.
 */
static int64_t syscall_getdents_staged(struct task *task,uint64_t number,int32_t fd,
                                        uint64_t buffer,uint32_t capacity)
{
    if(number!=LINUX_SYS_GETDENTS && number!=LINUX_SYS_GETDENTS64)return -RELIEFOS_ENOSYS;
    struct task_file *file=task_file_for_io(task,fd);
    if(!file || (file->flags&TASK_FILE_FLAG_PATH) || !file_can_read(file))return -RELIEFOS_EBADF;
    if(file->node.type!=RELIEFOS_FS_TYPE_DIR)return -RELIEFOS_ENOTDIR;
    if(!task || task->syscall_file!=file || task->syscall_file_number!=number)return -RELIEFOS_EBADF;
    if(file->io_owner && file->io_owner!=task->pid)return -RELIEFOS_EAGAIN;
    if(capacity && (buffer<RELIEFNT_USER_BASE || buffer>=RELIEFNT_USER_TOP ||
       capacity>RELIEFNT_USER_TOP-buffer))return -RELIEFOS_EFAULT;
    struct task_lookup_path *slot=NULL;int rc=task_lookup_scope_next(task,&slot);if(rc)return rc;
    struct dirent_receipt local={0},*stage=&local;
    if(slot) {
        if(slot->name) {
            if(slot->flags!=TASK_LOOKUP_DIRENT_KIND)return -RELIEFOS_EINVAL;
            stage=(struct dirent_receipt *)slot->name;
        } else {
            stage=kernel_malloc(sizeof(*stage));if(!stage)return -RELIEFOS_ENOMEM;
            *stage=(struct dirent_receipt){0};slot->name=(char *)stage;slot->flags=TASK_LOOKUP_DIRENT_KIND;
        }
    }
    if(!task->regular_io.active) {
        task->regular_io.buffer=buffer;task->regular_io.count=capacity;
        task->regular_io.position=file->offset;task->regular_io.done=0;task->regular_io.active=true;
    }
    buffer=task->regular_io.buffer;capacity=(uint32_t)task->regular_io.count;
    uint32_t written=(uint32_t)task->regular_io.done;int error=0;
    file->io_owner=task->pid;
    storage_set_io_async_context(false);uint64_t started=time_work_counter_us();
    while(!error) {
        if(!stage->ready) {
            if(!stage->named) {
                uint64_t next=file->offset;
                rc=vfs_path_readdir(&file->path_ref,&next,&stage->entry);
                if(rc==-RELIEFOS_EAGAIN || rc==KERNEL_SYSCALL_BLOCKED)return rc;
                if(rc<0){error=rc;break;}
                if(!rc)break;
                uint32_t length=0;
                while(length<sizeof(stage->entry.name) && stage->entry.name[length])++length;
                if(!length || length==sizeof(stage->entry.name)){error=-LINUX_EIO;break;}
                bool wide=number==LINUX_SYS_GETDENTS64;
                uint32_t name_offset=wide ? (uint32_t)offsetof(struct linux_dirent64,d_name) : 18u;
                uint16_t record=(uint16_t)((name_offset+length+1+(wide ? 0 : 1)+7)&~7u);
                if(record>capacity-written){error=-RELIEFOS_EINVAL;break;}
                stage->next=next;stage->length=record;stage->name_length=(uint16_t)length;
                stage->name_offset=(uint16_t)name_offset;stage->named=true;
            }
            struct linux_stat_abi status;
            rc=vfs_path_dirent_stat(&file->path_ref,stage->entry.name,&status);
            if(rc==-RELIEFOS_EAGAIN || rc==KERNEL_SYSCALL_BLOCKED)return rc;
            if(rc==-RELIEFOS_ENOENT) {
                file->offset=stage->next;stage->named=false;
                if(time_work_counter_us()-started>=2000) {
                    storage_release_task_io(task->pid);return -RELIEFOS_EAGAIN;
                }
                continue;
            }
            if(rc){error=rc;break;}
            if(stage->length>sizeof(stage->bytes)){error=-LINUX_EIO;break;}
            __builtin_memset(stage->bytes,0,stage->length);
            __builtin_memcpy(stage->bytes,&status.st_ino,sizeof(status.st_ino));
            __builtin_memcpy(stage->bytes+8,&stage->next,sizeof(stage->next));
            __builtin_memcpy(stage->bytes+16,&stage->length,sizeof(stage->length));
            __builtin_memcpy(stage->bytes+stage->name_offset,stage->entry.name,stage->name_length+1);
            stage->bytes[number==LINUX_SYS_GETDENTS64 ? 18 : stage->length-1]=
                (uint8_t)((status.st_mode&LINUX_S_IFMT)>>12);
            stage->named=false;stage->ready=true;
            /* Both callbacks completed; do not page in while retaining their
             * same-PID transport reservation. */
            storage_release_task_io(task->pid);
        }
        rc=swap_prepare_user_range(task,buffer+written,stage->length,true);
        if(rc==SWAP_STORAGE_CONTENDED) {
            if(!written)return KERNEL_SYSCALL_SWAP_CONTENDED;
            break;
        }
        if(rc<0 || !user_range_writable(buffer+written,stage->length)){error=-RELIEFOS_EFAULT;break;}
        __builtin_memcpy((void *)(uintptr_t)(buffer+written),stage->bytes,stage->length);
        file->offset=stage->next;written+=stage->length;task->regular_io.done=written;stage->ready=false;
        if(time_work_counter_us()-started>=2000)return -RELIEFOS_EAGAIN;
    }
    task->regular_io.active=false;if(file->io_owner==task->pid)file->io_owner=0;
    return written ? (int64_t)written : error;
}
#endif

/** @brief Enumerate actual directory inodes with retry-stable accepted records.
 * @param task Execution-owned caller with the dispatcher-pinned directory OFD.
 * @param number Linux GETDENTS or GETDENTS64 selects the record layout.
 * @param fd Signed low-32-bit directory descriptor, resolved through admission.
 * @param buffer User destination; the original value survives metadata waits.
 * @param capacity Low-32-bit output capacity, captured once for this invocation.
 * @return Accepted bytes, zero at EOF, negative Linux errno or internal
 * EAGAIN/BLOCKED preserving the buffer prefix and cursor. Shared OFD position
 * remains reserved until completion/common cancellation. Record inode/type
 * comes from the same provider, never a diagnostic name or another mount.
 * An entry removed before metadata acquisition is skipped with its next cookie;
 * other provider errors preserve the unconsumed entry and surface normally.
 * Kernel metadata callbacks are synchronous; no graph/module spinlock spans
 * them. A completed-record fairness boundary retains legal syscall progress.
 */
int64_t syscall_getdents_object(struct task *task,uint64_t number,int32_t fd,uint64_t buffer,uint32_t capacity)
{
#ifdef CONFIG_SWAP
    return syscall_getdents_staged(task,number,fd,buffer,capacity);
#else
    if(number!=LINUX_SYS_GETDENTS && number!=LINUX_SYS_GETDENTS64)return -RELIEFOS_ENOSYS;
    struct task_file *file=task_file_for_io(task,fd);
    if(!file || (file->flags&TASK_FILE_FLAG_PATH) || !file_can_read(file))return -RELIEFOS_EBADF;
    if(file->node.type!=RELIEFOS_FS_TYPE_DIR)return -RELIEFOS_ENOTDIR;
    if(!task || task->syscall_file!=file || task->syscall_file_number!=number)return -RELIEFOS_EBADF;
    if(file->io_owner && file->io_owner!=task->pid)return -RELIEFOS_EAGAIN;
    if(!task->regular_io.active) {
        task->regular_io.buffer=buffer;task->regular_io.count=capacity;
        task->regular_io.position=file->offset;task->regular_io.done=0;task->regular_io.active=true;
    }
    buffer=task->regular_io.buffer;capacity=(uint32_t)task->regular_io.count;
    uint32_t written=(uint32_t)task->regular_io.done;int error=0;
    file->io_owner=task->pid;
    if(!user_range_writable(buffer,capacity))error=-RELIEFOS_EFAULT;
    storage_set_io_async_context(false);uint64_t started=time_work_counter_us();
    while(!error) {
        struct reliefos_dir_entry entry;uint64_t next=file->offset;
        int rc=vfs_path_readdir(&file->path_ref,&next,&entry);
        if(rc==-RELIEFOS_EAGAIN || rc==KERNEL_SYSCALL_BLOCKED)return rc;
        if(rc<0){error=rc;break;}
        if(!rc)break;
        uint32_t length=0;while(length<sizeof(entry.name) && entry.name[length])++length;
        if(!length || length==sizeof(entry.name)){error=-LINUX_EIO;break;}
        bool wide=number==LINUX_SYS_GETDENTS64;
        uint32_t name_offset=wide ? (uint32_t)offsetof(struct linux_dirent64,d_name) : 18u;
        uint16_t record=(uint16_t)((name_offset+length+1+(wide ? 0 : 1)+7)&~7u);
        if(record>capacity-written){error=-RELIEFOS_EINVAL;break;}
        struct linux_stat_abi status;
        rc=vfs_path_dirent_stat(&file->path_ref,entry.name,&status);
        if(rc==-RELIEFOS_EAGAIN || rc==KERNEL_SYSCALL_BLOCKED)return rc;
        if(rc==-RELIEFOS_ENOENT) {
            file->offset=next;
            if(time_work_counter_us()-started>=2000) {
                storage_release_task_io(task->pid);return -RELIEFOS_EAGAIN;
            }
            continue;
        }
        if(rc){error=rc;break;}
        uint8_t *destination=(uint8_t *)(uintptr_t)buffer+written;
        __builtin_memset(destination,0,record);
        __builtin_memcpy(destination,&status.st_ino,sizeof(status.st_ino));
        __builtin_memcpy(destination+8,&next,sizeof(next));
        __builtin_memcpy(destination+16,&record,sizeof(record));
        __builtin_memcpy(destination+name_offset,entry.name,length+1);
        destination[wide ? 18 : record-1]=(uint8_t)((status.st_mode&LINUX_S_IFMT)>>12);
        file->offset=next;written+=record;task->regular_io.done=written;
        if(time_work_counter_us()-started>=2000) {
            storage_release_task_io(task->pid);return -RELIEFOS_EAGAIN;
        }
    }
    task->regular_io.active=false;if(file->io_owner==task->pid)file->io_owner=0;
    return written ? (int64_t)written : error;
#endif
}
