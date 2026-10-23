#include "../internal.h"

static bool syscall_select_bit(const uint64_t *set, uint32_t fd);


/**
 * @brief Park the caller until readiness, the one-tick backstop, or timeout.
 *
 * Pollable sources publish a generation change and wake this shared queue.
 * The generation comparison is atomic with queue registration, closing the
 * check-to-sleep race. The tick deadline remains a conservative backstop for
 * readiness sources that do not publish notifications.
 */
int64_t syscall_poll_park(struct task *task, int64_t timeout_ms,
                                 uint64_t generation, bool can_wait_for_readiness)
{
    uint64_t now = time_ticks();
    if (!task->poll_deadline_ticks) {
        task->poll_deadline_ticks = timeout_ms < 0
                                        ? UINT64_MAX
                                        : now + (uint64_t)timeout_ms / 10u + 1u;
    } else if (now >= task->poll_deadline_ticks) {
        task->poll_deadline_ticks = 0;
        return 0;
    }
    if (can_wait_for_readiness) {
        int parked = kernel_poll_wait_current(generation, now + 1u);
        if (parked >= 0) {
            /* The syscall epilogue rewinds int $0x80 for this sentinel without
             * sleeping again. On a generation mismatch it retries immediately;
             * on success the publisher or one-tick deadline makes it runnable. */
            return KERNEL_SYSCALL_BLOCKED;
        }
    }
    return -RELIEFOS_EAGAIN;
}

/**
 * @brief Poll descriptors using their bound terminal or device readiness.
 * @param task Calling task under the execution transaction.
 * @param fds Validated in/out poll array; nullable when nfds is zero.
 * @param nfds Number of entries.
 * @param timeout_ms Linux poll timeout, narrowed to signed 32 bits.
 * @param generation Readiness generation captured before scanning descriptors.
 * @return Ready count, zero on timeout, negative errno or a blocked retry token.
 */
int64_t syscall_poll_impl(struct task *task, struct pollfd *fds,
                                 uint32_t nfds, int64_t timeout_ms,
                                 uint64_t generation)
{
    uint64_t ready = 0;
    timeout_ms = (int32_t)timeout_ms;

    if (!nfds) {
        /* poll(NULL, 0, timeout) is the classic millisecond sleep idiom. */
        if (timeout_ms != 0)
            return syscall_poll_park(task, timeout_ms, generation, false);
        task->poll_deadline_ticks = 0;
        return 0;
    }
    if (timeout_ms != 0 && task->poll_deadline_ticks &&
        time_ticks() >= task->poll_deadline_ticks) {
        task->poll_deadline_ticks = 0;
        return 0;
    }
    for (uint32_t i = 0; i < nfds; ++i) {
        short events = fds[i].events;
        short revents = 0;
        int fd = fds[i].fd;
        struct task_file *file;
        fds[i].revents = 0;
        if (fd < 0) {
            continue;
        }
        file = task_file_for_io(task, fd);
        {
            struct task_pty_fd *endpoint = task_pty_for_io(task, fd);
            if (endpoint) {
                if (pty_is_hungup(endpoint->pty_id)) {
                    if (events & POLLIN) revents |= POLLIN;
                    revents |= POLLHUP;
                    if (endpoint->endpoint == TASK_PTY_ENDPOINT_SLAVE) {
                        revents |= POLLERR;
                    }
                } else {
                    if (events & POLLIN) {
                        uint32_t available = endpoint->endpoint == TASK_PTY_ENDPOINT_MASTER
                                                 ? pty_output_available(endpoint->pty_id)
                                                 : pty_input_available(endpoint->pty_id);
                        if (available) revents |= POLLIN;
                    }
                    if (events & POLLOUT) revents |= POLLOUT;
                }
                fds[i].revents = revents;
                if (revents) ++ready;
                continue;
            }
        }
        if (file && file->kind == TASK_FILE_KIND_SIGNALFD) {
            revents = events & task_signalfd_poll(task, file);
        } else if (file && (file->flags & TASK_FILE_FLAG_EPOLL)) {
            if ((events & (POLLIN | POLLRDNORM)) &&
                task_epoll_poll_ready(task, file))
                revents |= events & (POLLIN | POLLRDNORM);
        } else if (file && (file->flags & TASK_FILE_FLAG_PIPE)) {
            revents = task_pipe_poll(file, events);
        } else if (file && (file->flags & TASK_FILE_FLAG_SOCKET_UNIX)) {
            revents = task_socket_poll(file, events);
        } else if (file && (file->flags & TASK_FILE_FLAG_SOCKET_INET)) {
            revents = task_inet_poll(file, events);
        } else if (file && (file->flags & TASK_FILE_FLAG_EVENTFD)) {
            if ((events & (POLLIN | POLLRDNORM)) && file->aux) revents |= POLLIN | POLLRDNORM;
            if ((events & (POLLOUT | POLLWRNORM)) && file->aux < UINT64_MAX - 1ULL)
                revents |= POLLOUT | POLLWRNORM;
        } else if (file && (file->flags & TASK_FILE_FLAG_TIMERFD)) {
            struct task_timerfd *timer = (struct task_timerfd *)(uintptr_t)file->aux;
            task_timerfd_update(timer);
            if ((events & (POLLIN | POLLRDNORM)) && timer && timer->expirations)
                revents |= POLLIN | POLLRDNORM;
        } else if (file && file->drm_file) {
            revents = drm_file_poll(file, events);
        } else if (file && file->drm_prime_file) {
            revents = drm_prime_file_poll(file, events);
        } else if (file && (file->flags & TASK_FILE_FLAG_DEV_NODE)) {
            if (task_device_is(file, STORAGE_DEV_KIND_KEYBOARD) ||
                task_device_is(file, STORAGE_DEV_KIND_MOUSE)) {
                task_evdev_bind_vt(task, file);
                if ((events & POLLIN) &&
                    input_evdev_available_vt(file->node.first_cluster, file->aux,
                                             file->aux2, file->input_vt)) {
                    revents |= POLLIN;
                }
            } else if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_MIXER) {
                revents |= audio_mixer_poll(file, events);
            } else if (task_device_is(file, STORAGE_DEV_KIND_AUDIO)) {
                if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_PCM) {
                    revents |= audio_device_poll(file, events);
                } else {
                    revents |= audio_oss_poll(file, events);
                }
            } else if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_CONTROL) {
                revents |= audio_control_poll_file(file->audio_control_file, events);
            } else if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_TIMER) {
                revents |= audio_timer_poll(file, events);
            } else if (task_device_is(file, STORAGE_DEV_KIND_TTY) ||
                       task_device_is(file, STORAGE_DEV_KIND_CONSOLE)) {
                uint32_t terminal = task_device_is(file, STORAGE_DEV_KIND_CONSOLE)
                    ? (uint32_t)file->aux : task->pty_id;
                if ((events & POLLIN) && pty_input_available(terminal)) revents |= POLLIN;
                if (pty_is_hungup(terminal)) {
                    revents |= POLLHUP;
                }
            } else if (events & POLLIN) {
                revents |= POLLIN;
            }
            if ((events & POLLOUT) && file_can_write(file) &&
                !task_device_is(file, STORAGE_DEV_KIND_KEYBOARD) &&
                !task_device_is(file, STORAGE_DEV_KIND_MOUSE) &&
                !task_device_is(file, STORAGE_DEV_KIND_AUDIO) &&
                !(file->node.flags & (STORAGE_NODE_FLAG_AUDIO_CONTROL |
                                      STORAGE_NODE_FLAG_AUDIO_MIXER))) {
                revents |= POLLOUT;
            }
        } else if (file) {
            /* Linux regular files are always ready: readiness is not tied to
             * the current offset or the descriptor's access mode. */
            if (file->node.type == RELIEFOS_FS_TYPE_FILE ||
                file->node.type == RELIEFOS_FS_TYPE_DIR) {
                revents |= events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
            } else {
                if ((events & POLLIN) && file_can_read(file)) revents |= POLLIN;
                if ((events & POLLOUT) && file_can_write(file)) revents |= POLLOUT;
            }
        } else if (fd >= 0 && fd <= 2 && task_pty_stream_for_fd(task, fd) >= 0) {
            if (fd == 0) {
                if (pty_input_available(task->pty_id)) revents |= POLLIN;
                if (pty_is_hungup(task->pty_id)) revents |= POLLHUP;
            } else if (pty_is_hungup(task->pty_id)) {
                revents |= POLLHUP;
            } else if (events & POLLOUT) {
                revents |= POLLOUT;
            }
        } else if (!task_pty_fd_for_fd(task, fd)) {
            revents |= POLLNVAL;
        } else if (events & POLLOUT) {
            revents |= POLLOUT;
        }
        fds[i].revents = revents;
        if (revents) ++ready;
    }
    if (ready) {
        task->poll_deadline_ticks = 0;
        return (int64_t)ready;
    }
    if (timeout_ms == 0) {
        task->poll_deadline_ticks = 0;
        return 0;
    }
    return syscall_poll_park(task, timeout_ms, generation, true);
}

int64_t syscall_poll(uint64_t fds_ptr, uint64_t count, int64_t timeout_ms)
{
    struct task *task = sched_current_task();
    uint32_t nfds = (uint32_t)count;
    kernel_poll_wait_remove(task);
    if (!task || nfds > sched_task_limits(task)->nofile.rlim_cur) {
        return -RELIEFOS_EINVAL;
    }
    if (nfds && !user_range_writable(fds_ptr, (uint64_t)nfds * sizeof(struct pollfd))) {
        return -RELIEFOS_EFAULT;
    }
    uint64_t generation = kernel_poll_readiness_generation();
    return syscall_poll_impl(task, (struct pollfd *)(uintptr_t)fds_ptr, nfds,
                             timeout_ms, generation);
}

/**
 * @brief Test one descriptor bit in a copied select set.
 * @param set Kernel copy of a user fd_set, or null for an absent set.
 * @param fd Descriptor number represented by the bit.
 * @return True when the requested bit is set.
 */
static bool syscall_select_bit(const uint64_t *set, uint32_t fd)
{
    return set && (set[fd >> 6] & (1ULL << (fd & 63u))) != 0;
}

/**
 * @brief Implement select/pselect with dynamic fd bitmaps and poll readiness.
 * @param nfds_arg Highest descriptor plus one, in the Linux signed-int ABI.
 * @param read_ptr Optional readable/writable read set pointer.
 * @param write_ptr Optional readable/writable write set pointer.
 * @param except_ptr Optional readable/writable exception set pointer.
 * @param timeout_ptr Optional timeval or pselect timespec pointer.
 * @param pselect_timeout True selects timespec input without timeout copyback.
 * @return Ready descriptor count, zero on timeout, or a negative errno/block token.
 * @note User fd_sets are copied before polling and changed only after all error
 *       checks pass, so EFAULT, ENOMEM and EBADF preserve their input masks.
 */
int64_t syscall_select_common(uint64_t nfds_arg, uint64_t read_ptr,
                                     uint64_t write_ptr, uint64_t except_ptr,
                                     uint64_t timeout_ptr, bool pselect_timeout)
{
    struct task *task = sched_current_task();
    uint64_t *read_set = NULL;
    uint64_t *write_set = NULL;
    uint64_t *except_set = NULL;
    uint64_t *bitmap_storage = NULL;
    struct pollfd *pollfds = NULL;
    int *fd_map = NULL;
    uint64_t nfds = nfds_arg;
    size_t words = 0;
    size_t bytes = 0;
    uint32_t selected = 0;
    int64_t timeout_ms = -1;
    int64_t result = -RELIEFOS_EINVAL;

    kernel_poll_wait_remove(task);
    if (!task || nfds_arg > INT32_MAX) return -RELIEFOS_EINVAL;
    if (nfds > sched_task_limits(task)->nofile.rlim_cur) return -RELIEFOS_EINVAL;
    words = (size_t)((nfds + 63u) / 64u);
    if (words > SIZE_MAX / sizeof(uint64_t)) return -RELIEFOS_EINVAL;
    bytes = words * sizeof(uint64_t);
    if (bytes) {
        if (read_ptr && !user_range_ok(read_ptr, bytes)) return -RELIEFOS_EFAULT;
        if (write_ptr && !user_range_ok(write_ptr, bytes)) return -RELIEFOS_EFAULT;
        if (except_ptr && !user_range_ok(except_ptr, bytes)) return -RELIEFOS_EFAULT;
        if (read_ptr && !user_range_writable(read_ptr, bytes)) return -RELIEFOS_EFAULT;
        if (write_ptr && !user_range_writable(write_ptr, bytes)) return -RELIEFOS_EFAULT;
        if (except_ptr && !user_range_writable(except_ptr, bytes)) return -RELIEFOS_EFAULT;
        uint32_t set_count = (read_ptr != 0) + (write_ptr != 0) + (except_ptr != 0);
        if (set_count) {
            if (bytes > SIZE_MAX / set_count) return -RELIEFOS_EINVAL;
            bitmap_storage = kernel_malloc(bytes * set_count);
            if (!bitmap_storage) return -RELIEFOS_ENOMEM;
            __builtin_memset(bitmap_storage, 0, bytes * set_count);
            uint64_t *next = bitmap_storage;
            if (read_ptr) {
                read_set = next;
                __builtin_memcpy(read_set, (const void *)(uintptr_t)read_ptr, bytes);
                next += words;
            }
            if (write_ptr) {
                write_set = next;
                __builtin_memcpy(write_set, (const void *)(uintptr_t)write_ptr, bytes);
                next += words;
            }
            if (except_ptr) {
                except_set = next;
                __builtin_memcpy(except_set, (const void *)(uintptr_t)except_ptr, bytes);
            }
        }
    }
    if (timeout_ptr) {
        if (pselect_timeout) {
            struct linux_timespec timeout;
            if (!user_range_ok(timeout_ptr, sizeof(timeout))) {
                result = -RELIEFOS_EFAULT;
                goto select_out;
            }
            timeout = *(const struct linux_timespec *)(uintptr_t)timeout_ptr;
            if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000) {
                result = -RELIEFOS_EINVAL;
                goto select_out;
            }
            timeout_ms = timeout.tv_sec > 2147482
                            ? 2147483647
                            : timeout.tv_sec * 1000 + (timeout.tv_nsec + 999999) / 1000000;
        } else {
            struct linux_timeval timeout;
            if (!user_range_ok(timeout_ptr, sizeof(timeout)) ||
                !user_range_writable(timeout_ptr, sizeof(timeout))) {
                result = -RELIEFOS_EFAULT;
                goto select_out;
            }
            timeout = *(const struct linux_timeval *)(uintptr_t)timeout_ptr;
            if (timeout.tv_sec < 0 || timeout.tv_usec < 0 || timeout.tv_usec >= 1000000) {
                result = -RELIEFOS_EINVAL;
                goto select_out;
            }
            timeout_ms = timeout.tv_sec > 2147482
                            ? 2147483647
                            : timeout.tv_sec * 1000 + (timeout.tv_usec + 999) / 1000;
        }
    }
    for (uint32_t fd = 0; fd < (uint32_t)nfds; ++fd) {
        short events = 0;
        if (syscall_select_bit(read_set, fd)) events |= POLLIN | POLLRDNORM;
        if (syscall_select_bit(write_set, fd)) events |= POLLOUT | POLLWRNORM;
        if (syscall_select_bit(except_set, fd)) events |= POLLPRI;
        if (events) ++selected;
    }
    if (selected) {
        /* nfds is bounded to INT32_MAX above, so both products fit x86-64 size_t. */
        pollfds = (struct pollfd *)kernel_malloc((size_t)selected * sizeof(*pollfds));
        fd_map = (int *)kernel_malloc((size_t)selected * sizeof(*fd_map));
        if (!pollfds || !fd_map) {
            result = -RELIEFOS_ENOMEM;
            goto select_out;
        }
        uint32_t at = 0;
        for (uint32_t fd = 0; fd < (uint32_t)nfds; ++fd) {
            short events = 0;
            if (syscall_select_bit(read_set, fd)) events |= POLLIN | POLLRDNORM;
            if (syscall_select_bit(write_set, fd)) events |= POLLOUT | POLLWRNORM;
            if (syscall_select_bit(except_set, fd)) events |= POLLPRI;
            if (!events) continue;
            pollfds[at] = (struct pollfd){(int32_t)fd, events, 0};
            fd_map[at++] = (int)fd;
        }
    }
    uint64_t generation = kernel_poll_readiness_generation();
    result = syscall_poll_impl(task, pollfds, selected, timeout_ms, generation);
    if (result < 0) goto select_out;
    for (uint32_t i = 0; i < selected; ++i) {
        if (pollfds[i].revents & POLLNVAL) {
            result = -RELIEFOS_EBADF;
            goto select_out;
        }
    }
    if (read_ptr && bytes) __builtin_memset((void *)(uintptr_t)read_ptr, 0, bytes);
    if (write_ptr && bytes) __builtin_memset((void *)(uintptr_t)write_ptr, 0, bytes);
    if (except_ptr && bytes) __builtin_memset((void *)(uintptr_t)except_ptr, 0, bytes);
    for (uint32_t i = 0; i < selected; ++i) {
        short revents = pollfds[i].revents;
        uint32_t fd = (uint32_t)fd_map[i];
        if (read_ptr && (revents & (POLLIN | POLLRDNORM | POLLHUP | POLLERR)))
            ((uint64_t *)(uintptr_t)read_ptr)[fd >> 6] |= 1ULL << (fd & 63u);
        if (write_ptr && (revents & (POLLOUT | POLLWRNORM | POLLHUP | POLLERR)))
            ((uint64_t *)(uintptr_t)write_ptr)[fd >> 6] |= 1ULL << (fd & 63u);
        if (except_ptr && (revents & POLLPRI))
            ((uint64_t *)(uintptr_t)except_ptr)[fd >> 6] |= 1ULL << (fd & 63u);
    }
    if (!pselect_timeout && timeout_ptr && result == 0) {
        struct linux_timeval zero = {0, 0};
        *(struct linux_timeval *)(uintptr_t)timeout_ptr = zero;
    }
select_out:
    kernel_free(bitmap_storage);
    kernel_free(pollfds);
    kernel_free(fd_map);
    return result;
}

int64_t syscall_ppoll(uint64_t fds_ptr, uint64_t count, uint64_t timeout_ptr,
                             uint64_t signal_mask_ptr, uint64_t signal_mask_size)
{
    int64_t timeout_ms = -1;
    if (signal_mask_ptr) {
        if (signal_mask_size != sizeof(uint64_t)) return -RELIEFOS_EINVAL;
        if (!user_range_ok(signal_mask_ptr, sizeof(uint64_t))) return -RELIEFOS_EFAULT;
        return -RELIEFOS_EOPNOTSUPP;
    }
    if (signal_mask_size != 0 && signal_mask_size != sizeof(uint64_t)) {
        return -RELIEFOS_EINVAL;
    }
    if (timeout_ptr) {
        struct linux_timespec timeout;
        if (!user_range_ok(timeout_ptr, sizeof(timeout))) return -RELIEFOS_EFAULT;
        timeout = *(const struct linux_timespec *)(uintptr_t)timeout_ptr;
        if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000) {
            return -RELIEFOS_EINVAL;
        }
        if (timeout.tv_sec > 2147482) {
            timeout_ms = 2147483647;
        } else {
            timeout_ms = timeout.tv_sec * 1000 + (timeout.tv_nsec + 999999) / 1000000;
            if (timeout_ms > 2147483647) timeout_ms = 2147483647;
        }
    }
    return syscall_poll(fds_ptr, count, timeout_ms);
}
