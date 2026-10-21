#include "../internal.h"

static uint32_t epoll_to_poll(uint32_t events);

static uint32_t poll_to_epoll(short events);

static struct task_epoll *task_epoll_for_fd(struct task *task, int fd);

static int epoll_event_valid(uint32_t events);

static int64_t syscall_epoll_wait_common(struct task *task, struct task_epoll *epoll,
                                         uint64_t events_ptr, uint32_t maxevents,
                                         int64_t timeout_ms);


/**
 * @brief Remove a descriptor from every epoll set owned by a task.
 * @param task Descriptor-table owner.
 * @param fd Descriptor being closed or replaced.
 *
 * Linux removes an epoll registration when the last descriptor reference is
 * closed. Keeping a numeric fd here would otherwise let a later open reuse
 * the number while dispatching the old callback pointer, which is fatal for
 * Xorg's input thread during VT return.
 */
void task_epoll_remove_fd(struct task *task, int fd)
{
    if (!task || fd < 0) return;
    for (uint32_t i = 0; i < sched_task_file_capacity(task); ++i) {
        struct task_file *file = sched_task_file_at(task, i);
        if (!file || !file->used || !(file->flags & TASK_FILE_FLAG_EPOLL) || !file->aux)
            continue;
        struct task_epoll *epoll = (struct task_epoll *)(uintptr_t)file->aux;
        for (uint32_t j = 0; j < TASK_EPOLL_MAX_ENTRIES; ++j) {
            if (!epoll->item[j].active || epoll->item[j].fd != fd) continue;
            epoll->item[j] = (struct task_epoll_entry){0};
            if (epoll->entries) --epoll->entries;
        }
    }
    for (uint32_t i = 0; i < SCHED_TASK_STDIO_MAX; ++i) {
        struct task_file *file = &sched_task_fds(task)->stdio_files[i];
        if (!file->used || !(file->flags & TASK_FILE_FLAG_EPOLL) || !file->aux)
            continue;
        struct task_epoll *epoll = (struct task_epoll *)(uintptr_t)file->aux;
        for (uint32_t j = 0; j < TASK_EPOLL_MAX_ENTRIES; ++j) {
            if (!epoll->item[j].active || epoll->item[j].fd != fd) continue;
            epoll->item[j] = (struct task_epoll_entry){0};
            if (epoll->entries) --epoll->entries;
        }
    }
}

static uint32_t epoll_to_poll(uint32_t events)
{
    uint32_t result = 0;
    if (events & (EPOLLIN | EPOLLRDNORM | EPOLLRDBAND | EPOLLMSG)) result |= POLLIN;
    if (events & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)) result |= POLLOUT;
    if (events & EPOLLPRI) result |= POLLPRI;
    if (events & EPOLLRDHUP) result |= POLLRDHUP;
    return result;
}

static uint32_t poll_to_epoll(short events)
{
    uint32_t result = 0;
    if (events & (POLLIN | POLLRDNORM | POLLRDBAND)) result |= EPOLLIN;
    if (events & (POLLOUT | POLLWRNORM | POLLWRBAND)) result |= EPOLLOUT;
    if (events & POLLPRI) result |= EPOLLPRI;
    if (events & POLLERR) result |= EPOLLERR;
    if (events & POLLHUP) result |= EPOLLHUP;
    if (events & POLLRDHUP) result |= EPOLLRDHUP;
    if (events & POLLNVAL) result |= EPOLLERR;
    return result;
}

static struct task_epoll *task_epoll_for_fd(struct task *task, int fd)
{
    struct task_file *file = task_file_for_fd(task, fd);
    if (!file || !(file->flags & TASK_FILE_FLAG_EPOLL) || !file->aux) return NULL;
    return (struct task_epoll *)(uintptr_t)file->aux;
}

int64_t syscall_epoll_create(uint64_t size, uint32_t flags)
{
    struct task *task = sched_current_task();
    struct storage_node node = {
        .type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE,
    };
    struct task_epoll *epoll;
    struct task_file *file;
    int fd;
    if (!task || (flags & ~LINUX_O_CLOEXEC) || (!flags && !size)) return -RELIEFOS_EINVAL;
    epoll = kernel_malloc(sizeof(*epoll));
    if (!epoll) return -RELIEFOS_ENOMEM;
    *epoll = (struct task_epoll){0};
    fd = alloc_task_fd(task, &node, TASK_FILE_FLAG_EPOLL | RELIEFOS_O_RDWR |
                       (flags & LINUX_O_CLOEXEC), NULL);
    if (fd < 0) {
        kernel_free(epoll);
        return fd;
    }
    file = task_file_for_fd(task, fd);
    if (!file) {
        kernel_free(epoll);
        return -RELIEFOS_EMFILE;
    }
    file->aux = (uint64_t)(uintptr_t)epoll;
    return fd;
}

static int epoll_event_valid(uint32_t events)
{
    const uint32_t supported = EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLERR | EPOLLHUP |
        EPOLLRDNORM | EPOLLRDBAND | EPOLLWRNORM | EPOLLWRBAND | EPOLLMSG |
        EPOLLRDHUP | EPOLLEXCLUSIVE | EPOLLWAKEUP | EPOLLONESHOT | EPOLLET;
    return !(events & ~supported);
}

int64_t syscall_epoll_ctl(uint64_t epfd_arg, uint64_t op_arg,
                                 uint64_t fd_arg, uint64_t event_ptr)
{
    struct task *task = sched_current_task();
    struct task_epoll *epoll;
    struct task_file *target;
    struct epoll_event event;
    int epfd = (int)epfd_arg;
    int fd = (int)fd_arg;
    uint32_t op = (uint32_t)op_arg;
    int found = -1;
    if (!task || epfd_arg > INT32_MAX || fd_arg > INT32_MAX) return -RELIEFOS_EBADF;
    epoll = task_epoll_for_fd(task, epfd);
    target = task_file_for_fd(task, fd);
    /* PTY endpoints use a separate descriptor table. poll_impl also handles
     * them and the implicit standard streams of legacy PTY tasks. */
    int target_open = target != NULL || task_pty_endpoint_for_fd(task, fd) != NULL ||
                      task_pty_stream_for_fd(task, fd) >= 0;
    if (!epoll || !target_open || fd == epfd) return -RELIEFOS_EBADF;
    if (op != EPOLL_CTL_DEL && (!event_ptr || !user_range_ok(event_ptr, sizeof(event))))
        return -RELIEFOS_EFAULT;
    if (op != EPOLL_CTL_DEL) {
        event = *(const struct epoll_event *)(uintptr_t)event_ptr;
        if (!epoll_event_valid(event.events) || (event.events & EPOLLEXCLUSIVE))
            return -RELIEFOS_EINVAL;
    }
    for (uint32_t i = 0; i < TASK_EPOLL_MAX_ENTRIES; ++i) {
        if (epoll->item[i].active && epoll->item[i].fd == fd) {
            found = (int)i;
            break;
        }
    }
    if (op == EPOLL_CTL_ADD) {
        if (found >= 0) return -RELIEFOS_EEXIST;
        for (uint32_t i = 0; i < TASK_EPOLL_MAX_ENTRIES; ++i) {
            if (epoll->item[i].active) continue;
            epoll->item[i] = (struct task_epoll_entry){
                .fd = fd, .events = event.events, .data = event.data,
                .active = 1,
            };
            ++epoll->entries;
            kernel_poll_readiness_notify();
            return 0;
        }
        return -RELIEFOS_ENOSPC;
    }
    if (op == EPOLL_CTL_MOD) {
        if (found < 0) return -RELIEFOS_ENOENT;
        epoll->item[found].events = event.events;
        epoll->item[found].data = event.data;
        epoll->item[found].ready = 0;
        kernel_poll_readiness_notify();
        return 0;
    }
    if (op == EPOLL_CTL_DEL) {
        if (found < 0) return -RELIEFOS_ENOENT;
        epoll->item[found] = (struct task_epoll_entry){0};
        --epoll->entries;
        return 0;
    }
    return -RELIEFOS_EINVAL;
}

static int64_t syscall_epoll_wait_common(struct task *task, struct task_epoll *epoll,
                                         uint64_t events_ptr, uint32_t maxevents,
                                         int64_t timeout_ms)
{
    uint32_t written = 0;
    /* Linux validates maxevents as the caller's output capacity.  It is
     * independent from the number of descriptors currently registered in
     * this epoll instance (which is an internal implementation limit). */
    if (!maxevents || maxevents > INT32_MAX / sizeof(struct epoll_event))
        return -RELIEFOS_EINVAL;
    if (!user_range_writable(events_ptr, (uint64_t)maxevents * sizeof(struct epoll_event)))
        return -RELIEFOS_EFAULT;
    if (timeout_ms < -1) return -RELIEFOS_EINVAL;
    uint64_t poll_generation = kernel_poll_readiness_generation();
    for (uint32_t i = 0; i < TASK_EPOLL_MAX_ENTRIES && written < maxevents; ++i) {
        struct task_epoll_entry *entry = &epoll->item[i];
        struct pollfd pollfd;
        uint32_t ready;
        uint64_t generation;
        uint64_t deadline = task->poll_deadline_ticks;
        if (!entry->active || !entry->events) continue;
        pollfd = (struct pollfd){.fd = entry->fd, .events = (int16_t)epoll_to_poll(entry->events)};
        (void)syscall_poll_impl(task, &pollfd, 1, 0, poll_generation);
        /* poll(..., 0) is only a readiness probe here. Its zero-timeout
         * cleanup must not erase epoll_wait's blocking deadline. */
        if (timeout_ms != 0 && deadline)
            task->poll_deadline_ticks = deadline;
        ready = poll_to_epoll(pollfd.revents);
        generation = task_socket_event_generation(task_file_for_fd(task, entry->fd));
        if (!ready) {
            entry->ready = 0;
            entry->generation = generation;
            continue;
        }
        if (entry->events & EPOLLET) {
            /*
             * Edge-triggered readiness is tracked per event bit.  A UNIX
             * stream socket is normally writable, so treating any previous
             * readiness as a blanket suppression loses a later EPOLLIN edge
             * whenever Xorg registers EPOLLIN|EPOLLOUT together.
             */
            uint32_t edge = ready & ~entry->ready;
            uint32_t input = ready & (EPOLLIN | EPOLLPRI | EPOLLRDNORM |
                                      EPOLLRDBAND | EPOLLMSG | EPOLLRDHUP);
            /* A stream can be drained and refilled before the next epoll
             * probe.  The readiness bit then never passes through zero, but
             * rx_written still records a real new input edge. */
            if (input && generation != entry->generation) edge |= input;
            entry->ready = ready;
            entry->generation = generation;
            if (!edge) continue;
            ready = edge;
        }
        ((struct epoll_event *)(uintptr_t)events_ptr)[written++] =
            (struct epoll_event){.events = ready, .data = entry->data};
        if (!(entry->events & EPOLLET)) entry->ready = ready;
        if (entry->events & EPOLLONESHOT) entry->events = 0;
    }
    if (written) {
        task->poll_deadline_ticks = 0;
        return written;
    }
    if (timeout_ms == 0 || (task->poll_deadline_ticks &&
                            time_ticks() >= task->poll_deadline_ticks)) {
        task->poll_deadline_ticks = 0;
        return 0;
    }
    return syscall_poll_park(task, timeout_ms, poll_generation, epoll->entries != 0);
}

int64_t syscall_epoll_wait(uint64_t epfd_arg, uint64_t events_ptr,
                                  uint64_t maxevents_arg, int64_t timeout_ms)
{
    struct task *task = sched_current_task();
    struct task_epoll *epoll;
    kernel_poll_wait_remove(task);
    if (!task || epfd_arg > INT32_MAX || maxevents_arg > UINT32_MAX)
        return -RELIEFOS_EBADF;
    epoll = task_epoll_for_fd(task, (int)epfd_arg);
    if (!epoll) return -RELIEFOS_EBADF;
    return syscall_epoll_wait_common(task, epoll, events_ptr, (uint32_t)maxevents_arg, timeout_ms);
}

int64_t syscall_epoll_pwait2(uint64_t epfd, uint64_t events, uint64_t maxevents,
                                    uint64_t timeout, uint64_t sigmask, uint64_t sigsetsize)
{
    int64_t timeout_ms = -1;
    if (sigmask) {
        if (sigsetsize != sizeof(uint64_t) || !user_range_ok(sigmask, sizeof(uint64_t)))
            return -RELIEFOS_EINVAL;
        /* Atomic temporary signal masks are not yet exposed by the scheduler. */
        return -RELIEFOS_ENOSYS;
    }
    if (timeout) {
        struct linux_timespec value;
        if (!user_range_ok(timeout, sizeof(value))) return -RELIEFOS_EFAULT;
        value = *(const struct linux_timespec *)(uintptr_t)timeout;
        if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000LL)
            return -RELIEFOS_EINVAL;
        if (value.tv_sec > INT64_MAX / 1000) timeout_ms = INT64_MAX;
        else timeout_ms = value.tv_sec * 1000 + (value.tv_nsec + 999999) / 1000000;
    }
    return syscall_epoll_wait(epfd, events, maxevents, timeout_ms);
}
