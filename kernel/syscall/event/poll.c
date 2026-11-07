#include "../internal.h"
#include <reliefnt/swap.h>

/* Standalone polling fixtures may omit the anonymous inotify provider. */
extern short task_inotify_poll(const struct task_file *) __attribute__((weak));
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
static int64_t syscall_poll_impl_core(struct task *task, struct pollfd *fds,
                                      uint32_t nfds, int64_t timeout_ms,
                                      uint64_t generation, bool *scanned)
{
    uint64_t ready = 0;
    if (scanned) *scanned = false;
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
    if (scanned) *scanned = true;
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
        } else if (file && file->kind == TASK_FILE_KIND_INOTIFY) {
            revents = task_inotify_poll ? events & task_inotify_poll(file) : 0;
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
        } else if (file && file->character_file) {
            revents=character_file_poll(file->character_file,file,events);
        } else if (file && (file->flags & TASK_FILE_FLAG_DEV_NODE)) {
            if (task_device_is(file, STORAGE_DEV_KIND_KEYBOARD) ||
                task_device_is(file, STORAGE_DEV_KIND_MOUSE)) {
                task_evdev_bind_vt(task, file);
                if ((events & POLLIN) &&
                    input_evdev_available_vt(file->node.first_cluster, file->aux,
                                             file->aux2, file->input_vt)) {
                    revents |= POLLIN;
                }
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

int64_t syscall_poll_impl(struct task *task, struct pollfd *fds,
                          uint32_t nfds, int64_t timeout_ms,
                          uint64_t generation)
{
    return syscall_poll_impl_core(task, fds, nfds, timeout_ms, generation, NULL);
}

#ifdef CONFIG_SWAP
/* One allocation holds the original poll array and the finished result. */
struct poll_user_state {
    uint64_t address;
    uint32_t count, imported, exported;
    int64_t result;
    bool finished, scanned;
    struct pollfd entries[];
};

/** @brief Import and return a poll array without holding user pages over waits.
 * @param task Current execution-owned task. @param address User poll array.
 * @param count Number of entries after the caller's rlimit check.
 * @param timeout_ms Original poll timeout. @return Readiness count, a private
 * prerequisite-wait marker, or an ordinary error. An armed syscall scope owns
 * the immutable input and any completed result; an unscoped call frees it here.
 * Each preparation and copy spans at most 32 KiB, and no user address or page
 * alias is retained across a scheduling boundary.
 */
static int64_t syscall_poll_staged(struct task *task, uint64_t address,
                                    uint32_t count, int64_t timeout_ms)
{
    uint64_t bytes = (uint64_t)count * sizeof(struct pollfd);
    if (address < RELIEFNT_USER_BASE || address >= RELIEFNT_USER_TOP ||
        bytes > RELIEFNT_USER_TOP - address)
        return -RELIEFOS_EFAULT;
    struct task_lookup_path *slot = NULL;
    int rc = task_lookup_scope_next(task, &slot);
    if (rc) return rc;
    struct poll_user_state *state;
    if (slot && slot->name) {
        if (slot->flags != TASK_LOOKUP_POLL_KIND) return -RELIEFOS_EINVAL;
        state = (struct poll_user_state *)slot->name;
        if (state->address != address || state->count != count) return -RELIEFOS_EINVAL;
    } else {
        state = kernel_malloc(sizeof(*state) + (size_t)bytes);
        if (!state) return -RELIEFOS_ENOMEM;
        *state = (struct poll_user_state){.address = address, .count = count};
        if (slot) { slot->name = (char *)state; slot->flags = TASK_LOOKUP_POLL_KIND; }
    }
    int64_t result = 0;
    while (state->imported < count) {
        uint32_t n = count - state->imported;
        if (n > 32768u / sizeof(struct pollfd)) n = 32768u / sizeof(struct pollfd);
        uint64_t user = address + (uint64_t)state->imported * sizeof(struct pollfd);
        uint64_t size = (uint64_t)n * sizeof(struct pollfd);
        rc = swap_prepare_user_range(task, user, size, false);
        if (!rc) rc = swap_prepare_user_range(task, user, size, true);
        if (rc || !user_range_ok(user, size) || !user_range_writable(user, size)) {
            result = rc == SWAP_STORAGE_CONTENDED ? KERNEL_SYSCALL_SWAP_CONTENDED : -RELIEFOS_EFAULT;
            goto poll_out;
        }
        __builtin_memcpy(state->entries + state->imported, (const void *)(uintptr_t)user, size);
        state->imported += n;
    }
    if (!state->finished) {
        uint64_t generation = kernel_poll_readiness_generation();
        result = syscall_poll_impl_core(task, state->entries, count, timeout_ms,
                                        generation, &state->scanned);
        if (result < 0) goto poll_out;
        state->result = result;
        state->finished = true;
    }
    while (state->scanned && state->exported < count) {
        uint32_t n = count - state->exported;
        if (n > 32768u / sizeof(struct pollfd)) n = 32768u / sizeof(struct pollfd);
        uint64_t user = address + (uint64_t)state->exported * sizeof(struct pollfd);
        uint64_t size = (uint64_t)n * sizeof(struct pollfd);
        rc = swap_prepare_user_range(task, user, size, true);
        if (rc || !user_range_writable(user, size)) {
            result = rc == SWAP_STORAGE_CONTENDED ? KERNEL_SYSCALL_SWAP_CONTENDED : -RELIEFOS_EFAULT;
            goto poll_out;
        }
        struct pollfd *out = (struct pollfd *)(uintptr_t)user;
        for (uint32_t i = 0; i < n; ++i)
            out[i].revents = state->entries[state->exported + i].revents;
        state->exported += n;
    }
    result = state->result;
poll_out:
    if (!slot) kernel_free(state);
    return result;
}
#endif

int64_t syscall_poll(uint64_t fds_ptr, uint64_t count, int64_t timeout_ms)
{
    struct task *task = sched_current_task();
    uint32_t nfds = (uint32_t)count;
    kernel_poll_wait_remove(task);
    if (!task || nfds > sched_task_limits(task)->nofile.rlim_cur) {
        return -RELIEFOS_EINVAL;
    }
#ifdef CONFIG_SWAP
    if (nfds) return syscall_poll_staged(task, fds_ptr, nfds, timeout_ms);
#endif
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

#ifdef CONFIG_SWAP
struct select_user_state {
    uint64_t pointers[3], offsets[3], timeout_ptr;
    uint32_t nfds, bytes, imported[3], output_set, output_done;
    int64_t timeout_ms, result;
    bool pselect_timeout, timeout_ready, finished, timeout_written;
    uint64_t sets[];
};

/** @brief Return the task-owned input or finished output bitmap for one set. */
static uint64_t *select_saved_set(struct select_user_state *state, unsigned index)
{
    return state->pointers[index]
        ? (uint64_t *)((uint8_t *)state->sets + state->offsets[index]) : NULL;
}

/** @brief Execute select using bounded imports and an owned output receipt.
 * @param task Current execution-owned caller. @param nfds Number of fds.
 * @param pointers Optional read, write and exception bitmaps.
 * @param timeout_ptr Optional timeval/timespec address.
 * @param pselect_timeout Timespec input without copyback when true.
 * @return Original result, EFAULT/ENOMEM, or the private prerequisite marker.
 * The scope owns only kernel copies and numeric addresses between retries;
 * result bitmaps are merged for overlapping user sets before each bounded
 * output chunk, preserving the original shared-buffer OR semantics.
 */
static int64_t syscall_select_staged(struct task *task, uint32_t nfds,
                                     const uint64_t pointers[3], uint64_t timeout_ptr,
                                     bool pselect_timeout)
{
    uint32_t bytes = (uint32_t)(((uint64_t)nfds + 63u) / 64u * sizeof(uint64_t));
    size_t allocation = sizeof(struct select_user_state);
    for (unsigned i = 0; i < 3; ++i) if (bytes && pointers[i]) {
        if (pointers[i] < RELIEFNT_USER_BASE || pointers[i] >= RELIEFNT_USER_TOP ||
            bytes > RELIEFNT_USER_TOP - pointers[i]) return -RELIEFOS_EFAULT;
        if (allocation > SIZE_MAX - bytes) return -RELIEFOS_EINVAL;
        allocation += bytes;
    }
    struct task_lookup_path *slot = NULL;
    int rc = task_lookup_scope_next(task, &slot);
    if (rc) return rc;
    struct select_user_state *state;
    if (slot && slot->name) {
        if (slot->flags != TASK_LOOKUP_SELECT_KIND) return -RELIEFOS_EINVAL;
        state = (struct select_user_state *)slot->name;
        if (state->nfds != nfds || state->timeout_ptr != timeout_ptr ||
            state->pselect_timeout != pselect_timeout ||
            __builtin_memcmp(state->pointers, pointers, sizeof(state->pointers)))
            return -RELIEFOS_EINVAL;
    } else {
        state = kernel_malloc(allocation);
        if (!state) return -RELIEFOS_ENOMEM;
        *state = (struct select_user_state){.nfds = nfds, .bytes = bytes,
            .timeout_ptr = timeout_ptr, .pselect_timeout = pselect_timeout, .timeout_ms = -1};
        size_t offset = 0;
        for (unsigned i = 0; i < 3; ++i) {
            state->pointers[i] = pointers[i];
            state->offsets[i] = offset;
            if (bytes && pointers[i]) offset += bytes;
        }
        if (slot) { slot->name = (char *)state; slot->flags = TASK_LOOKUP_SELECT_KIND; }
    }
    int64_t result = 0;
    for (unsigned i = 0; i < 3; ++i) while (bytes && pointers[i] && state->imported[i] < bytes) {
        uint32_t n = bytes - state->imported[i];
        if (n > 32768u) n = 32768u;
        uint64_t user = pointers[i] + state->imported[i];
        rc = swap_prepare_user_range(task, user, n, false);
        if (!rc) rc = swap_prepare_user_range(task, user, n, true);
        if (rc || !user_range_ok(user, n) || !user_range_writable(user, n)) {
            result = rc == SWAP_STORAGE_CONTENDED ? KERNEL_SYSCALL_SWAP_CONTENDED : -RELIEFOS_EFAULT;
            goto select_staged_out;
        }
        __builtin_memcpy((uint8_t *)select_saved_set(state, i) + state->imported[i],
                         (const void *)(uintptr_t)user, n);
        state->imported[i] += n;
    }
    if (!state->timeout_ready) {
        if (timeout_ptr) {
            size_t size = pselect_timeout ? sizeof(struct linux_timespec) : sizeof(struct linux_timeval);
            rc = swap_prepare_user_range(task, timeout_ptr, size, false);
            if (!rc && !pselect_timeout) rc = swap_prepare_user_range(task, timeout_ptr, size, true);
            if (rc || !user_range_ok(timeout_ptr, size) ||
                (!pselect_timeout && !user_range_writable(timeout_ptr, size))) {
                result = rc == SWAP_STORAGE_CONTENDED ? KERNEL_SYSCALL_SWAP_CONTENDED : -RELIEFOS_EFAULT;
                goto select_staged_out;
            }
            if (pselect_timeout) {
                struct linux_timespec value = *(const struct linux_timespec *)(uintptr_t)timeout_ptr;
                if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000) {
                    result = -RELIEFOS_EINVAL; goto select_staged_out;
                }
                state->timeout_ms = value.tv_sec > 2147482 ? 2147483647 :
                    value.tv_sec * 1000 + (value.tv_nsec + 999999) / 1000000;
            } else {
                struct linux_timeval value = *(const struct linux_timeval *)(uintptr_t)timeout_ptr;
                if (value.tv_sec < 0 || value.tv_usec < 0 || value.tv_usec >= 1000000) {
                    result = -RELIEFOS_EINVAL; goto select_staged_out;
                }
                state->timeout_ms = value.tv_sec > 2147482 ? 2147483647 :
                    value.tv_sec * 1000 + (value.tv_usec + 999) / 1000;
            }
        }
        state->timeout_ready = true;
    }
    if (!state->finished) {
        uint64_t *read_set = select_saved_set(state, 0);
        uint64_t *write_set = select_saved_set(state, 1);
        uint64_t *except_set = select_saved_set(state, 2);
        uint32_t selected = 0;
        for (uint32_t fd = 0; fd < nfds; ++fd)
            if (syscall_select_bit(read_set, fd) || syscall_select_bit(write_set, fd) ||
                syscall_select_bit(except_set, fd)) ++selected;
        struct pollfd *pollfds = NULL;
        int *fd_map = NULL;
        if (selected) {
            pollfds = kernel_malloc((size_t)selected * sizeof(*pollfds));
            fd_map = kernel_malloc((size_t)selected * sizeof(*fd_map));
            if (!pollfds || !fd_map) {
                kernel_free(pollfds); kernel_free(fd_map);
                result = -RELIEFOS_ENOMEM; goto select_staged_out;
            }
            uint32_t at = 0;
            for (uint32_t fd = 0; fd < nfds; ++fd) {
                short events = 0;
                if (syscall_select_bit(read_set, fd)) events |= POLLIN | POLLRDNORM;
                if (syscall_select_bit(write_set, fd)) events |= POLLOUT | POLLWRNORM;
                if (syscall_select_bit(except_set, fd)) events |= POLLPRI;
                if (events) { pollfds[at] = (struct pollfd){(int32_t)fd, events, 0}; fd_map[at++] = (int)fd; }
            }
        }
        result = syscall_poll_impl(task, pollfds, selected, state->timeout_ms,
                                   kernel_poll_readiness_generation());
        if (result >= 0) for (uint32_t i = 0; i < selected; ++i)
            if (pollfds[i].revents & POLLNVAL) { result = -RELIEFOS_EBADF; break; }
        if (result >= 0) {
            if (read_set) __builtin_memset(read_set, 0, bytes);
            if (write_set) __builtin_memset(write_set, 0, bytes);
            if (except_set) __builtin_memset(except_set, 0, bytes);
            for (uint32_t i = 0; i < selected; ++i) {
                short revents = pollfds[i].revents;
                uint32_t fd = (uint32_t)fd_map[i];
                if (read_set && (revents & (POLLIN | POLLRDNORM | POLLHUP | POLLERR)))
                    read_set[fd >> 6] |= 1ULL << (fd & 63u);
                if (write_set && (revents & (POLLOUT | POLLWRNORM | POLLHUP | POLLERR)))
                    write_set[fd >> 6] |= 1ULL << (fd & 63u);
                if (except_set && (revents & POLLPRI)) except_set[fd >> 6] |= 1ULL << (fd & 63u);
            }
            state->result = result; state->finished = true;
        }
        kernel_free(pollfds); kernel_free(fd_map);
        if (result < 0) goto select_staged_out;
    }
    while (state->output_set < 3) {
        unsigned i = state->output_set;
        if (!pointers[i] || state->output_done == bytes) {
            ++state->output_set; state->output_done = 0; continue;
        }
        uint32_t n = bytes - state->output_done;
        if (n > 32768u) n = 32768u;
        uint64_t user = pointers[i] + state->output_done;
        rc = swap_prepare_user_range(task, user, n, true);
        if (rc || !user_range_writable(user, n)) {
            result = rc == SWAP_STORAGE_CONTENDED ? KERNEL_SYSCALL_SWAP_CONTENDED : -RELIEFOS_EFAULT;
            goto select_staged_out;
        }
        uint8_t *out = (uint8_t *)(uintptr_t)user;
        for (uint32_t j = 0; j < n; ++j) {
            uint8_t value = 0; uint64_t address = user + j;
            for (unsigned k = 0; k < 3; ++k)
                if (pointers[k] && address >= pointers[k] && address - pointers[k] < bytes)
                    value |= ((const uint8_t *)select_saved_set(state, k))[address - pointers[k]];
            out[j] = value;
        }
        state->output_done += n;
    }
    if (!pselect_timeout && timeout_ptr && state->result == 0 && !state->timeout_written) {
        struct linux_timeval zero = {0, 0};
        rc = swap_prepare_user_range(task, timeout_ptr, sizeof(zero), true);
        if (rc || !user_range_writable(timeout_ptr, sizeof(zero))) {
            result = rc == SWAP_STORAGE_CONTENDED ? KERNEL_SYSCALL_SWAP_CONTENDED : -RELIEFOS_EFAULT;
            goto select_staged_out;
        }
        *(struct linux_timeval *)(uintptr_t)timeout_ptr = zero;
        state->timeout_written = true;
    }
    result = state->result;
select_staged_out:
    if (!slot) kernel_free(state);
    return result;
}
#endif

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
#ifdef CONFIG_SWAP
    struct task *task = sched_current_task();
    kernel_poll_wait_remove(task);
    if (!task || nfds_arg > INT32_MAX || nfds_arg > sched_task_limits(task)->nofile.rlim_cur)
        return -RELIEFOS_EINVAL;
    const uint64_t pointers[3] = {read_ptr, write_ptr, except_ptr};
    return syscall_select_staged(task, (uint32_t)nfds_arg, pointers, timeout_ptr, pselect_timeout);
#else
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
#endif
}

/**
 * @brief Poll with a temporary signal mask owned across scheduler retries.
 * @param fds_ptr User pollfd array, nullable when count is zero.
 * @param count Number of pollfd entries, checked against the caller's limit.
 * @param timeout_ptr Optional user timespec; null selects an indefinite wait.
 * @param signal_mask_ptr Optional user pointer to the native 64-bit signal set.
 * @param signal_mask_size Signal-set size; a nonnull mask requires eight bytes.
 * @return Ready count, zero on timeout, negative errno or an internal retry token.
 * @note The syscall execution transaction serializes mask installation with
 * polling. Matching retries retain the temporary mask; final returns and scope
 * cancellation restore it, while signal delivery saves the old mask for sigreturn.
 * Mask and timeout operands are copied before installation and retained by the swap scope.
 */
int64_t syscall_ppoll(uint64_t fds_ptr, uint64_t count, uint64_t timeout_ptr,
                             uint64_t signal_mask_ptr, uint64_t signal_mask_size)
{
    struct task *task = sched_current_task();
    uint64_t requested_mask = 0;
    int64_t timeout_ms = -1;
    int64_t result = -RELIEFOS_EINVAL;
    if (!task) return result;
    if (signal_mask_ptr) {
        if (signal_mask_size != sizeof(requested_mask)) goto ppoll_out;
#ifdef CONFIG_SWAP
        struct task_user_text operand;
        result = task_lookup_user_bytes(task, signal_mask_ptr, sizeof(requested_mask), &operand);
        if (result) goto ppoll_out;
        __builtin_memcpy(&requested_mask, operand.text, sizeof(requested_mask));
        kernel_free(operand.allocation);
#else
        if (!task->sigsuspend_active) {
            if (!user_range_ok(signal_mask_ptr, sizeof(requested_mask))) {
                result = -RELIEFOS_EFAULT;
                goto ppoll_out;
            }
            __builtin_memcpy(&requested_mask, (const void *)(uintptr_t)signal_mask_ptr,
                             sizeof(requested_mask));
        }
#endif
    } else if (signal_mask_size != 0 && signal_mask_size != sizeof(requested_mask)) {
        goto ppoll_out;
    }
    if (timeout_ptr) {
        struct linux_timespec timeout;
#ifdef CONFIG_SWAP
        struct task_user_text operand;
        result = task_lookup_user_bytes(task, timeout_ptr, sizeof(timeout), &operand);
        if (result) goto ppoll_out;
        __builtin_memcpy(&timeout, operand.text, sizeof(timeout));
        kernel_free(operand.allocation);
#else
        if (!user_range_ok(timeout_ptr, sizeof(timeout))) {
            result = -RELIEFOS_EFAULT;
            goto ppoll_out;
        }
        timeout = *(const struct linux_timespec *)(uintptr_t)timeout_ptr;
#endif
        if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000) {
            result = -RELIEFOS_EINVAL;
            goto ppoll_out;
        }
        if (timeout.tv_sec > 2147482) {
            timeout_ms = 2147483647;
        } else {
            timeout_ms = timeout.tv_sec * 1000 + (timeout.tv_nsec + 999999) / 1000000;
            if (timeout_ms > 2147483647) timeout_ms = 2147483647;
        }
    }
    if (signal_mask_ptr && !task->sigsuspend_active) {
        task->sigsuspend_saved_mask = task->blocked_signals;
        task->sigsuspend_active = 1;
        task->blocked_signals = requested_mask & ~((1ULL << 8) | (1ULL << 18));
    }
    result = syscall_poll(fds_ptr, count, timeout_ms);
ppoll_out:
    if (result != -RELIEFOS_EAGAIN &&
        result != KERNEL_SYSCALL_BLOCKED &&
#ifdef CONFIG_SWAP
        !(result == KERNEL_SYSCALL_SWAP_CONTENDED && task->lookup_scope.armed &&
          task->lookup_scope.number == LINUX_SYS_PPOLL) &&
#endif
        task->sigsuspend_active) {
        task->blocked_signals = task->sigsuspend_saved_mask;
        task->sigsuspend_active = 0;
    }
    return result;
}
