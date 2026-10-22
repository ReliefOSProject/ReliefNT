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
int64_t syscall_legacy_descriptors(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    if (number == LINUX_SYS_CREAT) {
        /* creat(2) is exactly open(path, O_CREAT|O_WRONLY|O_TRUNC, mode)
         * on Linux; preserve the raw syscall's two-argument ABI. */
        return syscall_dispatch_regs_legacy(LINUX_SYS_OPEN, a0,
                                             RELIEFOS_O_CREAT | RELIEFOS_O_WRONLY |
                                             RELIEFOS_O_TRUNC, a1, 0, 0, 0);
    }

    if (number == LINUX_SYS_OPEN || number == LINUX_SYS_OPENAT) {
        struct task *task = sched_current_task();
        if (task && task->fifo_open_file) return task_fifo_open(task, NULL, 0, NULL);
        struct storage_node node;
        char path[RELIEFOS_FS_PATH_LEN];
        uint32_t flags = (uint32_t)(number == LINUX_SYS_OPENAT ? a2 : a1);
        uint32_t mode = (uint32_t)(number == LINUX_SYS_OPENAT ? a3 : a2);
        /* Unknown open bits are ignored by Linux open/openat. Never let them
         * supply private descriptor kinds such as DEV_BLOCK or DEV_NODE. */
        flags &= LINUX_O_ACCMODE | LINUX_O_CREAT | LINUX_O_EXCL | LINUX_O_NOCTTY |
                 LINUX_O_TRUNC | LINUX_O_APPEND | LINUX_O_NONBLOCK | LINUX_O_DSYNC |
                 LINUX_O_ASYNC | LINUX_O_DIRECT | LINUX_O_LARGEFILE | LINUX_O_DIRECTORY |
                 LINUX_O_NOFOLLOW | LINUX_O_NOATIME | LINUX_O_CLOEXEC | LINUX_O_SYNC |
                 LINUX_O_PATH | LINUX_O_TMPFILE;
        /* Translate only user open flags: internal epoll allocation uses the
         * same numeric bit as O_PATH and must retain its descriptor kind. */
        flags = (flags & ~(uint32_t)(LINUX_O_PATH | TASK_FILE_FLAG_PATH)) |
                ((flags & LINUX_O_PATH) ? TASK_FILE_FLAG_PATH : 0);
        if ((flags & (RELIEFOS_O_CREAT | RELIEFOS_O_DIRECTORY)) ==
            (RELIEFOS_O_CREAT | RELIEFOS_O_DIRECTORY)) return -RELIEFOS_EINVAL;
        uint8_t created = 0;
        uint32_t lookup_flags = flags & LINUX_O_NOFOLLOW ? 0 : FS_LOOKUP_FOLLOW;
        if ((flags & (LINUX_O_CREAT | LINUX_O_EXCL)) == (LINUX_O_CREAT | LINUX_O_EXCL))
            lookup_flags = FS_LOOKUP_PARENT;
        int ret = resolve_user_path_at_flags(task,
                    number == LINUX_SYS_OPENAT ? (int32_t)a0 : LINUX_AT_FDCWD,
                    number == LINUX_SYS_OPENAT ? a1 : a0, false, path, NULL, false, lookup_flags);
        if (ret < 0) {
            return ret;
        }
        if (lookup_flags == FS_LOOKUP_PARENT) {
            ret = mutation_path_slash(path, true);
            if (ret < 0) return ret;
        }
        /* POSIX stream aliases follow the caller's current descriptors,
         * including redirections installed by dup2().  Do this before the
         * synthetic devfs lookup so /dev/stdin is not mistaken for a fresh
         * PTY device node. */
        if (text_eq_cstr(path, "/dev/stdin") ||
            text_eq_cstr(path, "/dev/stdout") ||
            text_eq_cstr(path, "/dev/stderr")) {
            int source = text_eq_cstr(path, "/dev/stdin") ? 0 :
                         (text_eq_cstr(path, "/dev/stdout") ? 1 : 2);
            struct task_file *source_file = task_file_for_fd(task, source);
            if (source_file) {
                return task_duplicate_file_fd(task, source, 0, 0);
            }
            if (task_pty_stream_for_fd(task, source) >= 0) {
                return task_pty_duplicate_fd(task, source, 0, 0);
            }
            return -RELIEFOS_EBADF;
        }
        if (text_eq_cstr(path, "/dev/null")) {
            uint32_t access = (flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_RDONLY ? FS_ACCESS_READ :
                              (flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_WRONLY ? FS_ACCESS_WRITE :
                              FS_ACCESS_READ | FS_ACCESS_WRITE;
            ret = fs_permissions_check(task, path, access, false);
            if (ret < 0) return ret;
            if (flags & RELIEFOS_O_DIRECTORY) return -RELIEFOS_ENOTDIR;
            struct storage_node null_node = {
                .type = RELIEFOS_FS_TYPE_DEVICE,
                .flags = STORAGE_NODE_FLAG_DEV_NODE,
                .first_cluster = STORAGE_DEV_KIND_NULL,
            };
            return alloc_task_fd(task, &null_node,
                                 flags | TASK_FILE_FLAG_DEV_NULL |
                                 TASK_FILE_FLAG_DEV_NODE, path);
        }
        /* Unix98 PTY allocation is represented by explicit endpoint FDs.
         * The session owner is the opening task; fork/exec inherit the fd. */
        if (text_eq_cstr(path, "/dev/tty")) {
            uint32_t access = (flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_RDONLY ? FS_ACCESS_READ :
                (flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_WRONLY ? FS_ACCESS_WRITE : FS_ACCESS_READ | FS_ACCESS_WRITE;
            ret = fs_permissions_check(task, path, access, false);
            if (ret < 0) return ret;
            if (flags & RELIEFOS_O_DIRECTORY) return -RELIEFOS_ENOTDIR;
            if (!task || !task->controlling_pty_id) return -LINUX_ENXIO;
            return task_pty_endpoint_fd(task, task->controlling_pty_id,
                                       TASK_PTY_ENDPOINT_SLAVE, flags);
        }
        if (text_eq_cstr(path, "/dev/ptmx")) {
            uint32_t access = (flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_RDONLY ? FS_ACCESS_READ :
                              (flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_WRONLY ? FS_ACCESS_WRITE :
                              FS_ACCESS_READ | FS_ACCESS_WRITE;
            ret = fs_permissions_check(task, path, access, false);
            if (ret < 0) return ret;
            if (flags & RELIEFOS_O_DIRECTORY) return -RELIEFOS_ENOTDIR;
            int32_t pty_id = pty_create(task ? task->pid : 0);
            if (pty_id < 0) return pty_id;
            ret = task_pty_endpoint_fd(task, (uint32_t)pty_id,
                                       TASK_PTY_ENDPOINT_MASTER, flags);
            if (ret < 0) {
                (void)pty_destroy(task ? task->pid : 0, (uint32_t)pty_id);
            }
            return ret;
        }
        {
            uint32_t pty_id;
            if (task_pty_endpoint_path(path, &pty_id)) {
                if ((flags & (RELIEFOS_O_CREAT | RELIEFOS_O_EXCL)) == (RELIEFOS_O_CREAT | RELIEFOS_O_EXCL))
                    return -RELIEFOS_EEXIST;
                uint32_t access = (flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_RDONLY ? FS_ACCESS_READ :
                    (flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_WRONLY ? FS_ACCESS_WRITE : FS_ACCESS_READ | FS_ACCESS_WRITE;
                ret = fs_permissions_check(task, path, access, false);
                if (ret < 0) return ret;
                if (flags & RELIEFOS_O_DIRECTORY) return -RELIEFOS_ENOTDIR;
                ret = task_pty_endpoint_fd(task, pty_id, TASK_PTY_ENDPOINT_SLAVE, flags);
                if (ret >= 0 && !(flags & LINUX_O_NOCTTY))
                    pty_open_controlling(pty_id, task->pid, (flags & RELIEFOS_O_ACCMODE) != RELIEFOS_O_WRONLY);
                return ret;
            }
        }
        if ((!__builtin_strncmp(path,"/proc",5) && (!path[5] || path[5]=='/')) ||
            (!__builtin_strncmp(path,"/sys",4) && (!path[4] || path[4]=='/'))) {
            struct storage_node proc_node;
            if (proc_lookup(path, &proc_node) == 0) {
                ret = fs_permissions_check(task, path, FS_ACCESS_READ, false);
                if (ret < 0) return ret;
                if ((flags & RELIEFOS_O_ACCMODE) != RELIEFOS_O_RDONLY || (flags & RELIEFOS_O_TRUNC))
                    return -RELIEFOS_EROFS;
                int fd = alloc_task_fd(task, &proc_node, flags, path);
                if (fd >= 0) {
                    struct task_file *proc_file = task_file_for_fd(task, fd);
                    if (proc_file) proc_file->node = proc_node;
                }
                return fd;
            }
            return -RELIEFOS_ENOENT;
        }
        ret = fs_permissions_search(task, path, false);
        if (ret < 0) return ret;
        ret = storage_lookup_path(path, &node);
        if (ret < 0) {
            if (ret == -2 && (flags & RELIEFOS_O_CREAT)) {
                ret = fs_permissions_parent(task, path, false);
                if (ret < 0) return ret;
                ret = storage_write_file(path, "", 0);
                if (ret < 0) {
                    return ret;
                }
                created = 1;
                ret = storage_lookup_path(path, &node);
                if (!ret) ret = fs_permissions_create(task, path, &node, mode);
                if (ret < 0) {
                    (void)storage_unlink(path);
                    return ret;
                }
            }
            if (ret < 0) {
                return ret == -2 ? -RELIEFOS_ENOENT : ret;
            }
        }
        if ((flags & (RELIEFOS_O_CREAT | RELIEFOS_O_EXCL)) ==
            (RELIEFOS_O_CREAT | RELIEFOS_O_EXCL) && !created) {
            return -RELIEFOS_EEXIST;
        }
        if (node.type == RELIEFOS_FS_TYPE_SYMLINK) return -LINUX_ELOOP;
        if (!created) {
            uint32_t access = (flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_RDONLY ? FS_ACCESS_READ :
                              (flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_WRONLY ? FS_ACCESS_WRITE :
                              FS_ACCESS_READ | FS_ACCESS_WRITE;
            if (flags & RELIEFOS_O_TRUNC) access |= FS_ACCESS_WRITE;
            ret = fs_permissions_check(task, path, access, false);
            if (ret < 0) return ret;
        }
        if ((flags & RELIEFOS_O_DIRECTORY) && node.type != RELIEFOS_FS_TYPE_DIR) {
            return -RELIEFOS_ENOTDIR;
        }
        if (!(flags & TASK_FILE_FLAG_PATH) && (node.flags & (STORAGE_NODE_FLAG_EXT2 | STORAGE_NODE_FLAG_TMPFS))) {
            struct linux_stat_abi inode_stat;
            ret = storage_inode_stat(&node, &inode_stat);
            if (ret < 0) return ret;
            if ((inode_stat.st_mode & LINUX_S_IFMT) == LINUX_S_IFIFO)
                return task_fifo_open(task, &node, flags, path);
        }
        if (node.type == RELIEFOS_FS_TYPE_SOCKET) return -LINUX_ENXIO;
        if (node.type == RELIEFOS_FS_TYPE_FILE) {
            struct reliefos_permissions value;
            ret = fs_permissions_get(path, &node, &value);
            if (ret < 0) return ret;
            if ((value.mode & LINUX_S_IFMT) == LINUX_S_IFSOCK) return -LINUX_ENXIO;
        }
        if (node.flags & STORAGE_NODE_FLAG_DEV_NODE) {
            if (node.first_cluster == STORAGE_DEV_KIND_DRM_CARD0 &&
                (flags & LINUX_O_EXCL)) return -LINUX_EBUSY;
            uint32_t device_flags = TASK_FILE_FLAG_DEV_NODE;
            if (node.flags & STORAGE_NODE_FLAG_DEV_BLOCK) {
                if (flags & LINUX_O_EXCL) {
                    int busy = storage_disk_block_busy(STORAGE_BLOCK_DISK_ID(node.volume_id),
                                                       STORAGE_BLOCK_PARTITION(node.volume_id));
                    if (busy) return busy < 0 ? busy : -LINUX_EBUSY;
                }
                device_flags |= TASK_FILE_FLAG_DEV_BLOCK;
            }
            int fd = alloc_task_fd(task, &node,
                                   flags | device_flags, path);
            if (fd >= 0 &&
                (node.first_cluster == STORAGE_DEV_KIND_DRM_CARD0 ||
                 node.first_cluster == STORAGE_DEV_KIND_DRM_RENDERD128) &&
                !(flags & TASK_FILE_FLAG_PATH)) {
                struct task_file *file = task_file_for_fd(task, fd);
                int drm_ret = drm_file_open(task, file);
                if (drm_ret < 0) {
                    if (file) clear_task_file(file);
                    return drm_ret;
                }
            }
            if (fd >= 0 && (node.flags & STORAGE_NODE_FLAG_AUDIO_PCM)) {
                struct task_file *file = task_file_for_fd(task, fd);
                int audio_ret = audio_device_open(task, file);
                if (audio_ret < 0) {
                    if (file) clear_task_file(file);
                    return audio_ret;
                }
            }
            if (fd >= 0 && (node.flags & STORAGE_NODE_FLAG_AUDIO_CONTROL)) {
                struct task_file *file = task_file_for_fd(task, fd);
                int control_ret = audio_control_open(task, file);
                if (control_ret < 0) {
                    if (file) clear_task_file(file);
                    return control_ret;
                }
            }
            if (fd >= 0 && (node.flags & STORAGE_NODE_FLAG_AUDIO_MIXER)) {
                struct task_file *file = task_file_for_fd(task, fd);
                int mixer_ret = audio_mixer_open(task, file);
                if (mixer_ret < 0) {
                    if (file) clear_task_file(file);
                    return mixer_ret;
                }
            }
            if (fd >= 0 && (node.flags & STORAGE_NODE_FLAG_AUDIO_TIMER)) {
                struct task_file *file = task_file_for_fd(task, fd);
                int timer_ret = audio_timer_open(task, file);
                if (timer_ret < 0) {
                    if (file) clear_task_file(file);
                    return timer_ret;
                }
            }
            if (fd >= 0 && node.first_cluster == STORAGE_DEV_KIND_AUDIO &&
                !(node.flags & (STORAGE_NODE_FLAG_AUDIO_PCM |
                                STORAGE_NODE_FLAG_AUDIO_CONTROL |
                                STORAGE_NODE_FLAG_AUDIO_MIXER))) {
                struct task_file *file = task_file_for_fd(task, fd);
                int oss_ret = audio_oss_open(task, file);
                if (oss_ret < 0) {
                    if (file) clear_task_file(file);
                    return oss_ret;
                }
            }
            if (fd >= 0 && node.first_cluster == STORAGE_DEV_KIND_SHM) {
                struct task_file *file = task_file_for_fd(task, fd);
                if (!file || task_shm_attach(file, 0) < 0) {
                    if (file) clear_task_file(file);
                    return -RELIEFOS_ENOMEM;
                }
            }
            if (fd >= 0 && (node.first_cluster == STORAGE_DEV_KIND_KEYBOARD ||
                            node.first_cluster == STORAGE_DEV_KIND_MOUSE)) {
                struct task_file *file = task_file_for_fd(task, fd);
                if (file) {
                    file->aux = input_evdev_cursor_now();
                    /* Bind graphical consumers to their inherited VT even
                     * when Xorg opens evdev before KDSETMODE(KD_GRAPHICS). */
                    file->input_vt = task_evdev_vt_hint(task);
                }
            }
            return fd;
        }
        if (node.type == RELIEFOS_FS_TYPE_DIR && ((flags & RELIEFOS_O_ACCMODE) != RELIEFOS_O_RDONLY)) {
            return -RELIEFOS_EISDIR;
        }
        if (node.type == RELIEFOS_FS_TYPE_FILE && (flags & RELIEFOS_O_TRUNC) && file_can_write(&(struct task_file){.flags = flags})) {
            ret = storage_write_file(path, "", 0);
            if (ret < 0) {
                return ret;
            }
            ret = storage_lookup_path(path, &node);
            if (ret < 0) {
                return ret == -2 ? -RELIEFOS_ENOENT : ret;
            }
        }
        int fd = alloc_task_fd(task, &node, flags, path);
        if (fd >= 0 && (flags & RELIEFOS_O_APPEND)) {
            struct task_file *file = task_file_for_fd(task, fd);
            if (file && file->node.type == RELIEFOS_FS_TYPE_FILE) {
                file->offset = file->node.size;
            }
        }
        return fd;
    }

    if (number == LINUX_SYS_PIPE) {
        return syscall_ipc_pipe(a0);
    }

    if (number == LINUX_SYS_PIPE2) {
        return syscall_ipc_pipe2(a0, a1);
    }

    if (number == LINUX_SYS_CLOSE) {
        struct task *task = sched_current_task();
        /* Linux close takes unsigned int, including on native x86-64. */
        uint32_t fd = (uint32_t)a0;
        if (!task) return -RELIEFOS_EBADF;
        if (fd < SCHED_TASK_STDIO_MAX) {
            int result = 0;
            struct task_file *stdio_file = task_descriptor_for_fd(task, (int)fd);
            struct task_pty_fd *pty_fd = task_pty_fd_for_fd(task, (int)fd);
            if (stdio_file) {
                task_epoll_remove_fd(task, (int)fd);
                syscall_record_locks_close(task, stdio_file);
                result = clear_task_file_result(stdio_file);
            } else if (pty_fd) {
                task_pty_release_entry(pty_fd);
            } else if (sched_task_fds(task)->closed_stdio_mask & (1u << fd)) {
                return -RELIEFOS_EBADF;
            }
            /* The inherited PTY/console streams have no task_file entry.
             * Closing one must still release its descriptor number so that
             * open("/dev/null") after close(0) returns zero. */
            sched_task_fds(task)->closed_stdio_mask |= 1u << fd;
            return result;
        }
        struct task_file *descriptor = task_descriptor_for_fd(task, (int)fd);
        if (descriptor) {
            task_epoll_remove_fd(task, (int)fd);
            syscall_record_locks_close(task, descriptor);
            return clear_task_file_result(descriptor);
        }
        {
            struct task_pty_fd *pty_fd = task_pty_fd_for_fd(task, (int)fd);
            if (pty_fd) {
                task_pty_release_entry(pty_fd);
                return 0;
            }
        }
        return -RELIEFOS_EBADF;
    }

    if (number == LINUX_SYS_FTRUNCATE) {
        struct task *task = sched_current_task();
        struct task_file *file;
        int ret;
        if ((int64_t)a1 < 0) {
            return -RELIEFOS_EINVAL;
        }
        file = task_file_for_fd(task, (int)a0);
        if (!file) {
            return -RELIEFOS_EBADF;
        }
        if (file->flags & TASK_FILE_FLAG_DEV_SHM) {
            return task_shm_truncate(file, (uint64_t)a1);
        }
        if (file->node.type != RELIEFOS_FS_TYPE_FILE || !file_can_write(file) || !file->path[0]) {
            return -RELIEFOS_EBADF;
        }
        ret = file->inode ? storage_truncate_held_node(&file->node, a1) :
            storage_truncate_file(file->path, a1);
        if (ret < 0) {
            return ret;
        }
        file->read_cursor.valid = 0;
        ret = file->inode ? 0 : storage_lookup_path(file->path, &file->node);
        if (ret < 0) {
            return ret;
        }
        return 0;
    }

    if (number == LINUX_SYS_DUP3) {
        struct task *task = sched_current_task();
        struct task_file *file;
        int result;
        a2 = (uint32_t)a2;
        if ((int)a0 == (int)a1) return -RELIEFOS_EINVAL;
        if (a2 & ~(uint64_t)RELIEFOS_O_CLOEXEC) return -RELIEFOS_EINVAL;
        if (task_file_for_fd(task, (int)a0)) {
            result = task_dup2_fd(task, (int)a0, (int)a1);
            if (result < 0) return result;
            file = task_descriptor_for_fd(task, result);
            if (file) file->fd_flags = (a2 & RELIEFOS_O_CLOEXEC) ? RELIEFOS_FD_CLOEXEC : 0;
            return result;
        }
        result = task_pty_dup2_fd(task, (int)a0, (int)a1);
        if (result < 0) return result;
        struct task_pty_fd *pty_fd = task_pty_fd_for_fd(task, result);
        if (pty_fd) pty_fd->flags = (a2 & RELIEFOS_O_CLOEXEC) ? RELIEFOS_FD_CLOEXEC : 0;
        return result;
    }

    if (number == LINUX_SYS_DUP || number == LINUX_SYS_DUP2 || number == LINUX_SYS_FCNTL) {
        struct task *task = sched_current_task();
        /* fcntl's command is unsigned int; its argument remains unsigned long. */
        if (number == LINUX_SYS_FCNTL) a1 = (uint32_t)a1;
        if (number == LINUX_SYS_FCNTL && (a1 == LINUX_F_GETLK || a1 == LINUX_F_SETLK || a1 == LINUX_F_SETLKW))
            return syscall_record_lock((int32_t)a0, (uint32_t)a1, a2);
        if (number == LINUX_SYS_DUP) {
            struct task_file *source = task_file_for_fd(task, (int)a0);
            if (source) {
                if (!sched_task_limits(task)->nofile.rlim_cur) return -RELIEFOS_EMFILE;
                return task_duplicate_file_fd(task, (int)a0, 0, 0);
            }
            if (task && !sched_task_limits(task)->nofile.rlim_cur &&
                (task_pty_fd_for_fd(task, (int)a0) || task_pty_stream_for_fd(task, (int)a0) >= 0))
                return -RELIEFOS_EMFILE;
            return task_pty_duplicate_fd(task, (int)a0, 0, 0);
        }
        if (number == LINUX_SYS_DUP2) {
            if (task_file_for_fd(task, (int)a0)) {
                return task_dup2_fd(task, (int)a0, (int)a1);
            }
            return task_pty_dup2_fd(task, (int)a0, (int)a1);
        }
        if (a1 == RELIEFOS_F_DUPFD || a1 == RELIEFOS_F_DUPFD_CLOEXEC) {
            if (task_file_for_fd(task, (int)a0)) {
                return task_duplicate_file_fd(task, (int)a0, (int)a2,
                                              a1 == RELIEFOS_F_DUPFD_CLOEXEC ?
                                                  RELIEFOS_FD_CLOEXEC : 0);
            }
            return task_pty_duplicate_fd(task, (int)a0, (int)a2,
                                         a1 == RELIEFOS_F_DUPFD_CLOEXEC ? RELIEFOS_FD_CLOEXEC : 0);
        }
        {
            struct task_file *file_fd = task_file_for_fd(task, (int)a0);
            struct task_pty_fd *pty_fd = task_pty_fd_for_fd(task, (int)a0);
            /* Descriptor flags live in the descriptor table entry, never in
             * the shared open file description, and use the same storage as
             * ioctl(FIOCLEX/FIONCLEX) so the two interfaces cannot disagree.
             * Implicit stdin/stdout/stderr descriptors are covered as well. */
            if (a1 == RELIEFOS_F_GETFD) {
                return task_fd_descriptor_flags(task, (int)a0);
            }
            if (a1 == RELIEFOS_F_SETFD) {
                int ret = task_fd_set_descriptor_flags(task, (int)a0, (uint32_t)a2);
                return ret < 0 ? ret : 0;
            }
            if (!file_fd && !pty_fd && task_pty_stream_for_fd(task, (int)a0) < 0) {
                return -RELIEFOS_EBADF;
            }
            if (a1 == RELIEFOS_F_GETFL) {
                if (file_fd) {
                    return (int64_t)(file_fd->flags &
                        (RELIEFOS_O_ACCMODE | RELIEFOS_O_APPEND | RELIEFOS_O_NONBLOCK));
                }
                if (pty_fd) {
                    return (int64_t)task_pty_status(pty_fd);
                }
                /* Implicit controlling-TTY stdio descriptors are readable
                 * (fd 0) or writable (fd 1/2). */
                return a0 == 0 ? (int64_t)RELIEFOS_O_RDONLY
                               : (int64_t)RELIEFOS_O_WRONLY;
            }
            if (a1 == RELIEFOS_F_SETFL) {
                if (file_fd) {
                    /* Preserve descriptor-kind/device bits (PIPE, SOCKET_*,
                     * DEV_*) while updating only the open-status flags. */
                    file_fd->flags = (file_fd->flags &
                                      ~(RELIEFOS_O_APPEND | RELIEFOS_O_NONBLOCK)) |
                                     (file_fd->flags & RELIEFOS_O_ACCMODE) |
                                     ((uint32_t)a2 & (RELIEFOS_O_APPEND |
                                                      RELIEFOS_O_NONBLOCK));
                } else {
                    int ret = task_pty_ensure_fd(task, (int)a0, &pty_fd);
                    if (ret < 0) return ret;
                    pty_fd->description->status_flags = (task_pty_status(pty_fd) & RELIEFOS_O_ACCMODE) |
                        ((uint32_t)a2 & (RELIEFOS_O_APPEND | RELIEFOS_O_NONBLOCK));
                }
                return 0;
            }
            if (a1 == LINUX_F_GET_SEALS || a1 == LINUX_F_ADD_SEALS) {
                /* Linux memfd_fcntl: sealing is a shmem-file interface; pty
                 * and stream descriptors are not sealable and report EINVAL. */
                if (!file_fd) return -RELIEFOS_EINVAL;
                return a1 == LINUX_F_GET_SEALS
                    ? task_shm_get_seals(file_fd)
                    : task_shm_add_seals(file_fd, (uint32_t)a2);
            }
        }
        return -RELIEFOS_ENOSYS;
    }

    if (number == LINUX_SYS_FLOCK) {
        return syscall_flock(a0, (uint32_t)a1);
    }

    if (number == LINUX_SYS_CLOSE_RANGE) {
        struct task *task = sched_current_task();
        uint32_t first = (uint32_t)a0;
        uint32_t last = (uint32_t)a1;
        uint32_t flags = (uint32_t)a2;
        if (!task) return -RELIEFOS_EPERM;
        if (last < first || (flags & ~6u)) return -RELIEFOS_EINVAL;
        if (flags & 2u) {
            int unshare_ret = syscall_unshare_task_files(task);
            if (unshare_ret < 0) return unshare_ret;
        }
        /* Linux accepts UINT_MAX as the open-ended upper bound. Iterate only
         * materialized descriptor slots; PTY aliases live in a separate small
         * table and are handled independently below. */
        uint32_t stdio_last = last < 2u ? last : 2u;
        for (uint32_t fd = first; fd <= stdio_last; ++fd) {
            if (flags & 4u) {
                sched_task_fds(task)->cloexec_stdio_mask |= 1u << fd;
            } else {
                (void)syscall_dispatch_regs_legacy(LINUX_SYS_CLOSE, fd, 0, 0, 0, 0, 0);
            }
        }
        uint32_t file_first = first < 3u ? 3u : first;
        uint32_t file_capacity = sched_task_file_capacity(task);
        uint64_t file_end = (uint64_t)file_capacity + 2u;
        uint32_t file_last = last < file_end ? last :
                             file_end > UINT32_MAX ? UINT32_MAX : (uint32_t)file_end;
        if (file_first <= file_last) {
            for (uint64_t fd = file_first; fd <= file_last; ++fd) {
                if (flags & 4u) {
                    struct task_file *descriptor = task_descriptor_for_fd(task, (int)fd);
                    if (descriptor) descriptor->fd_flags |= RELIEFOS_FD_CLOEXEC;
                } else {
                    (void)syscall_dispatch_regs_legacy(LINUX_SYS_CLOSE, fd, 0, 0, 0, 0, 0);
                }
            }
        }
        for (uint32_t i = 0; i < SCHED_TASK_PTY_FD_MAX; ++i) {
            struct task_pty_fd *pty_fd = &sched_task_fds(task)->pty_fds[i];
            if (!pty_fd->used || pty_fd->fd < 0 || (uint32_t)pty_fd->fd < first ||
                (uint32_t)pty_fd->fd > last) continue;
            if (flags & 4u) pty_fd->flags |= RELIEFOS_FD_CLOEXEC;
            else (void)syscall_dispatch_regs_legacy(LINUX_SYS_CLOSE,
                                                     (uint32_t)pty_fd->fd, 0, 0, 0, 0, 0);
        }
        return 0;
    }

    return -RELIEFOS_ENOSYS;
}
