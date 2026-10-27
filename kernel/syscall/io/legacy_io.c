#include "../internal.h"





/**
 * @brief Dispatch an already-normalised syscall within this ABI category.
 * @param number Linux or ReliefOS syscall number selected by the common dispatcher.
 * @param a0 First native ABI argument; pointers remain subject to the original checks.
 * @param a1 Second native ABI argument.
 * @param a2 Third native ABI argument.
 * @param a3 Fourth native ABI argument.
 * @param a4 Fifth native ABI argument.
 * @param a5 Sixth native ABI argument.
 * @return Original result, negative errno, or internal blocking/retry status.
 * @note The caller retains the kernel execution transaction and descriptor pins.
 */
int64_t syscall_legacy_io(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    if (number == LINUX_SYS_WRITE) {
        return syscall_io_write(a0, a1, a2);
    }

    if (number == LINUX_SYS_READ) {
        return syscall_io_read(a0, a1, a2);
    }

    if (number == LINUX_SYS_SENDFILE) {
        return syscall_sendfile(a0, a1, a2, a3);
    }

    if (number == LINUX_SYS_COPY_FILE_RANGE) {
        return syscall_copy_file_range(a0, a1, a2, a3, a4, a5);
    }

    if (number == LINUX_SYS_PREAD64 || number == LINUX_SYS_PWRITE64) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_io(task, (int)a0);
        uint64_t saved;
        int64_t result;
        if (file && file->kind == TASK_FILE_KIND_SIGNALFD)
            return (int64_t)a3 < 0 ? -LINUX_EINVAL : -LINUX_ESPIPE;
        if ((int64_t)a3 < 0) return -RELIEFOS_EINVAL;
        if (file && (file->flags & TASK_FILE_FLAG_PIPE)) return -LINUX_ESPIPE;
        if (!file || (file->flags & TASK_FILE_FLAG_PATH)) return -RELIEFOS_EBADF;
        if (file->flags & TASK_FILE_FLAG_DEV_BLOCK) {
            bool writing = number == LINUX_SYS_PWRITE64;
            if (writing ? !file_can_write(file) : !file_can_read(file)) return -RELIEFOS_EBADF;
            if (writing ? !user_range_ok(a1,a2) : !user_range_writable(a1,a2)) return -RELIEFOS_EFAULT;
            /* Match write(2): device DAC is checked when this descriptor is opened. */
            return syscall_regular_io(task,file,a1,a2,a3,writing,true);
        }
        if (!file->character_file && file->node.type == RELIEFOS_FS_TYPE_FILE &&
            !(file->flags & (TASK_FILE_FLAG_DEV_NODE | TASK_FILE_FLAG_DEV_SHM))) {
            bool writing = number == LINUX_SYS_PWRITE64;
            if (writing ? !file_can_write(file) : !file_can_read(file)) return -RELIEFOS_EBADF;
            if (writing ? !user_range_ok(a1, a2) : !user_range_writable(a1, a2)) return -RELIEFOS_EFAULT;
            return syscall_regular_io(task, file, a1, a2, a3, writing, true);
        }
        saved = file->offset;
        file->offset = a3;
        result = number == LINUX_SYS_PREAD64 ? syscall_io_read(a0,a1,a2) : syscall_io_write(a0,a1,a2);
        file->offset = saved;
        return result;
    }

    if (number == LINUX_SYS_READV || number == LINUX_SYS_WRITEV ||
        number == LINUX_SYS_PREADV || number == LINUX_SYS_PWRITEV ||
        number == LINUX_SYS_PREADV2 || number == LINUX_SYS_PWRITEV2)
        return syscall_vector_io(sched_current_task(),number,a0,a1,a2,a3,a4,a5);

    if(number==LINUX_SYS_GETDENTS || number==LINUX_SYS_GETDENTS64)
        return syscall_getdents_object(sched_current_task(),number,(int32_t)a0,a1,(uint32_t)a2);

    if (number == LINUX_SYS_LSEEK) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_io(task, (int)a0);
        int64_t offset = (int64_t)a1;
        int64_t base = 0;
        int64_t size = 0;
        if (!file || (file->flags & TASK_FILE_FLAG_PATH)) {
            return -RELIEFOS_EBADF;
        }
        if(file->character_file)return character_file_seek(file->character_file,file,offset,(uint32_t)a2);
        if (file->flags & TASK_FILE_FLAG_PIPE) return -LINUX_ESPIPE;
        if (task_device_is(file, STORAGE_DEV_KIND_CONSOLE)) return -LINUX_ESPIPE;
        if (file->io_owner && file->io_owner != task->pid) return -RELIEFOS_EAGAIN;
        if (file->kind == TASK_FILE_KIND_SIGNALFD) return (uint32_t)a2 > 4 ? -LINUX_EINVAL : (int64_t)file->offset;
        if (file->node.type == RELIEFOS_FS_TYPE_FILE || file->node.type == RELIEFOS_FS_TYPE_DIR)
            return syscall_seek_object(task,(int32_t)a0,offset,(uint32_t)a2);
        if (file->flags & TASK_FILE_FLAG_DEV_BLOCK) {
            /* Block devices are seekable byte streams.  GPT editors and
             * filesystem tools use lseek(2) before issuing aligned raw I/O;
             * treating them as non-seekable makes every read after offset
             * zero fail with EINVAL. */
            size = (int64_t)file->node.size;
            if ((int)a2 == RELIEFOS_SEEK_CUR) {
                base = (int64_t)file->offset;
            } else if ((int)a2 == RELIEFOS_SEEK_END) {
                base = size;
            } else if ((int)a2 != RELIEFOS_SEEK_SET) {
                return -RELIEFOS_EINVAL;
            }
        } else {
            return -RELIEFOS_EINVAL;
        }
        int64_t position;
        if (__builtin_add_overflow(base,offset,&position) || position < 0)
            return -RELIEFOS_EINVAL;
        file->offset = (uint64_t)position;
        file->read_cursor.valid = 0;
        return (int64_t)file->offset;
    }

    return -RELIEFOS_ENOSYS;
}
