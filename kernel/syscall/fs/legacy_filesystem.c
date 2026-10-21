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
int64_t syscall_legacy_filesystem(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    if (number == LINUX_SYS_UTIME || number == LINUX_SYS_UTIMES ||
        number == LINUX_SYS_FUTIMESAT || number == LINUX_SYS_UTIMENSAT) {
        struct task *task = sched_current_task();
        struct linux_timespec times[2];
        uint64_t path_ptr = a0;
        int32_t dirfd = LINUX_AT_FDCWD;
        uint32_t flags = 0;
        if (number == LINUX_SYS_FUTIMESAT) {
            dirfd = (int32_t)a0;
            path_ptr = a1;
        } else if (number == LINUX_SYS_UTIMENSAT) {
            dirfd = (int32_t)a0;
            path_ptr = a1;
            flags = (uint32_t)a3;
        }
        bool times_supplied = true;
        if (number == LINUX_SYS_UTIMENSAT) {
            times_supplied = a2 != 0;
            if (times_supplied) {
                if (!user_range_ok(a2, sizeof(times))) return -RELIEFOS_EFAULT;
                times[0] = ((const struct linux_timespec *)(uintptr_t)a2)[0];
                times[1] = ((const struct linux_timespec *)(uintptr_t)a2)[1];
            } else {
                times[0].tv_sec = times[1].tv_sec = 0;
                times[0].tv_nsec = times[1].tv_nsec = 1073741823;
            }
        } else if (number == LINUX_SYS_UTIMES || number == LINUX_SYS_FUTIMESAT) {
            uint64_t times_ptr = number == LINUX_SYS_FUTIMESAT ? a2 : a1;
            times_supplied = times_ptr != 0;
            if (!times_supplied) {
                times[0].tv_nsec = times[1].tv_nsec = 1073741823;
                times[0].tv_sec = times[1].tv_sec = 0;
            } else {
                struct linux_timeval values[2];
                if (!user_range_ok(times_ptr, sizeof(values))) return -RELIEFOS_EFAULT;
                values[0] = ((const struct linux_timeval *)(uintptr_t)times_ptr)[0];
                values[1] = ((const struct linux_timeval *)(uintptr_t)times_ptr)[1];
                for (unsigned i = 0; i < 2; ++i) {
                    if (values[i].tv_usec < 0 || values[i].tv_usec > 999999)
                        return -RELIEFOS_EINVAL;
                    times[i].tv_sec = values[i].tv_sec;
                    times[i].tv_nsec = values[i].tv_usec * 1000;
                }
            }
        } else {
            struct { int64_t actime, modtime; } value;
            if (!a1) {
                times[0].tv_nsec = times[1].tv_nsec = 1073741823;
                times[0].tv_sec = times[1].tv_sec = 0;
            } else {
                if (!user_range_ok(a1, sizeof(value))) return -RELIEFOS_EFAULT;
                __builtin_memcpy(&value, (const void *)(uintptr_t)a1, sizeof(value));
                times[0] = (struct linux_timespec){value.actime, 0};
                times[1] = (struct linux_timespec){value.modtime, 0};
            }
        }
        for (unsigned i = 0; i < 2; ++i)
            if (times[i].tv_nsec < 0 ||
                (times[i].tv_nsec > 999999999 &&
                 !(times[i].tv_nsec == 1073741823 ||
                   (number == LINUX_SYS_UTIMENSAT && times[i].tv_nsec == 1073741822))))
                return -RELIEFOS_EINVAL;
        if (number == LINUX_SYS_UTIMENSAT && times_supplied &&
            times[0].tv_nsec == 1073741822 && times[1].tv_nsec == 1073741822)
            return 0;
        return syscall_update_path_times(task, dirfd, path_ptr, times, flags, times_supplied);
    }

    if (number == LINUX_SYS_OPENAT2) {
        return syscall_openat2(a0, a1, a2, a3);
    }

    if (number == LINUX_SYS_FADVISE64) {
        struct task *task = sched_current_task();
        if ((int64_t)a1 < 0 || (int64_t)a2 < 0) return -RELIEFOS_EINVAL;
        struct task_file *file = task_file_for_fd(task, (int)a0);
        if (!file) return -RELIEFOS_EBADF;
        if (a3 > 5) return -RELIEFOS_EINVAL;
        return 0;
    }

    if (number == LINUX_SYS_READAHEAD) {
        struct task_file *file = task_file_for_fd(sched_current_task(), (int)a0);
        if (!file) return -RELIEFOS_EBADF;
        if ((int64_t)a1 < 0) return -RELIEFOS_EINVAL;
        if (file->node.type != RELIEFOS_FS_TYPE_FILE &&
            !(file->flags & TASK_FILE_FLAG_DEV_BLOCK)) return -LINUX_ESPIPE;
        return 0;
    }

    if (number == LINUX_SYS_FALLOCATE) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_fd(task, (int)a0);
        uint64_t end;
        if (!file) return -RELIEFOS_EBADF;
        if ((int64_t)a2 < 0 || (int64_t)a3 < 0 || !a3 || a2 > UINT64_MAX - a3) {
            return -RELIEFOS_EINVAL;
        }
        if (file->node.type != RELIEFOS_FS_TYPE_FILE || !file_can_write(file) || !file->path[0]) {
            return -RELIEFOS_EBADF;
        }
        end = a2 + a3;
        if (file->node.flags & STORAGE_NODE_FLAG_EXT_FAMILY) {
            if (a1>UINT32_MAX) return -RELIEFOS_EOPNOTSUPP;
            int ret=storage_fallocate_node(&file->node,(uint32_t)a1,a2,a3);
            if (!ret) file->read_cursor.valid=0;
            return ret;
        }
        if (a1 != 0) return -RELIEFOS_EOPNOTSUPP;
        if (end <= file->node.size) return 0;
        /* Storage backends have no sparse/preallocation primitive. Extending
         * with mode zero uses the existing truncate zero-fill contract. */
        int ret = file->inode ? storage_truncate_held_node(&file->node, end) :
            storage_truncate_file(file->path, end);
        if (ret < 0) return ret;
        if (!file->inode && storage_lookup_path(file->path, &file->node) < 0) return -RELIEFOS_EIO;
        file->read_cursor.valid = 0;
        return 0;
    }

    if (number == LINUX_SYS_SYNC_FILE_RANGE) {
        struct task_file *file = task_file_for_fd(sched_current_task(), (int)a0);
        if (!file) return -RELIEFOS_EBADF;
        if ((int64_t)a1 < 0 || (int64_t)a2 < 0 || (a3 & ~7u)) {
            return -RELIEFOS_EINVAL;
        }
        if (file->node.type != RELIEFOS_FS_TYPE_FILE &&
            !(file->flags & TASK_FILE_FLAG_DEV_BLOCK)) {
            return -RELIEFOS_EINVAL;
        }
        /* Storage writes are synchronous before write(2) returns. Valid
         * Linux wait/write flags therefore complete here without a second
         * backend operation; the descriptor, range and flag contract still
         * applies at the syscall boundary. */
        return 0;
    }

    if (number == LINUX_SYS_SYNC) {
        int ret = storage_sync_all();
        if (ret < 0) console_error("[storage] sync failed ret=%d\n", ret);
        return 0;
    }

    if (number == LINUX_SYS_SYNCFS) {
        struct task_file *file = task_file_for_fd(sched_current_task(), (int)a0);
        if (!file) return -RELIEFOS_EBADF;
        if (file->node.flags & (STORAGE_NODE_FLAG_PROC | STORAGE_NODE_FLAG_SYSFS)) return 0;
        return storage_sync_volume(file->node.volume_id);
    }

    if (number == LINUX_SYS_FCHMODAT || number == LINUX_SYS_FCHMODAT2 ||
        number == LINUX_SYS_FCHOWNAT || number == LINUX_SYS_FACCESSAT ||
        number == LINUX_SYS_FACCESSAT2) {
        struct task *task = sched_current_task();
        struct task_file *empty_file;
        char path[RELIEFOS_FS_PATH_LEN];
        uint32_t flags = number == LINUX_SYS_FCHOWNAT ? (uint32_t)a4 :
                         (number == LINUX_SYS_FCHMODAT2 || number == LINUX_SYS_FACCESSAT2) ? (uint32_t)a3 : 0;
        uint32_t supported = LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH;
        if (number == LINUX_SYS_FACCESSAT2) supported |= LINUX_AT_EACCESS;
        if (flags & ~supported) return -RELIEFOS_EINVAL;
        bool real_ids = (number == LINUX_SYS_FACCESSAT || number == LINUX_SYS_FACCESSAT2) &&
                        !(flags & LINUX_AT_EACCESS);
        int ret = resolve_user_path_at_flags(task, (int32_t)a0, a1, flags & LINUX_AT_EMPTY_PATH,
                    path, &empty_file, real_ids, flags & LINUX_AT_SYMLINK_NOFOLLOW ? 0 : FS_LOOKUP_FOLLOW);
        if (ret < 0) return ret;
        const struct storage_node *node = empty_file ? &empty_file->node : NULL;
        if (number == LINUX_SYS_FCHOWNAT) return fs_permissions_chown(task, path, node, (uint32_t)a2, (uint32_t)a3);
        if (number == LINUX_SYS_FCHMODAT || number == LINUX_SYS_FCHMODAT2)
            return fs_permissions_chmod(task, path, node, (uint32_t)a2);
        if (a2 & ~7u) return -RELIEFOS_EINVAL;
        return fs_permissions_check(task, path, (uint32_t)a2, !(flags & LINUX_AT_EACCESS));
    }

    if (number == LINUX_SYS_GETGROUPS || number == LINUX_SYS_SETGROUPS) {
        struct task *task = sched_current_task();
        int32_t size = (int32_t)a0;
        if (!task) return -RELIEFOS_EPERM;
        if (number == LINUX_SYS_SETGROUPS) {
            if (!(task->cap_effective & (1ULL << CAP_SETGID))) return -RELIEFOS_EPERM;
            if (size < 0 || size > 65536) return -RELIEFOS_EINVAL;
            if (size && !user_range_ok(a1, (uint64_t)size * 4)) return -RELIEFOS_EFAULT;
            return task_groups_set(task, (const uint32_t *)(uintptr_t)a1, (uint32_t)size);
        }
        uint32_t count = task->groups ? task->groups->count : 0;
        if (size < 0) return -RELIEFOS_EINVAL;
        if (!size) return count;
        if ((uint32_t)size < count) return -RELIEFOS_EINVAL;
        if (count && !user_range_writable(a1, (uint64_t)count * 4)) return -RELIEFOS_EFAULT;
        for (uint32_t i = 0; i < count; ++i) ((uint32_t *)(uintptr_t)a1)[i] = task->groups->ids[i];
        return count;
    }

    if (number == LINUX_SYS_CHMOD || number == LINUX_SYS_CHOWN || number == LINUX_SYS_LCHOWN) {
        struct task *task = sched_current_task();
        char path[RELIEFOS_FS_PATH_LEN];
        int ret = resolve_user_path_at_flags(task, LINUX_AT_FDCWD, a0, false, path, NULL,
                                             false, number == LINUX_SYS_LCHOWN ? 0 : FS_LOOKUP_FOLLOW);
        if (ret < 0) return ret;
        return number == LINUX_SYS_CHMOD
                   ? fs_permissions_chmod(task, path, NULL, (uint32_t)a1)
                   : fs_permissions_chown(task, path, NULL, (uint32_t)a1, (uint32_t)a2);
    }

    if (number == LINUX_SYS_FCHMOD || number == LINUX_SYS_FCHOWN) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_fd(task, (int)a0);
        if (!file) {
            struct storage_node node;
            const char *path;
            int ret = task_pty_node_for_fd(task, (int)a0, &node, &path);
            if (ret < 0) return ret;
            return number == LINUX_SYS_FCHMOD ? fs_permissions_chmod(task, path, &node, (uint32_t)a1) :
                fs_permissions_chown(task, path, &node, (uint32_t)a1, (uint32_t)a2);
        }
        if (!file || !file->path[0]) return -RELIEFOS_EBADF;
        return number == LINUX_SYS_FCHMOD
                   ? fs_permissions_chmod(task, file->path, &file->node, (uint32_t)a1)
                   : fs_permissions_chown(task, file->path, &file->node, (uint32_t)a1, (uint32_t)a2);
    }

    if (number == __NR_mknod || number == __NR_mknodat) {
        struct task *task = sched_current_task();
        uint32_t mode = (uint32_t)(number == __NR_mknodat ? a2 : a1);
        uint32_t type = mode & LINUX_S_IFMT;
        if (!type) type = LINUX_S_IFREG;
        if (type != LINUX_S_IFREG && type != LINUX_S_IFIFO && type != LINUX_S_IFSOCK)
            return type == LINUX_S_IFCHR || type == LINUX_S_IFBLK ? -LINUX_EOPNOTSUPP : -LINUX_EINVAL;
        char path[RELIEFOS_FS_PATH_LEN];
        int ret = resolve_user_path_at_flags(task, number == __NR_mknodat ? (int32_t)a0 : LINUX_AT_FDCWD,
            number == __NR_mknodat ? a1 : a0, false, path, NULL, false, FS_LOOKUP_PARENT);
        if (!ret) ret = mutation_path_slash(path, true);
        if (!ret) ret = fs_permissions_parent(task, path, false);
        if (ret < 0) return ret;
        struct storage_node node;
        ret = storage_lookup_path(path, &node);
        if (!ret) return -LINUX_EEXIST;
        if (ret != -LINUX_ENOENT) return ret;
        if (type == LINUX_S_IFREG) {
            ret = storage_write_file(path, "", 0);
            if (!ret) ret = storage_lookup_path(path, &node);
        } else ret = storage_create_special(path, type, &node);
        if (ret < 0) return ret;
        ret = fs_permissions_create(task, path, &node, mode);
        if (ret < 0) (void)storage_unlink(path);
        return ret;
    }

    if (number == LINUX_SYS_STATFS || number == LINUX_SYS_FSTATFS) {
        struct task *task = sched_current_task();
        struct linux_statfs_abi value = {0};
        struct storage_node node;
        char path[RELIEFOS_FS_PATH_LEN];
        int ret = 0;
        bool synthetic = false;
        if (number == LINUX_SYS_FSTATFS) {
            struct task_file *file = task_file_for_fd(task, (int32_t)a0);
            if (file) {
                node = file->node;
                copy_text(path, sizeof(path), file->path);
                if ((file->flags & TASK_FILE_FLAG_PIPE) && !file->inode) { value.f_type = 0x50495045; synthetic = true; }
                if (file->flags & (TASK_FILE_FLAG_SOCKET_UNIX | TASK_FILE_FLAG_SOCKET_INET)) {
                    value.f_type = 0x534f434b; synthetic = true;
                }
            } else if (task_pty_fd_for_fd(task, (int32_t)a0) || task_pty_stream_for_fd(task, (int32_t)a0) >= 0) {
                value.f_type = 0x1cd1; synthetic = true;
                path[0] = 0;
            } else return -RELIEFOS_EBADF;
        } else {
            ret = resolve_user_path(task, a0, path, sizeof(path));
            if (!ret) ret = fs_permissions_search(task, path, false);
            if (ret < 0) return ret;
            if (proc_lookup(path, &node) < 0) ret = storage_lookup_path(path, &node);
            if (ret < 0) return ret;
        }
        struct storage_node proc;
        if (path[0] && proc_lookup(path, &proc) == 0) { value.f_type = (proc.flags & STORAGE_NODE_FLAG_SYSFS) ? 0x62656572 : 0x9fa0; synthetic = true; }
        if (synthetic) {
            value.f_bsize = value.f_frsize = 4096;
            value.f_namelen = 255;
            value.f_flags = LINUX_ST_VALID | (value.f_type == 0x62656572 ? LINUX_ST_RDONLY : 0);
        } else ret = storage_statfs(&node, &value);
        if (ret < 0) return ret;
        if (!user_range_writable(a1, sizeof(value))) return -RELIEFOS_EFAULT;
        *(struct linux_statfs_abi *)(uintptr_t)a1 = value;
        return 0;
    }

    if (number == LINUX_SYS_STAT || number == LINUX_SYS_LSTAT || number == LINUX_SYS_NEWFSTATAT) {
        struct task *task = sched_current_task();
        struct reliefos_stat st;
        struct linux_stat_abi linux_st;
        char path[RELIEFOS_FS_PATH_LEN];
        int ret;
        if (number == LINUX_SYS_NEWFSTATAT) {
            struct task_file *empty_file;
            if ((uint32_t)a3 & ~(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH | LINUX_AT_NO_AUTOMOUNT))
                return -RELIEFOS_EINVAL;
            ret = resolve_user_path_at_flags(task, (int32_t)a0, a1, a3 & LINUX_AT_EMPTY_PATH,
                    path, &empty_file, false, a3 & LINUX_AT_SYMLINK_NOFOLLOW ? 0 : FS_LOOKUP_FOLLOW);
            if (ret < 0) return ret;
            if (empty_file) return syscall_dispatch_regs_legacy(LINUX_SYS_FSTAT, a0, a2, 0, 0, 0, 0);
            a1 = a2;
        } else {
            ret = resolve_user_path_at_flags(task, LINUX_AT_FDCWD, a0, false, path, NULL,
                                             false, number == LINUX_SYS_LSTAT ? 0 : FS_LOOKUP_FOLLOW);
            if (ret < 0) return ret;
        }
        if (!user_range_writable(a1, sizeof(linux_st))) {
            return -RELIEFOS_EFAULT;
        }
        ret = fs_permissions_search(task, path, false);
        if (ret < 0) return ret;
        if (text_eq_cstr(path, "/dev/null")) {
            st.type = RELIEFOS_FS_TYPE_DEVICE;
            st.reserved = 0;
            st.size = 0;
            ret = linux_stat_from_legacy(&linux_st, &st, path, NULL);
            if (ret < 0) return ret;
            *(struct linux_stat_abi *)(uintptr_t)a1 = linux_st;
            return 0;
        }
        {
            struct storage_node proc_node;
            if (proc_lookup(path, &proc_node) == 0) {
                st.type = proc_node.type;
                st.reserved = 0;
                st.size = proc_node.size;
                ret = linux_stat_from_legacy(&linux_st, &st, path, &proc_node);
                if (ret < 0) return ret;
                *(struct linux_stat_abi *)(uintptr_t)a1 = linux_st;
                return 0;
            }
        }
        ret = storage_stat_path(path, &st);
        if (ret < 0) {
            return ret == -2 ? -RELIEFOS_ENOENT : ret;
        }
        ret = linux_stat_from_legacy(&linux_st, &st, path, NULL);
        if (ret < 0) return ret;
        *(struct linux_stat_abi *)(uintptr_t)a1 = linux_st;
        return 0;
    }

    if (number == LINUX_SYS_STATX) {
        struct task *task = sched_current_task();
        struct task_file *empty_file = NULL;
        struct reliefos_stat st = {0};
        struct linux_stat_abi linux_st;
        struct linux_statx linux_stx;
        char path[RELIEFOS_FS_PATH_LEN];
        uint32_t flags = (uint32_t)a2;
        uint32_t mask = (uint32_t)a3;
        int ret;
        const uint32_t supported_flags = LINUX_AT_SYMLINK_NOFOLLOW |
            LINUX_AT_NO_AUTOMOUNT | LINUX_AT_EMPTY_PATH | LINUX_AT_STATX_SYNC_TYPE;
        if (!mask || (flags & ~supported_flags) ||
            (flags & LINUX_AT_STATX_SYNC_TYPE) == LINUX_AT_STATX_SYNC_TYPE) {
            return -RELIEFOS_EINVAL;
        }
        if (!user_range_writable(a4, sizeof(linux_stx))) return -RELIEFOS_EFAULT;
        ret = resolve_user_path_at_flags(task, (int32_t)a0, a1,
                                   flags & LINUX_AT_EMPTY_PATH, path,
                                   &empty_file, false, flags & LINUX_AT_SYMLINK_NOFOLLOW ? 0 : FS_LOOKUP_FOLLOW);
        if (ret < 0) return ret;
        if (empty_file) {
            st.type = empty_file->node.type;
            st.size = empty_file->node.size;
            ret = linux_stat_from_legacy(&linux_st, &st,
                                         empty_file->path[0] ? empty_file->path : NULL,
                                         &empty_file->node);
            if (ret >= 0) linux_stat_fd_type(&linux_st, empty_file);
        } else {
            ret = fs_permissions_search(task, path, false);
            if (ret < 0) return ret;
            {
                struct storage_node proc_node;
                if (proc_lookup(path, &proc_node) == 0) {
                    st.type = proc_node.type;
                    st.size = proc_node.size;
                    ret = linux_stat_from_legacy(&linux_st, &st, path, &proc_node);
                } else {
                    ret = storage_stat_path(path, &st);
                    if (ret >= 0) ret = linux_stat_from_legacy(&linux_st, &st, path, NULL);
                }
            }
        }
        if (ret < 0) return ret == -2 ? -RELIEFOS_ENOENT : ret;
        linux_statx_from_legacy(&linux_stx, &linux_st, mask);
        *(struct linux_statx *)(uintptr_t)a4 = linux_stx;
        return 0;
    }

    if (number == LINUX_SYS_FSTAT) {
        struct task *task = sched_current_task();
        struct reliefos_stat st;
        struct linux_stat_abi linux_st;
        int ret;
        if (!user_range_writable(a1, sizeof(linux_st))) {
            return -RELIEFOS_EFAULT;
        }
        ret = stat_for_fd((int)a0, task, &st);
        if (ret < 0) {
            return ret;
        }
        struct task_file *file = task_file_for_fd(task, (int)a0);
        const char *path = file && file->path[0] ? file->path : NULL;
        if (file && (file->flags & (TASK_FILE_FLAG_PIPE | TASK_FILE_FLAG_SOCKET_UNIX | TASK_FILE_FLAG_SOCKET_INET))) path = NULL;
        struct storage_node pty_node;
        const struct storage_node *node = file ? &file->node : NULL;
        if (!file && task_pty_node_for_fd(task, (int)a0, &pty_node, &path) == 0) node = &pty_node;
        ret = linux_stat_from_legacy(&linux_st, &st, path, node);
        if (ret < 0) return ret;
        linux_stat_fd_type(&linux_st, file);
        *(struct linux_stat_abi *)(uintptr_t)a1 = linux_st;
        return 0;
    }

    if (number == LINUX_SYS_ACCESS) {
        struct task *task = sched_current_task();
        char path[RELIEFOS_FS_PATH_LEN];
        int ret = resolve_user_path_ids(task, a0, path, sizeof(path), true);
        if (ret < 0) return ret;
        if (a1 & ~(4u | 2u | 1u)) return -RELIEFOS_EINVAL;
        return fs_permissions_check(task, path, (uint32_t)a1, true);
    }

    if (number == LINUX_SYS_LINK || number == LINUX_SYS_LINKAT ||
        number == LINUX_SYS_SYMLINK || number == LINUX_SYS_SYMLINKAT) {
        struct task *task = sched_current_task();
        char old_path[RELIEFOS_FS_PATH_LEN];
        char new_path[RELIEFOS_FS_PATH_LEN];
        struct storage_node source;
        int32_t old_dirfd = LINUX_AT_FDCWD;
        int32_t new_dirfd = LINUX_AT_FDCWD;
        uint64_t old_ptr = a0;
        uint64_t new_ptr = a1;
        int ret;
        if (number == LINUX_SYS_SYMLINK || number == LINUX_SYS_SYMLINKAT) {
            char *target = kernel_malloc(LINUX_PATH_MAX);
            if (!target) return -RELIEFOS_ENOMEM;
            uint64_t target_ptr = a0;
            uint64_t link_ptr = a1;
            int32_t dirfd = LINUX_AT_FDCWD;
            if (number == LINUX_SYS_SYMLINKAT) {
                dirfd = (int32_t)a1;
                link_ptr = a2;
            }
            ret = copy_user_path(target, LINUX_PATH_MAX, target_ptr);
            if (!ret && !target[0]) ret = -RELIEFOS_ENOENT;
            if (!ret) ret = resolve_user_path_at_flags(task, dirfd, link_ptr, false,
                                       new_path, NULL, false, FS_LOOKUP_PARENT);
            if (!ret && mutation_path_special(new_path)) ret = -RELIEFOS_EEXIST;
            if (!ret) ret = mutation_path_slash(new_path, true);
            if (!ret) ret = fs_permissions_parent(task, new_path, false);
            if (!ret) ret = storage_symlink(target, new_path);
            kernel_free(target);
            if (ret < 0) return storage_errno(ret);
            {
                struct storage_node created;
                ret = storage_lookup_path(new_path, &created);
                if (!ret) ret = fs_permissions_create(task, new_path, &created, 0777u);
                if (ret < 0) (void)storage_unlink(new_path);
            }
            return ret < 0 ? storage_errno(ret) : 0;
        }
        if (number == LINUX_SYS_LINKAT) {
            old_dirfd = (int32_t)a0;
            old_ptr = a1;
            new_dirfd = (int32_t)a2;
            new_ptr = a3;
            if ((uint32_t)a4 & ~(LINUX_AT_EMPTY_PATH | LINUX_AT_SYMLINK_FOLLOW)) return -RELIEFOS_EINVAL;
        }
        ret = resolve_user_path_at_flags(task, old_dirfd, old_ptr,
                                   number == LINUX_SYS_LINKAT && (a4 & LINUX_AT_EMPTY_PATH),
                                   old_path, NULL, false,
                                   number == LINUX_SYS_LINKAT && (a4 & LINUX_AT_SYMLINK_FOLLOW) ? FS_LOOKUP_FOLLOW : 0);
        if (ret < 0) return ret;
        ret = resolve_user_path_at_flags(task, new_dirfd, new_ptr, false,
                                   new_path, NULL, false, FS_LOOKUP_PARENT);
        if (ret < 0) return ret;
        if (mutation_path_special(new_path)) return -RELIEFOS_EEXIST;
        ret = mutation_path_slash(new_path, true);
        if (ret < 0) return ret;
        ret = storage_lookup_path(old_path, &source);
        if (ret < 0) return storage_errno(ret);
        if (source.type == RELIEFOS_FS_TYPE_DIR) return -RELIEFOS_EPERM;
        ret = fs_permissions_parent(task, new_path, false);
        if (ret < 0) return ret;
        ret = storage_link(old_path, new_path);
        return ret < 0 ? storage_errno(ret) : 0;
    }

    if (number == LINUX_SYS_READLINK || number == LINUX_SYS_READLINKAT) {
        struct task *task = sched_current_task();
        char path[RELIEFOS_FS_PATH_LEN];
        uint64_t pathname = number == LINUX_SYS_READLINKAT ? a1 : a0;
        uint64_t buffer = number == LINUX_SYS_READLINKAT ? a2 : a1;
        int32_t capacity = (int32_t)(number == LINUX_SYS_READLINKAT ? a3 : a2);
        int32_t dirfd = number == LINUX_SYS_READLINKAT ? (int32_t)a0 : LINUX_AT_FDCWD;
        int ret;
        if (capacity <= 0) return -RELIEFOS_EINVAL;
        ret = resolve_user_path_at_flags(task, dirfd, pathname,
                    number == LINUX_SYS_READLINKAT, path, NULL, false, 0);
        if (ret < 0) return ret;
        char *target = kernel_malloc(LINUX_PATH_MAX);
        if (!target) return -RELIEFOS_ENOMEM;
        ret = proc_readlink(path, target, LINUX_PATH_MAX);
        if (ret == -RELIEFOS_ENOENT) {
            struct storage_node node;
            int found = proc_lookup(path, &node);
            if (found == 0) ret = -RELIEFOS_EINVAL;
            else {
                uint32_t got = 0;
                found = storage_readlink(path, target, LINUX_PATH_MAX, &got);
                ret = found < 0 ? storage_errno(found) : (int)got;
            }
        }
        if (ret > capacity) ret = capacity;
        if (ret >= 0 && !user_range_writable(buffer, (uint32_t)ret)) ret = -RELIEFOS_EFAULT;
        for (int i = 0; i < ret; ++i) ((char *)(uintptr_t)buffer)[i] = target[i];
        kernel_free(target);
        return ret;
    }

    if (number == LINUX_SYS_FCHDIR) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_fd(task, (int)a0);
        if (!file) return -RELIEFOS_EBADF;
        if (file->node.type != RELIEFOS_FS_TYPE_DIR || !file->path[0]) return -RELIEFOS_ENOTDIR;
        int ret = fs_permissions_check(task, file->path, FS_ACCESS_EXEC, false);
        if (ret < 0) return ret;
        copy_text(sched_task_cwd(task), RELIEFOS_FS_PATH_LEN, file->path);
        return 0;
    }

    if (number == LINUX_SYS_TRUNCATE) {
        struct task *task = sched_current_task();
        char path[RELIEFOS_FS_PATH_LEN];
        int ret;
        if ((int64_t)a1 < 0) return -RELIEFOS_EINVAL;
        ret = resolve_user_path(task, a0, path, sizeof(path));
        if (ret < 0) return ret;
        ret = fs_permissions_check(task, path, FS_ACCESS_WRITE, false);
        if (ret < 0) return ret;
        ret = storage_truncate_file(path, a1);
        return ret < 0 ? storage_errno(ret) : 0;
    }

    if (number == LINUX_SYS_FSYNC || number == LINUX_SYS_FDATASYNC) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_fd(task, (int)a0);
        if (!file) return -RELIEFOS_EBADF;
        if (file->flags & TASK_FILE_FLAG_PATH) return -RELIEFOS_EBADF;
        if (file->flags & TASK_FILE_FLAG_DEV_SHM) return 0;
        if (file->flags & TASK_FILE_FLAG_DEV_BLOCK)
            return storage_sync_disk(STORAGE_BLOCK_DISK_ID(file->node.volume_id));
        if (file->kind || (file->flags & (TASK_FILE_FLAG_PIPE | TASK_FILE_FLAG_SOCKET_UNIX |
            TASK_FILE_FLAG_SOCKET_INET | TASK_FILE_FLAG_EVENTFD | TASK_FILE_FLAG_EPOLL | TASK_FILE_FLAG_TIMERFD)))
            return -RELIEFOS_EINVAL;
        if ((file->node.type != RELIEFOS_FS_TYPE_FILE && file->node.type != RELIEFOS_FS_TYPE_DIR) ||
            (file->node.flags & (STORAGE_NODE_FLAG_PROC | STORAGE_NODE_FLAG_SYSFS | STORAGE_NODE_FLAG_DEV_NODE |
                               STORAGE_NODE_FLAG_DEV_BLOCK | STORAGE_NODE_FLAG_DEV_FB0 | STORAGE_NODE_FLAG_PTY)))
            return -RELIEFOS_EINVAL;
        return storage_sync_volume(file->node.volume_id);
    }

    if (number == LINUX_SYS_UMASK) {
        struct task *task = sched_current_task();
        uint32_t old;
        if (!task) return -RELIEFOS_EPERM;
        old = (*sched_task_umask(task)) & 0777u;
        (*sched_task_umask(task)) = (uint32_t)a0 & 0777u;
        return old;
    }

    if (number == LINUX_SYS_GETCWD) {
        struct task *task = sched_current_task();
        const char *cwd = (task && sched_task_cwd(task)[0]) ? sched_task_cwd(task) : "/";
        const char *root = sched_task_root(task);
        size_t root_len = __builtin_strlen(root);
        char unreachable[RELIEFOS_FS_PATH_LEN + sizeof("(unreachable)")];
        if (root_len > 1) {
            if (!__builtin_strncmp(cwd, root, root_len) &&
                (!cwd[root_len] || cwd[root_len] == '/')) {
                cwd = cwd[root_len] ? cwd + root_len : "/";
            } else {
                __builtin_memcpy(unreachable, "(unreachable)", 13);
                copy_text(unreachable + 13, sizeof(unreachable) - 13, cwd);
                cwd = unreachable;
            }
        }
        size_t len = 0;
        while (cwd[len]) {
            ++len;
        }
        if (a1 == 0 || len + 1 > a1) {
            return -RELIEFOS_ERANGE;
        }
        if (!user_range_ok(a0, len + 1)) {
            return -RELIEFOS_EFAULT;
        }
        for (size_t i = 0; i <= len; ++i) {
            ((char *)(uintptr_t)a0)[i] = cwd[i];
        }
        return (int64_t)(len + 1);
    }

    if (number == LINUX_SYS_CHDIR || number == LINUX_SYS_CHROOT) {
        struct task *task = sched_current_task();
        struct storage_node node;
        char path[RELIEFOS_FS_PATH_LEN];
        int ret = resolve_user_path(task, a0, path, sizeof(path));
        if (ret < 0) {
            return ret;
        }
        ret = storage_lookup_path(path, &node);
        if (ret < 0) {
            return ret == -2 ? -RELIEFOS_ENOENT : ret;
        }
        if (node.type != RELIEFOS_FS_TYPE_DIR) {
            return -RELIEFOS_ENOTDIR;
        }
        ret = fs_permissions_check(task, path, FS_ACCESS_EXEC, false);
        if (ret < 0) return ret;
        if (number == LINUX_SYS_CHROOT) {
            if (!task || !(task->cap_effective & (1ULL << CAP_SYS_CHROOT))) return -RELIEFOS_EPERM;
            copy_text(sched_task_root_dir(task), RELIEFOS_FS_PATH_LEN, path);
            return 0;
        }
        if (task) {
            copy_text(sched_task_cwd(task), RELIEFOS_FS_PATH_LEN, path);
        }
        return 0;
    }

    if (number == LINUX_SYS_MKDIR || number == LINUX_SYS_MKDIRAT) {
        struct task *task = sched_current_task();
        char path[RELIEFOS_FS_PATH_LEN];
        uint32_t mode = number == LINUX_SYS_MKDIRAT ? (uint32_t)a2 : (uint32_t)a1;
        int ret = resolve_user_path_at_flags(task,
                    number == LINUX_SYS_MKDIRAT ? (int32_t)a0 : LINUX_AT_FDCWD,
                    number == LINUX_SYS_MKDIRAT ? a1 : a0, false, path, NULL, false, FS_LOOKUP_PARENT);
        if (ret < 0) {
            return ret;
        }
        if (mutation_path_special(path)) return -RELIEFOS_EEXIST;
        uint32_t path_length = 0;
        while (path[path_length]) ++path_length;
        if (path_length > 1 && path[path_length - 1] == '/') path[path_length - 1] = 0;
        /* Linux filename_create rejects an existing final dentry before
         * may_create checks write permission on its parent. mkdir -p relies
         * on EEXIST even when the caller cannot create new siblings. */
        struct storage_node existing;
        ret = fs_permissions_search(task, path, false);
        if (ret < 0) return ret;
        ret = storage_lookup_path(path, &existing);
        if (!ret) return -RELIEFOS_EEXIST;
        if (ret != -RELIEFOS_ENOENT) return storage_errno(ret);
        ret = fs_permissions_parent(task, path, false);
        if (ret < 0) {
            return ret;
        }
        ret = storage_mkdir(path);
        if (ret < 0) {
            return storage_errno(ret);
        }
        struct storage_node node;
        ret = storage_lookup_path(path, &node);
        if (!ret) ret = fs_permissions_create(task, path, &node, mode);
        if (ret < 0) (void)storage_rmdir(path);
        return ret;
    }

    if (number == LINUX_SYS_UNLINK || number == LINUX_SYS_RMDIR || number == LINUX_SYS_UNLINKAT) {
        struct task *task = sched_current_task();
        char path[RELIEFOS_FS_PATH_LEN];
        bool directory = number == LINUX_SYS_RMDIR || (number == LINUX_SYS_UNLINKAT && ((uint32_t)a2 & LINUX_AT_REMOVEDIR));
        if (number == LINUX_SYS_UNLINKAT && ((uint32_t)a2 & ~LINUX_AT_REMOVEDIR)) return -RELIEFOS_EINVAL;
        int ret = resolve_user_path_at_flags(task,
                    number == LINUX_SYS_UNLINKAT ? (int32_t)a0 : LINUX_AT_FDCWD,
                    number == LINUX_SYS_UNLINKAT ? a1 : a0, false, path, NULL, false, FS_LOOKUP_PARENT);
        if (ret < 0) {
            return ret;
        }
        int special = mutation_path_special(path);
        if (special) {
            if (!directory) return -RELIEFOS_EISDIR;
            return special == 1 ? -RELIEFOS_EINVAL : special == 2 ? -RELIEFOS_ENOTEMPTY : -RELIEFOS_EBUSY;
        }
        ret = mutation_path_slash(path, false);
        if (ret < 0) return ret;
        ret = fs_permissions_parent(task, path, true);
        if (ret < 0) {
            return ret;
        }
        ret = directory ? storage_rmdir(path) : storage_unlink(path);
        if (ret < 0) {
            return storage_errno(ret);
        }
        /* The member is already gone; a sidecar that could not be rewritten
         * must not turn a successful unlink into a reported failure. */
        (void)storage_sidecar_note_deleted(path);
        task_socket_unlink_path(path);
        return 0;
    }

    if (number == LINUX_SYS_RENAME || number == LINUX_SYS_RENAMEAT ||
        number == LINUX_SYS_RENAMEAT2) {
        struct task *task = sched_current_task();
        char old_path[RELIEFOS_FS_PATH_LEN];
        char new_path[RELIEFOS_FS_PATH_LEN];
        uint32_t flags = number == LINUX_SYS_RENAMEAT2 ? (uint32_t)a4 : 0;
        if (number == LINUX_SYS_RENAMEAT2 && flags != 0) {
            /* The storage backends do not implement exchange/noreplace
             * atomics; never silently turn those requests into rename. */
            return -RELIEFOS_EOPNOTSUPP;
        }
        int ret = resolve_user_path_at_flags(task,
                    number == LINUX_SYS_RENAME ? LINUX_AT_FDCWD : (int32_t)a0,
                    number == LINUX_SYS_RENAME ? a0 : a1, false,
                    old_path, NULL, false, FS_LOOKUP_PARENT);
        if (ret < 0) {
            return ret;
        }
        if (mutation_path_special(old_path)) return -RELIEFOS_EBUSY;
        ret = mutation_path_slash(old_path, false);
        if (ret < 0) return ret;
        ret = resolve_user_path_at_flags(task,
                    number == LINUX_SYS_RENAME ? LINUX_AT_FDCWD : (int32_t)a2,
                    number == LINUX_SYS_RENAME ? a1 : a3, false,
                    new_path, NULL, false, FS_LOOKUP_PARENT);
        if (ret < 0) {
            return ret;
        }
        if (mutation_path_special(new_path)) return -RELIEFOS_EBUSY;
        ret = mutation_path_slash(new_path, false);
        if (ret == -RELIEFOS_ENOENT) {
            struct storage_node source;
            ret = storage_lookup_path(old_path, &source);
            if (!ret && source.type != RELIEFOS_FS_TYPE_DIR) ret = -RELIEFOS_ENOTDIR;
        }
        if (ret < 0) return ret;
        ret = fs_permissions_parent(task, old_path, true);
        if (ret < 0) {
            return ret;
        }
        struct storage_node destination;
        int destination_exists = storage_lookup_path(new_path, &destination) == 0;
        ret = fs_permissions_parent(task, new_path, destination_exists);
        if (ret < 0) {
            return ret;
        }
        ret = storage_rename(old_path, new_path);
        if (ret < 0) {
            return storage_errno(ret);
        }
        /* The rename already succeeded; a record that could not be re-keyed
         * is cleaned up by the next explicit metadata write, not reported as
         * a failed rename. */
        (void)storage_sidecar_note_renamed(old_path, new_path);
        task_socket_rename_path(old_path, new_path);
        return 0;
    }

    if (number == LINUX_SYS_MOUNT) {
        struct task *task = sched_current_task();
        struct storage_node source_node;
        struct storage_node target_node;
        char source[RELIEFOS_FS_PATH_LEN];
        char target[RELIEFOS_FS_PATH_LEN];
        char filesystem[16];
        uint32_t disk_id;
        int32_t partition_index;
        int ret;

        if (!task || !(task->cap_effective & (1ULL << CAP_SYS_ADMIN))) return -RELIEFOS_EPERM;
        if (a3 & MS_REMOUNT) {
            if (a4) return -RELIEFOS_EOPNOTSUPP;
            ret = resolve_user_path(task, a1, target, sizeof(target));
            if (ret < 0) return ret;
            return storage_remount_path(target, a3 & ~MS_REMOUNT);
        }
        ret = resolve_user_path(task, a1, target, sizeof(target));
        if (ret < 0) return ret;
        filesystem[0]=0;
        if (a2) {
            ret=copy_user_string_fixed(filesystem,sizeof(filesystem),a2,NULL);
            if (ret < 0) return ret;
        }
        if (!__builtin_strcmp(filesystem,"tmpfs")) {
            uint64_t allowed=MS_RDONLY|MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_NOATIME|MS_NODIRATIME|MS_RELATIME|MS_STRICTATIME;
            if (a3 & ~allowed) return -RELIEFOS_EOPNOTSUPP;
            source[0]=0;
            if (a0) {
                ret=copy_user_string_fixed(source,sizeof(source),a0,NULL);
                if (ret < 0) return ret;
            }
            char *options=kernel_malloc(4096);
            if (!options) return -RELIEFOS_ENOMEM;
            options[0]=0;
            ret=a4 ? copy_user_string_fixed(options,4096,a4,NULL) : 0;
            uint64_t flags=a3;
            if (!(flags & (MS_NOATIME|MS_STRICTATIME))) flags |= MS_RELATIME;
            if (!ret) ret=storage_mount_tmpfs(source,target,flags,options,task->fsuid,task->fsgid);
            kernel_free(options);
            return ret;
        }
        if (a4 != 0 ||
            (a3 & ~(MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC |
                     MS_SYNCHRONOUS | MS_DIRSYNC | MS_NOATIME | MS_NODIRATIME |
                     MS_RELATIME | MS_STRICTATIME | MS_LAZYTIME)) != 0) {
            return -RELIEFOS_ENOTSUP;
        }
        /* The storage backends are read/write today. Do not claim a
         * read-only mount while later writes would still succeed. */
        if (a3 & MS_RDONLY) {
            return -RELIEFOS_ENOTSUP;
        }
        ret = resolve_user_path(task, a0, source, sizeof(source));
        if (ret < 0) {
            return ret;
        }
        ret = resolve_user_path(task, a1, target, sizeof(target));
        if (ret < 0) {
            return ret;
        }
        if (a2) {
            ret = copy_user_string_fixed(filesystem, sizeof(filesystem), a2, NULL);
            if (ret < 0) {
                return ret;
            }
        } else {
            filesystem[0] = 0;
        }
        ret = storage_lookup_path(source, &source_node);
        if (ret < 0) {
            return storage_errno(ret);
        }
        if (!(source_node.flags & STORAGE_NODE_FLAG_DEV_BLOCK)) {
            return -RELIEFOS_ENOTTY;
        }
        disk_id = STORAGE_BLOCK_DISK_ID(source_node.volume_id);
        partition_index = STORAGE_BLOCK_PARTITION(source_node.volume_id);
        if (partition_index < 0) {
            return -RELIEFOS_EINVAL;
        }
        ret = storage_lookup_path(target, &target_node);
        if (ret < 0) {
            return storage_errno(ret);
        }
        if (target_node.type != RELIEFOS_FS_TYPE_DIR) {
            return -RELIEFOS_ENOTDIR;
        }
        ret = storage_mount_block_partition(disk_id, (uint32_t)partition_index,
                                            target, filesystem[0] ? filesystem : NULL,
                                            a3, NULL);
        if (ret < 0) {
            console_error("[reliefnt] mount syscall failed source=%s target=%s fs=%s ret=%d\n",
                           source, target, filesystem[0] ? filesystem : "auto", ret);
        }
        return ret < 0 ? storage_errno(ret) : 0;
    }

    if (number == LINUX_SYS_UMOUNT2) {
        struct task *task = sched_current_task();
        char target[RELIEFOS_FS_PATH_LEN];
        uint32_t volume_id;
        int ret;
        if (!task || !(task->cap_effective & (1ULL << CAP_SYS_ADMIN))) return -RELIEFOS_EPERM;
        if (a1 & ~(uint64_t)UMOUNT_NOFOLLOW) {
            return -RELIEFOS_ENOTSUP;
        }
        ret = resolve_user_path(task, a0, target, sizeof(target));
        if (ret < 0) {
            return ret;
        }
        ret = storage_mount_path_volume_id(target, &volume_id);
        if (ret < 0) {
            return storage_errno(ret);
        }
        if (sched_volume_in_use(volume_id)) {
            return -RELIEFOS_EBUSY;
        }
        ret = storage_unmount_path(target, NULL);
        return ret < 0 ? storage_errno(ret) : 0;
    }

    return -RELIEFOS_ENOSYS;
}
