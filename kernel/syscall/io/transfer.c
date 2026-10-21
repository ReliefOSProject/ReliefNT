#include "../internal.h"



int64_t syscall_sendfile(uint64_t out_fd_arg, uint64_t in_fd_arg,
                                uint64_t offset_arg, uint64_t count_arg)
{
    struct task *task = sched_current_task();
    struct task_file *out;
    struct task_file *in;
    uint8_t *buffer;
    uint64_t position;
    uint64_t total = 0;
    uint64_t count = count_arg > 0x7ffff000ULL ? 0x7ffff000ULL : count_arg;
    int64_t result = 0;

    if (!task) return -RELIEFOS_EPERM;
    out = task_file_for_fd(task, (int)out_fd_arg);
    in = task_file_for_fd(task, (int)in_fd_arg);
    if (!out || !in) return -RELIEFOS_EBADF;
    if (out == in || task_file_description(out) == task_file_description(in)) {
        return -RELIEFOS_EINVAL;
    }
    if (!file_can_read(in) || !file_can_write(out)) return -RELIEFOS_EBADF;
    if (in->node.type != RELIEFOS_FS_TYPE_FILE ||
        (in->node.flags & STORAGE_NODE_FLAG_PROC)) return -RELIEFOS_EINVAL;
    if (out->node.type != RELIEFOS_FS_TYPE_FILE &&
        !(out->flags & (TASK_FILE_FLAG_PIPE | TASK_FILE_FLAG_SOCKET_UNIX |
                        TASK_FILE_FLAG_SOCKET_INET))) return -RELIEFOS_EINVAL;
    if (out->node.type == RELIEFOS_FS_TYPE_FILE && (out->flags & RELIEFOS_O_APPEND)) {
        return -RELIEFOS_EINVAL;
    }
    if (offset_arg) {
        if (!user_range_ok(offset_arg, sizeof(int64_t)) ||
            !user_range_writable(offset_arg, sizeof(int64_t))) return -RELIEFOS_EFAULT;
        if (*(const int64_t *)(uintptr_t)offset_arg < 0) return -RELIEFOS_EINVAL;
        position = (uint64_t)*(const int64_t *)(uintptr_t)offset_arg;
    } else {
        position = in->offset;
    }
    int refresh = storage_inode_refresh(&in->node);
    if (refresh < 0) return refresh;
    if (!count || position >= in->node.size) return 0;

    buffer = kernel_malloc(RELIEFOS_FS_IO_SLICE_BYTES);
    if (!buffer) return -RELIEFOS_ENOMEM;
    while (total < count && position < in->node.size) {
        uint64_t available = in->node.size - position;
        uint32_t request = (uint32_t)(count - total);
        uint32_t got = 0;
        uint32_t written = 0;
        int ret;
        if (request > RELIEFOS_FS_IO_SLICE_BYTES) request = RELIEFOS_FS_IO_SLICE_BYTES;
        if ((uint64_t)request > available) request = (uint32_t)available;
        ret = storage_read_node(&in->node, position, buffer, request, &got);
        if (ret < 0) {
            result = total ? (int64_t)total : storage_errno(ret);
            break;
        }
        if (!got) break;
        if (out->node.type == RELIEFOS_FS_TYPE_FILE) {
            ret = out->inode ? storage_write_held_node(&out->node, out->offset, buffer, got, &written) :
                storage_write_node(out->path, out->offset, buffer, got, &written);
            if (ret < 0) {
                result = total ? (int64_t)total : storage_errno(ret);
                break;
            }
            out->read_cursor.valid = 0;
            out->offset += written;
            if (written > 0 && out->node.first_cluster < 2) {
                struct storage_node updated;
                if (storage_lookup_path(out->path, &updated) == 0) out->node = updated;
            }
            if (written > 0 && out->offset > out->node.size) out->node.size = out->offset;
        } else if (out->flags & TASK_FILE_FLAG_PIPE) {
            ret = task_pipe_write(out, buffer, got);
            if (ret < 0) {
                result = total ? (int64_t)total : ret;
                break;
            }
            written = (uint32_t)ret;
        } else if (out->flags & TASK_FILE_FLAG_SOCKET_UNIX) {
            ret = task_socket_write(out, buffer, got);
            if (ret < 0) {
                result = total ? (int64_t)total : ret;
                break;
            }
            written = (uint32_t)ret;
        } else {
            ret = task_inet_write(out, buffer, got);
            if (ret < 0) {
                result = total ? (int64_t)total : ret;
                break;
            }
            written = (uint32_t)ret;
        }
        if (!written) break;
        if (written > got) written = got;
        position += written;
        total += written;
        if (written < got) break;
    }
    kernel_free(buffer);
    if (offset_arg) {
        *(int64_t *)(uintptr_t)offset_arg = (int64_t)position;
    } else {
        in->offset = position;
    }
    if (result) return result;
    return (int64_t)total;
}

int64_t syscall_copy_file_range(uint64_t in_fd_arg, uint64_t off_in_arg,
                                       uint64_t out_fd_arg, uint64_t off_out_arg,
                                       uint64_t len_arg, uint64_t flags_arg)
{
    struct task *task = sched_current_task();
    struct task_file *in;
    struct task_file *out;
    uint8_t *buffer;
    uint64_t in_position;
    uint64_t out_position;
    uint64_t total = 0;
    uint64_t len = len_arg > 0x7ffff000ULL ? 0x7ffff000ULL : len_arg;
    int64_t result = 0;

    if (!task) return -RELIEFOS_EPERM;
    if (flags_arg) return -RELIEFOS_EINVAL;
    in = task_file_for_fd(task, (int)in_fd_arg);
    out = task_file_for_fd(task, (int)out_fd_arg);
    if (!in || !out) return -RELIEFOS_EBADF;
    if (!file_can_read(in) || !file_can_write(out)) return -RELIEFOS_EBADF;
    if (in->node.type != RELIEFOS_FS_TYPE_FILE ||
        out->node.type != RELIEFOS_FS_TYPE_FILE ||
        (in->node.flags & STORAGE_NODE_FLAG_PROC) ||
        (out->node.flags & STORAGE_NODE_FLAG_PROC)) return -RELIEFOS_EINVAL;
    if (off_in_arg && (!user_range_ok(off_in_arg, sizeof(int64_t)) ||
                       !user_range_writable(off_in_arg, sizeof(int64_t)))) return -RELIEFOS_EFAULT;
    if (off_out_arg && (!user_range_ok(off_out_arg, sizeof(int64_t)) ||
                        !user_range_writable(off_out_arg, sizeof(int64_t)))) return -RELIEFOS_EFAULT;
    if (off_in_arg && *(const int64_t *)(uintptr_t)off_in_arg < 0) return -RELIEFOS_EINVAL;
    if (off_out_arg && *(const int64_t *)(uintptr_t)off_out_arg < 0) return -RELIEFOS_EINVAL;
    in_position = off_in_arg ? (uint64_t)*(const int64_t *)(uintptr_t)off_in_arg : in->offset;
    out_position = off_out_arg ? (uint64_t)*(const int64_t *)(uintptr_t)off_out_arg : out->offset;
    if (!in->inode && in->path[0] && storage_lookup_path(in->path, &in->node) < 0) return -RELIEFOS_EIO;
    if (!out->inode && out->path[0] && storage_lookup_path(out->path, &out->node) < 0) return -RELIEFOS_EIO;
    int refresh = storage_inode_refresh(&in->node);
    if (refresh < 0) return refresh;
    refresh = storage_inode_refresh(&out->node);
    if (refresh < 0) return refresh;
    if (!len || in_position >= in->node.size) return 0;

    buffer = kernel_malloc(RELIEFOS_FS_IO_SLICE_BYTES);
    if (!buffer) return -RELIEFOS_ENOMEM;
    while (total < len && in_position < in->node.size) {
        uint64_t available = in->node.size - in_position;
        uint32_t request = (uint32_t)(len - total);
        uint32_t got = 0;
        uint32_t written = 0;
        int ret;
        if (request > RELIEFOS_FS_IO_SLICE_BYTES) request = RELIEFOS_FS_IO_SLICE_BYTES;
        if ((uint64_t)request > available) request = (uint32_t)available;
        ret = storage_read_node(&in->node, in_position, buffer, request, &got);
        if (ret < 0) {
            result = total ? (int64_t)total : storage_errno(ret);
            break;
        }
        if (!got) break;
        ret = out->inode ? storage_write_held_node(&out->node, out_position, buffer, got, &written) :
            storage_write_node(out->path, out_position, buffer, got, &written);
        if (ret < 0) {
            result = total ? (int64_t)total : storage_errno(ret);
            break;
        }
        if (!written) break;
        if (written > got) written = got;
        in_position += written;
        out_position += written;
        total += written;
        out->read_cursor.valid = 0;
        if (written < got) break;
        if (out->node.first_cluster < 2) {
            struct storage_node updated;
            if (storage_lookup_path(out->path, &updated) == 0) out->node = updated;
        } else if (out_position > out->node.size) {
            out->node.size = out_position;
        }
    }
    kernel_free(buffer);
    if (off_in_arg) *(int64_t *)(uintptr_t)off_in_arg = (int64_t)in_position;
    else in->offset = in_position;
    if (off_out_arg) *(int64_t *)(uintptr_t)off_out_arg = (int64_t)out_position;
    else out->offset = out_position;
    return result ? result : (int64_t)total;
}
