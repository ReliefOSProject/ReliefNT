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
            if (writing && task_effective_role(task) != RELIEFOS_AUTH_ROLE_ADMIN &&
                !(task->uid == 0 && storage_installer_root_active())) return -RELIEFOS_EACCES;
            return syscall_regular_io(task,file,a1,a2,a3,writing,true);
        }
        if (file->node.type == RELIEFOS_FS_TYPE_FILE &&
            !(file->flags & (TASK_FILE_FLAG_DEV_NODE | TASK_FILE_FLAG_DEV_SHM)) &&
            !(file->node.flags & (STORAGE_NODE_FLAG_PROC | STORAGE_NODE_FLAG_SYSFS))) {
            bool writing = number == LINUX_SYS_PWRITE64;
            if (writing ? !file_can_write(file) : !file_can_read(file)) return -RELIEFOS_EBADF;
            if (writing ? !user_range_ok(a1, a2) : !user_range_writable(a1, a2)) return -RELIEFOS_EFAULT;
            return syscall_regular_io(task, file, a1, a2, a3, writing, true);
        }
        if (number == LINUX_SYS_PREAD64 && file->drm_file)
            return syscall_drm_pread(task, file, a1, a2, a3);
        saved = file->offset;
        file->offset = a3;
        result = syscall_dispatch_regs_legacy(number == LINUX_SYS_PREAD64 ? LINUX_SYS_READ : LINUX_SYS_WRITE,
                                              a0, a1, a2, 0, 0, 0);
        file->offset = saved;
        return result;
    }

    if (number == LINUX_SYS_READV || number == LINUX_SYS_WRITEV ||
        number == LINUX_SYS_PREADV || number == LINUX_SYS_PWRITEV ||
        number == LINUX_SYS_PREADV2 || number == LINUX_SYS_PWRITEV2) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_io(task, (int)a0);
        bool writing = number == LINUX_SYS_WRITEV || number == LINUX_SYS_PWRITEV ||
                       number == LINUX_SYS_PWRITEV2;
        bool positional = number == LINUX_SYS_PREADV || number == LINUX_SYS_PWRITEV ||
                          number == LINUX_SYS_PREADV2 || number == LINUX_SYS_PWRITEV2;
        uint64_t position = (uint64_t)(uint32_t)a3 |
                            ((uint64_t)(uint32_t)a4 << 32);
        bool current_position = (number == LINUX_SYS_PREADV2 ||
                                number == LINUX_SYS_PWRITEV2) &&
                               position == UINT64_MAX;
        bool drm_read = file && file->drm_file && !writing;
        uint64_t positional_flags = (number == LINUX_SYS_PREADV2 ||
                                     number == LINUX_SYS_PWRITEV2) ? a5 : 0;
        uint64_t saved_offset = 0;
        bool restore_position = false;
        int64_t total;
        if (file && file->kind == TASK_FILE_KIND_SIGNALFD) {
            if (positional && !current_position) return (int64_t)position < 0 ? -LINUX_EINVAL : -LINUX_ESPIPE;
            if (writing) return -LINUX_EINVAL;
            return task_signalfd_readv(task, file, a1, a2, (uint32_t)positional_flags);
        }
        if (positional && !current_position && (int64_t)position < 0)
            return -RELIEFOS_EINVAL;
        if (positional_flags &&
            !(drm_read && positional_flags == LINUX_RWF_HIPRI))
            return -RELIEFOS_EOPNOTSUPP;
        if (file ? !(writing ? file_can_write(file) : file_can_read(file)) :
            (!task_pty_for_io(task, (int)a0) &&
             ((uint32_t)a0 >= SCHED_TASK_STDIO_MAX ||
              (sched_task_fds(task)->closed_stdio_mask & (1u << (uint32_t)a0))))) return -RELIEFOS_EBADF;
        if (a2 > UIO_MAXIOV) return -RELIEFOS_EINVAL;
        if (!a2) return 0;
        if (!user_range_ok(a1, a2 * sizeof(struct iovec))) return -RELIEFOS_EFAULT;
        struct iovec iov[UIO_MAXIOV];
        const struct iovec *user_iov = (const struct iovec *)(uintptr_t)a1;
        uint64_t requested = 0;
        for (uint64_t i = 0; i < a2; ++i) {
            iov[i] = user_iov[i];
            if ((int64_t)iov[i].iov_len < 0) return -RELIEFOS_EINVAL;
            if (iov[i].iov_len > 0x7ffff000u - requested) iov[i].iov_len = 0x7ffff000u - requested;
            if (iov[i].iov_len && !(writing ? user_range_ok((uintptr_t)iov[i].iov_base, iov[i].iov_len) :
                user_range_writable((uintptr_t)iov[i].iov_base, iov[i].iov_len))) return -RELIEFOS_EFAULT;
            requested += iov[i].iov_len;
        }
        if (!requested) return 0;
        if (positional && !current_position) {
            if (!file) return -RELIEFOS_EINVAL;
            if (!drm_read) {
                if (file->node.type != RELIEFOS_FS_TYPE_FILE) return -RELIEFOS_ESPIPE;
                saved_offset = file->offset;
                file->offset = position;
                restore_position = true;
            }
        }
        if (drm_read) {
            return syscall_drm_readv(task, file, iov, a2,
                                     positional && !current_position,
                                     position, positional_flags);
        }
        if (file && (file->flags & TASK_FILE_FLAG_SOCKET_UNIX)) {
            if (restore_position) file->offset = saved_offset;
            return task_socket_vector(file, iov, (uint32_t)a2, requested, writing);
        }
        total = 0;
        for (uint64_t i = 0; i < a2; ++i) {
            if (!iov[i].iov_len) continue;
            /* The vector loop already validated its request. Reuse the same
             * scalar checks without recursively allocating the large legacy
             * dispatcher frame for every iovec. */
            int64_t part = writing
                ? syscall_io_write(a0, (uintptr_t)iov[i].iov_base, iov[i].iov_len)
                : syscall_io_read(a0, (uintptr_t)iov[i].iov_base, iov[i].iov_len);
            if (part < 0) {
                if (restore_position) file->offset = saved_offset;
                return total ? total : part;
            }
            total += part;
            if ((uint64_t)part < iov[i].iov_len) break;
        }
        if (restore_position) file->offset = saved_offset;
        return total;
    }

    if (number == LINUX_SYS_GETDENTS || number == LINUX_SYS_GETDENTS64) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_fd(task, (int)a0);
        uint32_t written = 0;
        uint32_t capacity = (uint32_t)a2;
        if (!file) return -RELIEFOS_EBADF;
        if (file->node.type != RELIEFOS_FS_TYPE_DIR) return -RELIEFOS_ENOTDIR;
        if (!file_can_read(file)) return -RELIEFOS_EBADF;
        if (!user_range_writable(a1, capacity)) return -RELIEFOS_EFAULT;
        while (1) {
            struct reliefos_dir_entry entry;
            uint64_t next_offset = file->offset;
            int step = file->node.flags & STORAGE_NODE_FLAG_PROC
                         ? proc_readdir(file->path, &next_offset, &entry)
                         : storage_readdir_node(&file->node, &next_offset, &entry);
            uint32_t name_len;
            uint32_t reclen;
            uint8_t *dst;
            if (step < 0) return written ? (int64_t)written : storage_errno(step);
            if (step == 0) break;
            name_len = 0;
            while (name_len < sizeof(entry.name) && entry.name[name_len]) ++name_len;
            /* The legacy getdents record has no explicit d_type field.  Since
             * Linux 2.6 the type byte is stored in the final byte of each
             * aligned record; getdents64 carries it at offset 18 instead. */
            uint32_t name_offset = number == LINUX_SYS_GETDENTS64 ?
                (uint32_t)offsetof(struct linux_dirent64, d_name) : 18u;
            uint32_t type_bytes = number == LINUX_SYS_GETDENTS64 ? 0u : 1u;
            reclen = (uint32_t)((name_offset + name_len + 1 + type_bytes + 7) & ~7u);
            if (reclen > capacity - written) return written ? (int64_t)written : -RELIEFOS_EINVAL;
            dst = (uint8_t *)(uintptr_t)a1 + written;
            *(uint64_t *)(dst + 0) = linux_stat_inode(entry.name, NULL);
            *(int64_t *)(dst + 8) = (int64_t)next_offset;
            *(uint16_t *)(dst + 16) = (uint16_t)reclen;
            uint8_t dtype = entry.type == RELIEFOS_FS_TYPE_DIR ? LINUX_DT_DIR :
                            entry.type == RELIEFOS_FS_TYPE_FIFO ? 1u :
                            entry.type == RELIEFOS_FS_TYPE_SOCKET ? LINUX_DT_SOCK :
                            entry.type == RELIEFOS_FS_TYPE_SYMLINK ? LINUX_DT_LNK :
                            entry.type == RELIEFOS_FS_TYPE_DEVICE ? LINUX_DT_CHR : LINUX_DT_REG;
            if (entry.type == RELIEFOS_FS_TYPE_FILE && !(file->node.flags & STORAGE_NODE_FLAG_PROC)) {
                char entry_path[RELIEFOS_FS_PATH_LEN];
                struct reliefos_permissions permissions;
                if (storage_resolve_path(file->path, entry.name, entry_path, sizeof(entry_path)) == 0 &&
                    fs_permissions_get(entry_path, NULL, &permissions) == 0 &&
                    (permissions.mode & LINUX_S_IFMT) == LINUX_S_IFSOCK) dtype = LINUX_DT_SOCK;
            }
            for (uint32_t i = 0; i <= name_len; ++i) dst[name_offset + i] = entry.name[i];
            for (uint32_t i = name_offset + name_len + 1; i < reclen; ++i) dst[i] = 0;
            if (number == LINUX_SYS_GETDENTS) dst[reclen - 1] = dtype;
            else dst[18] = dtype;
            file->offset = next_offset;
            written += reclen;
        }
        return written;
    }

    if (number == LINUX_SYS_LSEEK) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_fd(task, (int)a0);
        int64_t offset = (int64_t)a1;
        int64_t base = 0;
        int64_t size = 0;
        if (!file) {
            return -RELIEFOS_EBADF;
        }
        if (file->drm_file) return syscall_drm_llseek(file, a2);
        if (file->drm_prime_file)
            return drm_prime_file_llseek(file, offset, (uint32_t)a2);
        if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_CONTROL)
            return -LINUX_ESPIPE;
        if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_MIXER)
            return -LINUX_ESPIPE;
        if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_PCM)
            return audio_device_seek(file, offset, (int)a2, NULL);
        if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_TIMER)
            return audio_timer_seek(file, offset, (int)a2, NULL);
        if (file->flags & TASK_FILE_FLAG_PIPE) return -LINUX_ESPIPE;
        if (task_device_is(file, STORAGE_DEV_KIND_CONSOLE)) return -LINUX_ESPIPE;
        if (file->io_owner && file->io_owner != task->pid) return -RELIEFOS_EAGAIN;
        int refresh = storage_inode_refresh(&file->node);
        if (refresh < 0) return refresh;
        if (file->kind == TASK_FILE_KIND_SIGNALFD) return (uint32_t)a2 > 4 ? -LINUX_EINVAL : (int64_t)file->offset;
        if (file->node.type == RELIEFOS_FS_TYPE_FILE ||
            (file->flags & TASK_FILE_FLAG_DEV_SHM)) {
            size = (int64_t)file->node.size;
            base = 0;
            if ((int)a2 == RELIEFOS_SEEK_CUR) {
                base = (int64_t)file->offset;
            } else if ((int)a2 == RELIEFOS_SEEK_END) {
                base = size;
            } else if ((int)a2 != RELIEFOS_SEEK_SET) {
                return -RELIEFOS_EINVAL;
            }
        } else if (file->node.type == RELIEFOS_FS_TYPE_DIR) {
            if ((int)a2 == RELIEFOS_SEEK_CUR) {
                base = (int64_t)file->offset * (int64_t)sizeof(struct reliefos_dir_entry);
            } else if ((int)a2 == RELIEFOS_SEEK_SET) {
                base = 0;
            } else {
                return -RELIEFOS_EINVAL;
            }
        } else if (file->flags & TASK_FILE_FLAG_DEV_BLOCK) {
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
        if (base + offset < 0) {
            return -RELIEFOS_EINVAL;
        }
        if (file->node.type == RELIEFOS_FS_TYPE_DIR) {
            file->offset = (uint64_t)((base + offset) / (int64_t)sizeof(struct reliefos_dir_entry));
            file->aux = 0;
            return (int64_t)(file->offset * sizeof(struct reliefos_dir_entry));
        }
        file->offset = (uint64_t)(base + offset);
        file->read_cursor.valid = 0;
        return (int64_t)file->offset;
    }

    return -RELIEFOS_ENOSYS;
}
