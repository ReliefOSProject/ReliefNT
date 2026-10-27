#include <reliefnt/syscall_internal.h>
#include <reliefnt/syscall.h>
#include <reliefnt/time.h>
#include <reliefnt/futex.h>
#include <reliefos/fs_abi.h>
#include <linux/errno.h>

/**
 * @brief Transfer regular-file or genuine block-node data, retaining progress across retries.
 * @param task Current task, with dispatcher-owned retry state.
 * @param file Authorized canonical OFD retaining its actual provider path.
 * @param buffer User buffer already validated by the dispatcher for count bytes.
 * @param count Requested byte count; capped at Linux MAX_RW_COUNT.
 * @param position Byte offset for positional I/O; otherwise use the shared file offset.
 * @param writing Whether to write rather than read.
 * @param positional Whether to preserve the shared file offset.
 * @return Bytes transferred, negative errno, or EAGAIN/BLOCKED with saved
 * retry progress. An ordinary vector owns this element's imported metadata;
 * its parent state separately accounts for earlier completed elements.
 * A vector retains the OFD reservation between completed elements until its
 * common parent completion/cancellation, so another caller cannot interleave.
 * Execution ownership is required. Ordinary files use their retained provider
 * and optional OFD-local provider capture/cursor, never a diagnostic filename; genuine block nodes use the
 * block service. No graph/module lock spans callbacks. O_APPEND stats the
 * actual inode; sync flags complete through the same retained provider.
 */
int64_t syscall_regular_io(struct task *task, struct task_file *file, uint64_t buffer, uint64_t count,
                           uint64_t position, bool writing, bool positional)
{
    uint64_t done = 0;
    uint64_t started = time_uptime_us();
    bool block = file->flags & TASK_FILE_FLAG_DEV_BLOCK;
    if (block && (!(file->flags & TASK_FILE_FLAG_DEV_NODE) ||
                  (file->node.flags & (STORAGE_NODE_FLAG_DEV_NODE | STORAGE_NODE_FLAG_DEV_BLOCK)) !=
                    (STORAGE_NODE_FLAG_DEV_NODE | STORAGE_NODE_FLAG_DEV_BLOCK) ||
                  file->node.first_cluster != STORAGE_DEV_KIND_DISK))
        return -LINUX_EBADF;
    bool resumable =
        task->syscall_file == file &&
        (task->syscall_file_number == LINUX_SYS_READ || task->syscall_file_number == LINUX_SYS_WRITE ||
         task->syscall_file_number == LINUX_SYS_PREAD64 || task->syscall_file_number == LINUX_SYS_PWRITE64 ||
         (task->vector_io && (task->syscall_file_number==LINUX_SYS_READV || task->syscall_file_number==LINUX_SYS_WRITEV ||
          task->syscall_file_number==LINUX_SYS_PREADV || task->syscall_file_number==LINUX_SYS_PWRITEV ||
          task->syscall_file_number==LINUX_SYS_PREADV2 || task->syscall_file_number==LINUX_SYS_PWRITEV2)));
    count = count > 0x7ffff000u ? 0x7ffff000u : count;
    if (file->io_owner && file->io_owner != task->pid)
        return -LINUX_EAGAIN;
    if (resumable && task->regular_io.active) {
        buffer = task->regular_io.buffer;
        count = task->regular_io.count;
        position = task->regular_io.position;
        done = task->regular_io.done;
    } else {
        if (!positional)
            position = file->offset;
        if (!block && writing && (file->flags & RELIEFOS_O_APPEND)) {
            struct linux_stat_abi status;
            int ret = vfs_path_stat(&file->path_ref,&status);
            if (ret < 0)
                return ret;
            if(status.st_size<0)return -LINUX_EIO;
            position = (uint64_t)status.st_size;
        }
        if (position > INT64_MAX || count > (uint64_t)INT64_MAX - position)
            return -LINUX_EINVAL;
        if (resumable) {
            task->regular_io.buffer = buffer;
            task->regular_io.count = count;
            task->regular_io.position = position;
            task->regular_io.done = 0;
            task->regular_io.active = true;
        }
    }
    if (!positional)
        file->io_owner = task->pid;
    int error = 0;
    while (done < count) {
        uint32_t length = count - done > 32768 ? 32768 : (uint32_t)(count - done);
        uint32_t transferred = 0;
        int ret;
        if (block) {
            uint32_t disk = STORAGE_BLOCK_DISK_ID(file->node.volume_id);
            int32_t part = STORAGE_BLOCK_PARTITION(file->node.volume_id);
            ret = writing ? storage_disk_block_write(disk, part, position + done,
                                                     (const void *)(uintptr_t)(buffer + done), length,
                                                     &transferred)
                          : storage_disk_block_read(disk, part, position + done,
                                                    (void *)(uintptr_t)(buffer + done), length, &transferred);
        } else if (writing) {
            file->read_cursor.valid = 0;
            ret = vfs_path_write(&file->path_ref,position+done,
                (const void *)(uintptr_t)(buffer+done),length,&transferred);
        } else if(file->provider_file) {
            file->read_cursor.valid=0;
            ret=vfs_file_read(file->provider_file,position+done,(void *)(uintptr_t)(buffer+done),length,&transferred);
        } else {
            ret = vfs_path_read_cursor(&file->path_ref, position + done, (void *)(uintptr_t)(buffer + done),
                                           length, &transferred, &file->read_cursor);
        }
        if (transferred > length) {
            error = -LINUX_EIO;
            break;
        }
        done += transferred;
        if (!positional)
            file->offset = position + done;
        if (!block && writing && transferred) {
            file->node=*vfs_path_node(&file->path_ref);
            if (position + done > file->node.size)
                file->node.size = position + done;
        }
        if (resumable)
            task->regular_io.done = done;
        if ((ret == -LINUX_EAGAIN || ret == KERNEL_SYSCALL_BLOCKED) && resumable)
            return ret;
        if (ret < 0) {
            error = ret;
            break;
        }
        if (transferred < length)
            break;
        /* Yield only after a completed storage operation, with no DMA pending.
         * The dispatcher resumes this blocking syscall with its saved progress. */
        if (resumable && done < count && time_uptime_us() - started >= 2000) {
            storage_release_task_io(task->pid);
            return -LINUX_EAGAIN;
        }
    }
    if (!task->vector_io && file->io_owner == task->pid)
        file->io_owner = 0;
    if (resumable)
        task->regular_io.active = false;
    if (writing && done && (file->flags & LINUX_O_DSYNC)) {
        int sync = block ? storage_sync_disk(STORAGE_BLOCK_DISK_ID(file->node.volume_id))
                         : vfs_path_sync(&file->path_ref,false,
                             (file->flags&LINUX_O_SYNC)==LINUX_O_DSYNC);
        if (sync < 0) return sync;
    }
    return done ? (int64_t)done : error;
}
