#include "../internal.h"



int task_device_is(const struct task_file *file, uint32_t kind)
{
    return file && (file->flags & TASK_FILE_FLAG_DEV_NODE) &&
           file->node.first_cluster == kind;
}

/**
 * @brief Decode a kernel-created descriptor backed by a genuine block device node.
 * @param file Descriptor to validate; may be null.
 * @param disk_id Optional physical disk ID output.
 * @param partition_index Optional GPT entry index output, or -1 for the whole disk.
 * @return One for a block descriptor, zero for all other descriptors.
 */
int task_block_device(const struct task_file *file, uint32_t *disk_id,
                             int32_t *partition_index)
{
    if (!file || !(file->flags & TASK_FILE_FLAG_DEV_BLOCK) ||
        !(file->node.flags & STORAGE_NODE_FLAG_DEV_BLOCK) ||
        !task_device_is(file, STORAGE_DEV_KIND_DISK)) return 0;
    if (disk_id) *disk_id = STORAGE_BLOCK_DISK_ID(file->node.volume_id);
    if (partition_index) *partition_index = STORAGE_BLOCK_PARTITION(file->node.volume_id);
    return 1;
}

/**
 * @brief Read an opened device; block descriptors use Linux open-time DAC checks.
 * @param task Current task, required for device context.
 * @param file Opened readable device descriptor.
 * @param buffer Validated writable destination of length bytes.
 * @param length Maximum byte count to read.
 * @return Bytes read, negative errno, or KERNEL_SYSCALL_BLOCKED for terminal
 * job control; an empty nonblocking console returns negative EAGAIN.
 */
int task_device_read(struct task *task, struct task_file *file,
                            void *buffer, uint32_t length)
{
    uint32_t disk_id;
    int32_t partition_index;
    uint32_t got = 0;
    if (!task || !file || !buffer || !length) return 0;
    if (task_block_device(file, &disk_id, &partition_index)) {
        /* Linux DAC is checked when opening the device. A readable descriptor
         * remains readable after fork, privilege changes or SCM_RIGHTS. */
        int ret = storage_disk_block_read(disk_id, partition_index, file->offset,
                                          buffer, length, &got);
        return ret < 0 ? ret : (int)got;
    }
    if (task_device_is(file, STORAGE_DEV_KIND_NULL) ||
        task_device_is(file, STORAGE_DEV_KIND_FULL) ||
        task_device_is(file, STORAGE_DEV_KIND_SERIAL)) return 0;
    if (task_device_is(file, STORAGE_DEV_KIND_CONSOLE)) {
        uint32_t terminal = (uint32_t)file->aux;
        if (!terminal || !pty_is_active(terminal)) return -RELIEFOS_ENODEV;
        int64_t change = pty_check_change(terminal, task->pid, 21);
        if (change) return (int)change;
        if ((file->flags & RELIEFOS_O_NONBLOCK) && !pty_input_available(terminal) &&
            !pty_is_hungup(terminal)) return -RELIEFOS_EAGAIN;
        return (int)pty_read_input(terminal, (char *)buffer, length);
    }
    if (task_device_is(file, STORAGE_DEV_KIND_RANDOM) ||
        task_device_is(file, STORAGE_DEV_KIND_URANDOM)) {
        int ret = kernel_random_fill(buffer, length);
        return ret < 0 ? ret : (int)length;
    }
    if (task_device_is(file, STORAGE_DEV_KIND_ZERO)) {
        uint8_t *dst = (uint8_t *)buffer;
        for (uint32_t i = 0; i < length; ++i) dst[i] = 0;
        return (int)length;
    }
    if (task_device_is(file, STORAGE_DEV_KIND_TTY)) {
        return (int)pty_read_input(task->pty_id, (char *)buffer, length);
    }
    if (task_device_is(file, STORAGE_DEV_KIND_KEYBOARD) ||
        task_device_is(file, STORAGE_DEV_KIND_MOUSE)) {
        task_evdev_bind_vt(task, file);
        int ret = input_evdev_read_vt(file->node.first_cluster, &file->aux,
                                      buffer, length, file->aux2, file->input_vt);
        if (ret == 0 && (file->flags & RELIEFOS_O_NONBLOCK)) {
            return -RELIEFOS_EAGAIN;
        }
        return ret;
    }
    /* The first OSS implementation is playback-only. */
    if (task_device_is(file, STORAGE_DEV_KIND_AUDIO)) {
        if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_PCM)
            return audio_device_read(task, file, buffer, length);
        return audio_oss_read(task, file, buffer, length);
    }
    if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_TIMER)
        return audio_timer_read(task, file, buffer, length);
    if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_MIXER) return -RELIEFOS_ENODEV;
    if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_CONTROL) {
        if (file->flags & RELIEFOS_O_NONBLOCK)
            return audio_control_read_file(file->audio_control_file, buffer, length);
        return audio_control_read_task(task, file, buffer, length);
    }
    return -RELIEFOS_EBADF;
}

/** @brief Bound one device write before narrowing its 64-bit user length.
 * @param file Borrowed description under the execution transaction.
 * @param count Validated user byte count.
 * @return Request bytes; no ownership, allocation, wait or IRQ change occurs.
 * Task context. PCM adapters retain their small internal copy buffers, queue
 * limits, short transfers and interruptible waits. Other devices use the
 * filesystem slice. A 32 KiB PCM request avoids forcing one Doom mixed block
 * through five syscall returns while bounding the execution transaction.
 */
uint32_t task_device_write_length(const struct task_file *file, uint64_t count)
{
    uint32_t limit = task_device_is(file, STORAGE_DEV_KIND_AUDIO)
        ? 32768u : RELIEFOS_FS_IO_SLICE_BYTES;
    return count > limit ? limit : (uint32_t)count;
}

/**
 * @brief Write a device, applying terminal processing to system-console output.
 * @param task Calling task, used for permissions and terminal job control.
 * @param file Writable opened device description; console aux retains its VT.
 * @param buffer Validated source of length bytes.
 * @param length Bounded device transfer size in bytes.
 * @return Accepted bytes, negative errno, or KERNEL_SYSCALL_BLOCKED for job
 * control. Accepted console bytes are also mirrored to diagnostic serial.
 */
int task_device_write(struct task *task, struct task_file *file,
                             const void *buffer, uint32_t length)
{
    uint32_t disk_id;
    int32_t partition_index;
    uint32_t wrote = 0;
    if (!task || !file) return -RELIEFOS_EBADF;
    if (task_block_device(file, &disk_id, &partition_index)) {
        if (!file_can_write(file)) return -RELIEFOS_EBADF;
        int ret = storage_disk_block_write(disk_id, partition_index, file->offset,
                                           buffer, length, &wrote);
        return ret < 0 ? ret : (int)wrote;
    }
    if (task_device_is(file, STORAGE_DEV_KIND_NULL) ||
        task_device_is(file, STORAGE_DEV_KIND_ZERO) ||
        task_device_is(file, STORAGE_DEV_KIND_RANDOM) ||
        task_device_is(file, STORAGE_DEV_KIND_URANDOM)) return (int)length;
    if (task_device_is(file, STORAGE_DEV_KIND_FULL)) return -RELIEFOS_ENOSPC;
    if (task_device_is(file, STORAGE_DEV_KIND_TTY)) {
        if (!buffer) return -RELIEFOS_EFAULT;
        return (int)pty_write_output(task->pty_id, (const char *)buffer, length);
    }
    if (task_device_is(file, STORAGE_DEV_KIND_KEYBOARD) ||
        task_device_is(file, STORAGE_DEV_KIND_MOUSE)) {
        return input_evdev_write(file->node.first_cluster, buffer, length);
    }
    if (task_device_is(file, STORAGE_DEV_KIND_CONSOLE)) {
        if (!length) return 0;
        if (!buffer) return -RELIEFOS_EFAULT;
        uint32_t terminal = (uint32_t)file->aux;
        if (!terminal || !pty_is_active(terminal)) return -RELIEFOS_ENODEV;
        int64_t change = pty_check_change(terminal, task->pid, 0);
        if (change) return (int)change;
        int64_t result = pty_write_output(terminal, (const char *)buffer, length);
        if (result > 0) console_serial_write_len(buffer, (size_t)result);
        return (int)result;
    }
    if (task_device_is(file, STORAGE_DEV_KIND_KMSG)) {
        if (!buffer) return -RELIEFOS_EFAULT;
        console_write_len((const char *)buffer, length);
        return (int)length;
    }
    if (task_device_is(file, STORAGE_DEV_KIND_SERIAL)) {
        const char *src = (const char *)buffer;
        uint32_t done = 0;
        if (!buffer) return -RELIEFOS_EFAULT;
        /* The serial driver exposes a string-oriented backend.  Keep each
         * temporary chunk bounded and preserve the write length. */
        while (done < length) {
            char chunk[128];
            uint32_t take = length - done;
            if (take >= sizeof(chunk)) take = sizeof(chunk) - 1u;
            for (uint32_t i = 0; i < take; ++i) chunk[i] = src[done + i];
            chunk[take] = 0;
            serial_write(chunk);
            done += take;
        }
        return (int)done;
    }
    if (task_device_is(file, STORAGE_DEV_KIND_AUDIO)) {
        if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_PCM)
            return audio_device_write(task, file, buffer, length);
        return audio_oss_write(task, file, buffer, length);
    }
    if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_TIMER)
        return -RELIEFOS_EBADF;
    if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_MIXER) return -RELIEFOS_EBADF;
    if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_CONTROL)
        return -RELIEFOS_EBADF;
    return -RELIEFOS_EBADF;
}
