#include "../internal.h"

/** @brief Translate Linux epoll interest bits into poll readiness bits. */
static uint32_t epoll_to_poll(uint32_t events);

/** @brief Translate poll readiness bits into Linux epoll event bits. */
static uint32_t poll_to_epoll(short events);

/** @brief Resolve an epoll descriptor to its heap-owned watch set. */
static struct task_epoll *task_epoll_for_fd(struct task *task, int fd);

/** @brief Check whether an epoll event mask contains only supported bits. */
static int epoll_event_valid(uint32_t events);

/** @brief Probe an epoll watch set and copy ready events to the caller. */
static int64_t syscall_epoll_wait_common(struct task *task, struct task_epoll *epoll,
                                         uint64_t events_ptr, uint32_t maxevents,
                                         int64_t timeout_ms);

/**
 * @brief Grow the epoll watch vector without publishing partial state.
 * @param epoll Epoll state serialized by the syscall execution transaction.
 * @return Zero after growth or -ENOMEM when capacity or allocation is exhausted.
 */
static int task_epoll_grow(struct task_epoll *epoll)
{
    uint32_t capacity = epoll->capacity;
    uint32_t new_capacity;
    size_t bytes;
    struct task_epoll_entry *item;

    if (!capacity) {
        new_capacity = TASK_EPOLL_INITIAL_ENTRIES;
    } else {
        if (capacity > UINT32_MAX / 2u) return -RELIEFOS_ENOMEM;
        new_capacity = capacity * 2u;
    }
    bytes = (size_t)new_capacity * sizeof(*item);
    if (new_capacity && bytes / sizeof(*item) != (size_t)new_capacity)
        return -RELIEFOS_ENOMEM;
    item = kernel_malloc(bytes);
    if (!item) return -RELIEFOS_ENOMEM;
    __builtin_memset(item, 0, bytes);
    if (epoll->item && capacity)
        __builtin_memcpy(item, epoll->item, (size_t)capacity * sizeof(*item));
    kernel_free(epoll->item);
    epoll->item = item;
    epoll->capacity = new_capacity;
    return 0;
}


/**
 * @brief Detach one watch from its borrowed target's reverse list.
 * @param watch Watch node to unlink; null is ignored.
 * @note The target must still be live. Final target release removes its list
 * directly instead of calling this helper.
 */
static void task_epoll_watch_detach(struct task_epoll_watch *watch)
{
    if (!watch) return;
    struct task_epoll_watch **cursor = watch->file ? &watch->file->epoll_watches :
        (watch->pty ? &watch->pty->epoll_watches : NULL);
    while (cursor && *cursor && *cursor != watch) cursor = &(*cursor)->next;
    if (cursor && *cursor == watch) *cursor = watch->next;
}

/**
 * @brief Clear an epoll slot if it still owns the supplied watch node.
 * @param watch Watch node whose epoll slot is being removed.
 * @return True when a live slot was cleared, false if the owner already retired it.
 */
static bool task_epoll_watch_clear_entry(struct task_epoll_watch *watch)
{
    struct task_epoll *epoll = watch ? watch->epoll : NULL;
    if (!epoll || watch->slot >= epoll->capacity) return false;
    struct task_epoll_entry *entry = &epoll->item[watch->slot];
    if (!entry->active || entry->watch != watch) return false;
    *entry = (struct task_epoll_entry){0};
    if (epoll->entries) --epoll->entries;
    return true;
}

/**
 * @brief Remove a watch from its target list and owning epoll slot.
 * @param watch Heap-allocated registration node; null is ignored.
 * @return True when an active epoll slot was removed.
 */
static bool task_epoll_watch_remove(struct task_epoll_watch *watch)
{
    bool removed;
    if (!watch) return false;
    task_epoll_watch_detach(watch);
    removed = task_epoll_watch_clear_entry(watch);
    kernel_free(watch);
    return removed;
}

/**
 * @brief Retire all epoll interests before a file OFD's final free.
 * @param description Stable file description whose reference count reached zero.
 * @note Caller holds the global kernel execution transaction. The weak list is
 * consumed before any endpoint or other description-owned resource is released.
 */
void task_epoll_remove_file_description(struct task_file *description)
{
    bool removed = false;
    if (!description) return;
    while (description->epoll_watches) {
        struct task_epoll_watch *watch = description->epoll_watches;
        description->epoll_watches = watch->next;
        watch->file = NULL;
        removed |= task_epoll_watch_clear_entry(watch);
        kernel_free(watch);
    }
    if (removed) kernel_poll_readiness_notify();
}

/**
 * @brief Retire all epoll interests before a PTY OFD's final free.
 * @param description Stable PTY description whose reference count reached zero.
 * @note Caller holds the global kernel execution transaction. The weak list is
 * consumed before the terminal transfer reference is released.
 */
void task_epoll_remove_pty_description(struct task_pty_description *description)
{
    bool removed = false;
    if (!description) return;
    while (description->epoll_watches) {
        struct task_epoll_watch *watch = description->epoll_watches;
        description->epoll_watches = watch->next;
        watch->pty = NULL;
        removed |= task_epoll_watch_clear_entry(watch);
        kernel_free(watch);
    }
    if (removed) kernel_poll_readiness_notify();
}

/**
 * @brief Detach all weak target links and release an epoll state object.
 * @param epoll Heap-owned epoll instance; null is ignored.
 * @note Each registration node is unlinked from its target before the vector
 * and state object are freed; watched open descriptions are never retained here.
 */
void task_epoll_destroy(struct task_epoll *epoll)
{
    if (!epoll) return;
    for (uint32_t i = 0; i < epoll->capacity; ++i) {
        struct task_epoll_watch *watch = epoll->item[i].watch;
        if (!watch) continue;
        task_epoll_watch_detach(watch);
        kernel_free(watch);
    }
    kernel_free(epoll->item);
    kernel_free(epoll);
}

/**
 * @brief Translate Linux epoll interests into the poll implementation's mask.
 * @param events Linux epoll interest mask.
 * @return Equivalent POLLIN/POLLOUT/POLLPRI/POLLRDHUP bits.
 */
static uint32_t epoll_to_poll(uint32_t events)
{
    uint32_t result = 0;
    if (events & (EPOLLIN | EPOLLRDNORM | EPOLLRDBAND | EPOLLMSG)) result |= POLLIN;
    if (events & (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)) result |= POLLOUT;
    if (events & EPOLLPRI) result |= POLLPRI;
    if (events & EPOLLRDHUP) result |= POLLRDHUP;
    return result;
}

/**
 * @brief Translate poll results into the Linux epoll result mask.
 * @param events Poll result mask.
 * @return Equivalent epoll readiness bits.
 */
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

/**
 * @brief Resolve a descriptor to an epoll object owned by the task.
 * @param task Descriptor-table owner.
 * @param fd Candidate epoll descriptor.
 * @return Epoll state or null when the descriptor is not an epoll instance.
 */
static struct task_epoll *task_epoll_for_fd(struct task *task, int fd)
{
    struct task_file *file = task_file_for_fd(task, fd);
    if (!file || !(file->flags & TASK_FILE_FLAG_EPOLL) || !file->aux) return NULL;
    return (struct task_epoll *)(uintptr_t)file->aux;
}

/**
 * @brief Check whether an epoll OFD has an eligible event without consuming it.
 * @param task Task whose descriptor and syscall-I/O overrides are used to probe.
 * @param file Epoll open-file description to inspect.
 * @return True when an armed registration is currently ready, otherwise false.
 * @note The probe preserves the caller's I/O overrides and poll deadline. It
 *       does not advance edge state or disarm one-shot registrations.
 */
bool task_epoll_poll_ready(struct task *task, struct task_file *file)
{
    struct task_epoll *epoll;
    uint64_t poll_generation;
    uint64_t saved_deadline;

    if (!task || !file || !(file->flags & TASK_FILE_FLAG_EPOLL) || !file->aux)
        return false;
    epoll = (struct task_epoll *)(uintptr_t)file->aux;
    poll_generation = kernel_poll_readiness_generation();
    saved_deadline = task->poll_deadline_ticks;
    for (uint32_t i = 0; i < epoll->capacity; ++i) {
        struct task_epoll_entry *entry = &epoll->item[i];
        struct task_epoll_watch *watch = entry->watch;
        struct pollfd pollfd;
        struct task_file *saved_file;
        struct task_pty_fd saved_pty;
        int saved_fd;
        uint32_t ready;
        uint64_t generation;

        if (!entry->active || entry->disabled || !watch ||
            (!watch->file && !watch->pty) ||
            (watch->file && (watch->file->flags & TASK_FILE_FLAG_EPOLL)))
            continue;
        pollfd = (struct pollfd){
            .fd = entry->fd,
            .events = (int16_t)epoll_to_poll(entry->events),
        };
        saved_file = task->syscall_file;
        saved_pty = task->syscall_pty;
        saved_fd = task->syscall_fd;
        task->syscall_fd = entry->fd;
        task->syscall_file = watch->file;
        task->syscall_pty = watch->pty ? watch->pty_alias : (struct task_pty_fd){0};
        (void)syscall_poll_impl(task, &pollfd, 1, 0, poll_generation);
        task->syscall_file = saved_file;
        task->syscall_pty = saved_pty;
        task->syscall_fd = saved_fd;
        task->poll_deadline_ticks = saved_deadline;

        ready = poll_to_epoll(pollfd.revents);
        if (!ready) continue;
        generation = task_socket_event_generation(watch->file);
        if (entry->events & EPOLLET) {
            uint32_t edge = ready & ~entry->ready;
            uint32_t input = ready & (EPOLLIN | EPOLLPRI | EPOLLRDNORM |
                                      EPOLLRDBAND | EPOLLMSG | EPOLLRDHUP);
            if (input && generation != entry->generation) edge |= input;
            if (!edge) continue;
        }
        return true;
    }
    task->poll_deadline_ticks = saved_deadline;
    return false;
}

/**
 * @brief Create an epoll descriptor with an initially empty dynamic watch set.
 * @param size Linux epoll_create size hint; zero is invalid only for the legacy call.
 * @param flags Supported creation flags, currently only CLOEXEC.
 * @return Allocated descriptor or negative errno without publishing partial state.
 */
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
        task_epoll_destroy(epoll);
        return fd;
    }
    file = task_file_for_fd(task, fd);
    if (!file) {
        task_epoll_destroy(epoll);
        return -RELIEFOS_EMFILE;
    }
    file->aux = (uint64_t)(uintptr_t)epoll;
    return fd;
}

/**
 * @brief Validate supported Linux epoll control event bits.
 * @param events User interest mask copied from a checked event structure.
 * @return One when every bit is supported, zero when an unknown bit is set.
 */
static int epoll_event_valid(uint32_t events)
{
    const uint32_t supported = EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLERR | EPOLLHUP |
        EPOLLRDNORM | EPOLLRDBAND | EPOLLWRNORM | EPOLLWRBAND | EPOLLMSG |
        EPOLLRDHUP | EPOLLEXCLUSIVE | EPOLLWAKEUP | EPOLLONESHOT | EPOLLET;
    return !(events & ~supported);
}

/**
 * @brief Add, modify, or delete one descriptor in an epoll watch set.
 * @param epfd_arg Epoll descriptor number as received from the syscall ABI.
 * @param op_arg EPOLL_CTL_ADD, EPOLL_CTL_MOD, or EPOLL_CTL_DEL.
 * @param fd_arg Watched descriptor number as received by the syscall ABI; the
 *        registration key also records the descriptor's current OFD identity.
 * @param event_ptr Checked user event pointer for ADD/MOD; unused for DEL.
 * @return Zero on success or a negative Linux errno; failed ADD leaves no watch.
 * @note Distinct descriptors referencing one OFD have separate registrations.
 * Closing a number alone does not remove the watch while any total OFD reference
 * (descriptor, SCM_RIGHTS, or in-flight I/O pin) remains.
 * Adding an epoll OFD as a target is unsupported and returns -EPERM.
 */
int64_t syscall_epoll_ctl(uint64_t epfd_arg, uint64_t op_arg,
                                 uint64_t fd_arg, uint64_t event_ptr)
{
    struct task *task = sched_current_task();
    struct task_epoll *epoll;
    struct task_file *target;
    struct task_file *target_pin = NULL;
    struct task_pty_fd *target_pty = NULL;
    struct task_pty_fd pty_snapshot = {0};
    struct task_epoll_watch *watch;
    struct epoll_event event = {0};
    int epfd = (int)epfd_arg;
    int fd = (int)fd_arg;
    uint32_t op = (uint32_t)op_arg;
    int found = -1;
    if (!task || epfd_arg > INT32_MAX || fd_arg > INT32_MAX) return -RELIEFOS_EBADF;
    epoll = task_epoll_for_fd(task, epfd);
    target = task_file_for_fd(task, fd);
    if (!epoll || fd == epfd) return -RELIEFOS_EBADF;
    if (op != EPOLL_CTL_DEL && (!event_ptr || !user_range_ok(event_ptr, sizeof(event))))
        return -RELIEFOS_EFAULT;
    if (op != EPOLL_CTL_DEL) {
        event = *(const struct epoll_event *)(uintptr_t)event_ptr;
        if (!epoll_event_valid(event.events) || (event.events & EPOLLEXCLUSIVE))
            return -RELIEFOS_EINVAL;
    }
    if (op == EPOLL_CTL_ADD && target && (target->flags & TASK_FILE_FLAG_EPOLL))
        return -RELIEFOS_EPERM;
    if (target) {
        target_pin = task_file_get(target);
        if (!target_pin) return -RELIEFOS_ENOMEM;
    } else {
        int ret = task_pty_ensure_fd(task, fd, &target_pty);
        if (ret < 0) return ret == -RELIEFOS_ENOMEM ? ret : -RELIEFOS_EBADF;
        pty_snapshot = *target_pty;
    }
    for (uint32_t i = 0; i < epoll->capacity; ++i) {
        struct task_epoll_watch *candidate = epoll->item[i].watch;
        bool same_target = target_pin ?
            candidate && candidate->file == target_pin :
            candidate && candidate->pty == pty_snapshot.description;
        if (epoll->item[i].active && epoll->item[i].fd == fd && same_target) {
            found = (int)i;
            break;
        }
    }
    int64_t result = 0;
    if (op == EPOLL_CTL_ADD) {
        if (found >= 0) { result = -RELIEFOS_EEXIST; goto out; }
        uint32_t slot = epoll->capacity;
        for (uint32_t i = 0; i < epoll->capacity; ++i) {
            if (epoll->item[i].active) continue;
            slot = i;
            break;
        }
        if (slot == epoll->capacity) {
            int ret = task_epoll_grow(epoll);
            if (ret < 0) { result = ret; goto out; }
        }
        watch = kernel_malloc(sizeof(*watch));
        if (!watch) { result = -RELIEFOS_ENOMEM; goto out; }
        *watch = (struct task_epoll_watch){.epoll = epoll,
            .file = target_pin, .pty = target_pin ? NULL : pty_snapshot.description,
            .pty_alias = pty_snapshot, .slot = slot};
        if (target_pin) {
            watch->next = target_pin->epoll_watches;
            target_pin->epoll_watches = watch;
        } else {
            watch->next = pty_snapshot.description->epoll_watches;
            pty_snapshot.description->epoll_watches = watch;
        }
        epoll->item[slot] = (struct task_epoll_entry){
            .fd = fd, .events = event.events, .data = event.data,
            .active = 1, .watch = watch,
        };
        ++epoll->entries;
        kernel_poll_readiness_notify();
        goto out;
    }
    if (op == EPOLL_CTL_MOD) {
        if (found < 0) { result = -RELIEFOS_ENOENT; goto out; }
        epoll->item[found].events = event.events;
        epoll->item[found].data = event.data;
        epoll->item[found].ready = 0;
        epoll->item[found].disabled = 0;
        epoll->item[found].generation = 0;
        kernel_poll_readiness_notify();
        goto out;
    }
    if (op == EPOLL_CTL_DEL) {
        if (found < 0) { result = -RELIEFOS_ENOENT; goto out; }
        watch = epoll->item[found].watch;
        if (watch) (void)task_epoll_watch_remove(watch);
        else {
            epoll->item[found] = (struct task_epoll_entry){0};
            --epoll->entries;
        }
        kernel_poll_readiness_notify();
        goto out;
    }
    result = -RELIEFOS_EINVAL;
out:
    if (target_pin) task_file_put(target_pin);
    return result;
}

/**
 * @brief Probe registered descriptors and return up to maxevents ready entries.
 * @param task Task whose descriptor table and timeout state are used.
 * @param epoll Dynamic watch set owned by the task's epoll open-file description.
 * @param events_ptr Writable user event array validated before any copyout.
 * @param maxevents Caller-provided event capacity, checked against the ABI limit.
 * @param timeout_ms Linux timeout in milliseconds, with -1 meaning indefinite.
 * @return Number of copied events, zero on timeout, or a negative errno/block token.
 */
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
    for (uint32_t i = 0; i < epoll->capacity && written < maxevents; ++i) {
        struct task_epoll_entry *entry = &epoll->item[i];
        struct pollfd pollfd;
        uint32_t ready;
        uint64_t generation;
        uint64_t deadline = task->poll_deadline_ticks;
        struct task_file *saved_file;
        struct task_pty_fd saved_pty;
        int saved_fd;
        if (!entry->active || entry->disabled) continue;
        pollfd = (struct pollfd){.fd = entry->fd, .events = (int16_t)epoll_to_poll(entry->events)};
        saved_file = task->syscall_file;
        saved_pty = task->syscall_pty;
        saved_fd = task->syscall_fd;
        task->syscall_fd = entry->fd;
        task->syscall_file = entry->watch ? entry->watch->file : NULL;
        task->syscall_pty = entry->watch ? entry->watch->pty_alias : (struct task_pty_fd){0};
        (void)syscall_poll_impl(task, &pollfd, 1, 0, poll_generation);
        task->syscall_file = saved_file;
        task->syscall_pty = saved_pty;
        task->syscall_fd = saved_fd;
        /* poll(..., 0) is only a readiness probe here. Its zero-timeout
         * cleanup must not erase epoll_wait's blocking deadline. */
        if (timeout_ms != 0 && deadline)
            task->poll_deadline_ticks = deadline;
        ready = poll_to_epoll(pollfd.revents);
        generation = task_socket_event_generation(entry->watch ? entry->watch->file : NULL);
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
        if (entry->events & EPOLLONESHOT) entry->disabled = 1;
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

/**
 * @brief Wait for events on an epoll descriptor using Linux syscall arguments.
 * @param epfd_arg Epoll descriptor number.
 * @param events_ptr Writable output array of epoll_event records.
 * @param maxevents_arg Number of output records available to the caller.
 * @param timeout_ms Millisecond timeout, -1 for indefinite, or nonnegative.
 * @return Event count, zero on timeout, or a negative errno/block token.
 */
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

/**
 * @brief Apply epoll_pwait2's timespec timeout and signal-mask validation.
 * @param epfd Epoll descriptor number.
 * @param events Writable output event array.
 * @param maxevents Output event capacity.
 * @param timeout Optional Linux timespec pointer; null means indefinite.
 * @param sigmask Optional signal-mask pointer, currently unsupported.
 * @param sigsetsize Signal-mask byte size.
 * @return Event count, zero on timeout, or a negative Linux errno.
 */
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
