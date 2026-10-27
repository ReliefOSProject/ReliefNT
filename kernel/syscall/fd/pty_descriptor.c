#include "../internal.h"



/**
 * @brief Find the PTY alias that represents one Linux descriptor.
 * @param task Descriptor-table owner; NULL is accepted.
 * @param fd Linux descriptor number, including implicit standard streams.
 * @return Borrowed alias entry or NULL when no PTY alias has that descriptor.
 */
struct task_pty_fd *task_pty_fd_for_fd(struct task *task, int fd)
{
    if (!task || fd < 0) return NULL;
    for (uint32_t i = 0; i < sched_task_pty_capacity(task); ++i) {
        struct task_pty_fd *entry = sched_task_pty_at(task, i);
        if (entry->used && entry->fd == fd) {
            return entry;
        }
    }
    return NULL;
}

struct task_pty_fd *task_pty_endpoint_for_fd(struct task *task, int fd)
{
    struct task_pty_fd *entry = task_pty_fd_for_fd(task, fd);
    return entry && entry->endpoint && entry->pty_id ? entry : NULL;
}

struct task_pty_fd *task_pty_for_io(struct task *task, int fd)
{
    if (task && task->syscall_file) return NULL;
    if (task && task->syscall_pty.used && task->syscall_fd == fd)
        return &task->syscall_pty;
    return task_pty_endpoint_for_fd(task, fd);
}

uint32_t task_pty_status(const struct task_pty_fd *entry)
{
    return entry->description ? entry->description->status_flags : entry->status_flags;
}

int task_pty_promote(struct task_pty_fd *entry)
{
    if (entry->description) return 0;
    struct task_pty_description *description = kernel_malloc(sizeof(*description));
    if (!description) return -RELIEFOS_ENOMEM;
    if (entry->pty_id) {
        int ret = pty_transfer_get(entry->pty_id, entry->endpoint);
        if (ret < 0) { kernel_free(description); return ret; }
    }
    *description = (struct task_pty_description){.references = 1,
        .status_flags = entry->status_flags, .pty_id = entry->pty_id, .endpoint = entry->endpoint};
    entry->description = description;
    return 0;
}

/**
 * @brief Drop one PTY descriptor alias and release its terminal ownership.
 * @param entry Alias entry to clear; null or already released aliases are ignored.
 * @note The final total description reference removes weak epoll watches before
 * dropping the PTY transfer and held device path references or freeing the
 * stable description. Final path release runs without a graph lock.
 */
void task_pty_release_entry(struct task_pty_fd *entry)
{
    uint32_t pty_id;
    if (!entry || !entry->used) {
        return;
    }
    pty_id = entry->pty_id;
    struct task_pty_description *description = entry->description;
    *entry = (struct task_pty_fd){0};
    if (description && --description->references == 0) {
        task_epoll_remove_pty_description(description);
        if (description->pty_id) pty_transfer_put(description->pty_id, description->endpoint);
        vfs_path_put(&description->path_ref);
        kernel_free(description);
    }
    if (pty_id) {
        pty_reap_hungup(pty_id);
    }
}

/**
 * @brief Install a PTY endpoint in the lowest available descriptor slot.
 * @param task Descriptor-table owner; required.
 * @param pty_id Live PTY session identifier.
 * @param endpoint Master or slave endpoint kind.
 * @param flags Linux open status flags, including optional O_CLOEXEC.
 * @return Installed descriptor or negative errno without publishing a partial alias.
 */
int task_pty_endpoint_fd(struct task *task, uint32_t pty_id,
                         uint32_t endpoint, uint32_t flags)
{
    if (!task || !pty_id || !endpoint || !pty_is_active(pty_id) ||
        !task_can_allocate_fd(task)) {
        return -RELIEFOS_EMFILE;
    }
    if (endpoint == TASK_PTY_ENDPOINT_SLAVE && !pty_slave_open_allowed(pty_id)) {
        return -RELIEFOS_EIO;
    }
    int candidate = task_unused_fd(task, 0);
    if (candidate < 0) return candidate;
    struct task_pty_fd *entry;
    int ret = sched_task_pty_alloc(task, &entry);
    if (ret < 0) return ret;
    *entry = (struct task_pty_fd){
        .used = 1, .fd = candidate,
        .flags = (flags & RELIEFOS_O_CLOEXEC) ? RELIEFOS_FD_CLOEXEC : 0,
        .status_flags = flags & (RELIEFOS_O_ACCMODE |
                                 RELIEFOS_O_APPEND |
                                 RELIEFOS_O_NONBLOCK),
        .stream = endpoint == TASK_PTY_ENDPOINT_MASTER ? 1u : 0u,
        .pty_id = pty_id, .endpoint = endpoint,
    };
    ret = task_pty_promote(entry);
    if (ret < 0) {
        *entry = (struct task_pty_fd){0};
        return ret;
    }
    if (candidate < 3) {
        sched_task_fds(task)->closed_stdio_mask &= ~(1u << candidate);
        sched_task_fds(task)->cloexec_stdio_mask &= ~(1u << candidate);
    }
    return candidate;
}

/**
 * @brief Parse a live numeric devpts path or resolve the active-VT alias.
 * @param path NUL-terminated endpoint path; NULL is rejected.
 * @param pty_id Writable result for the recognized active PTY identifier.
 * @return One for a recognized live endpoint path, zero for malformed, inactive,
 * or out-of-range paths.
 * @note Numeric PTY names are bounded by the signed Linux PTY identifier range.
 */
int task_pty_endpoint_path(const char *path, uint32_t *pty_id)
{
    const char *prefix = "/dev/pts/";
    uint32_t value = 0;
    uint32_t digits = 0;
    if (!path || !pty_id) return 0;
    if (pty_lookup_vt_path(path, NULL) == 0) {
        *pty_id = path[8] == '0' ? pty_vt_active() : (uint32_t)(path[8] - '0');
        return 1;
    }
    while (*prefix && *path && *prefix == *path) { ++prefix; ++path; }
    if (*prefix || !*path) return 0;
    while (*path >= '0' && *path <= '9') {
        uint32_t digit = (uint32_t)(*path - '0');
        if (value > (0x7fffffffu - digit) / 10u) return 0;
        value = value * 10u + digit;
        ++path; ++digits;
    }
    if (!digits || *path || !value || !pty_is_active(value)) return 0;
    *pty_id = value;
    return 1;
}

/**
 * Task pty stream for fd.
 * @param task Value supplied by the caller.
 * @param fd Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int task_pty_stream_for_fd(struct task *task, int fd)
{
    struct task_pty_fd *entry;
    struct task_file *file;
    if (!task || !task->pty_id) {
        return -1;
    }
    if (fd >= 0 && fd < (int)SCHED_TASK_STDIO_MAX &&
        (sched_task_fds(task)->closed_stdio_mask & (1u << (uint32_t)fd)) != 0) {
        return -1;
    }
    /**
 * @brief A redirected stdio descriptor shadows the implicit PTY stream.
 */
    file = task_file_for_fd(task, fd);
    if (file) {
        /* A real /dev/tty node is an alias for the caller's controlling PTY.
         * Keep it visible to termios/ioctl and terminal process-group calls,
         * while leaving /dev/console and ordinary files on their own paths. */
        if (task_device_is(file, STORAGE_DEV_KIND_TTY)) {
            return 0;
        }
        return -1;
    }
    entry = task_pty_fd_for_fd(task, fd);
    if (entry) return (int)entry->stream;
    return fd >= 0 && fd <= 2 ? fd : -1;
}

/**
 * @brief Materialize and promote an implicit PTY stream descriptor if needed.
 * @param task Descriptor-table owner; required.
 * @param fd Existing implicit stream or PTY alias descriptor.
 * @param out Receives a borrowed live alias entry; later PTY table growth can invalidate it.
 * @return Zero on success or negative errno without losing descriptor state.
 */
int task_pty_ensure_fd(struct task *task, int fd, struct task_pty_fd **out)
{
    if (!task || !out) return -RELIEFOS_EINVAL;
    struct task_pty_fd *entry = task_pty_fd_for_fd(task, fd);
    if (entry) {
        int ret = task_pty_promote(entry);
        if (ret < 0) return ret;
        *out = entry;
        return 0;
    }
    int stream = task_pty_stream_for_fd(task, fd);
    if (stream < 0 || task_file_for_fd(task, fd)) return -RELIEFOS_EBADF;
    int ret = sched_task_pty_alloc(task, &entry);
    if (ret < 0) return ret;
    *entry = (struct task_pty_fd){.used = 1, .fd = fd, .stream = (uint32_t)stream,
        .pty_id = task->pty_id, .endpoint = TASK_PTY_ENDPOINT_SLAVE,
        .status_flags = stream == 0 ? RELIEFOS_O_RDONLY : RELIEFOS_O_WRONLY,
        .flags = fd < 3 && (sched_task_fds(task)->cloexec_stdio_mask & (1u << fd))
            ? RELIEFOS_FD_CLOEXEC : 0};
    ret = task_pty_promote(entry);
    if (ret < 0) { *entry = (struct task_pty_fd){0}; return ret; }
    *out = entry;
    return 0;
}

/**
 * @brief Give implicit boot streams one shared, read/write /dev/console description.
 * @param task Descriptor owner; null and existing PTY streams are left alone.
 * @return Zero on success, negative ENODEV before VT setup, ENOMEM on allocation
 * failure, or the terminal reference error without publishing any descriptors.
 * Existing redirections and explicitly closed descriptors are preserved. Opening
 * /dev/console does not acquire a controlling terminal, matching Linux tty_open.
 */
int task_console_materialize_stdio(struct task *task)
{
    if (!task || task->pty_id) return 0;
    uint32_t missing = 0;
    for (int fd = 0; fd < 3; ++fd) {
        if (task_file_for_fd(task, fd) || task_pty_fd_for_fd(task, fd) ||
            (sched_task_fds(task)->closed_stdio_mask & (1u << fd))) continue;
        missing |= 1u << fd;
    }
    if (!missing) return 0;
    uint32_t terminal = pty_vt_id(pty_vt_active());
    if (!terminal) return -RELIEFOS_ENODEV;
    struct task_file *description = kernel_malloc(sizeof(*description));
    if (!description) return -RELIEFOS_ENOMEM;
    int ret = pty_transfer_get(terminal, TASK_PTY_ENDPOINT_SLAVE);
    if (ret < 0) {
        kernel_free(description);
        return ret;
    }
    *description = (struct task_file){.used = 1,
        .flags = TASK_FILE_FLAG_DEV_NODE | RELIEFOS_O_RDWR,
        .node = {.type = RELIEFOS_FS_TYPE_DEVICE, .flags = STORAGE_NODE_FLAG_DEV_NODE,
                 .first_cluster = STORAGE_DEV_KIND_CONSOLE},
        .aux = terminal};
    copy_text(description->path, sizeof(description->path), "/dev/console");
    for (int fd = 0; fd < 3; ++fd) {
        if (!(missing & (1u << fd))) continue;
        struct task_file *file = &sched_task_fds(task)->stdio_files[fd];
        *file = (struct task_file){.used = 1,
            .description = description,
            .fd_flags = (sched_task_fds(task)->cloexec_stdio_mask & (1u << fd)) ? RELIEFOS_FD_CLOEXEC : 0};
        ++description->references;
    }
    return 0;
}

/**
 * @brief Materialize inherited console or PTY streams before descriptor sharing.
 * @param task Descriptor owner; null is accepted.
 * @return Zero on success or a negative allocation/terminal-reference errno.
 */
int task_pty_materialize_stdio(struct task *task)
{
    int console_status = task_console_materialize_stdio(task);
    if (console_status < 0) return console_status;
    if (!task || !task->pty_id) return 0;
    for (int fd = 0; fd < 3; ++fd) {
        if (task_file_for_fd(task, fd) || task_pty_stream_for_fd(task, fd) < 0) continue;
        struct task_pty_fd *entry;
        int ret = task_pty_ensure_fd(task, fd, &entry);
        if (ret < 0) return ret;
    }
    return 0;
}

/** @brief Describe the original builtin terminal endpoint without path lookup.
 * @param task Borrowed current task under execution ownership.
 * @param fd Descriptor number with dispatcher-owned admission where applicable.
 * @param node Required caller-owned device identity output.
 * @param path Required output borrowing a static intrinsic metadata label.
 * @return Zero or negative errno. No namespace I/O, allocation or reference
 * transfer; an admitted endpoint remains stable across descriptor close/reuse.
 */
int task_pty_node_for_fd(struct task *task, int fd, struct storage_node *node,
                                 const char **path)
{
    struct task_pty_fd *endpoint = task_pty_for_io(task, fd);
    if (endpoint && endpoint->endpoint == TASK_PTY_ENDPOINT_MASTER) {
        *path = "/dev/ptmx";
        *node=(struct storage_node){.type=RELIEFOS_FS_TYPE_DEVICE,
            .flags=STORAGE_NODE_FLAG_DEV_NODE,.first_cluster=STORAGE_DEV_KIND_PTMX};
        return 0;
    }
    uint32_t id = endpoint ? endpoint->pty_id : task_pty_stream_for_fd(task, fd) >= 0 ? task->pty_id : 0;
    if (!id) return -RELIEFOS_EBADF;
    if (pty_vt_number(id)) {
        static const char *const vt_paths[] = {
            "/dev/tty1", "/dev/tty2", "/dev/tty3",
            "/dev/tty4", "/dev/tty5", "/dev/tty6"};
        *path = vt_paths[id - 1u];
    } else {
        *path = "/dev/pts";
    }
    return pty_get_node(id, node);
}

int task_pty_fd_available(struct task *task, int fd)
{
    if (!task || fd < 0 || task_file_for_fd(task, fd) || task_pty_fd_for_fd(task, fd)) {
        return 0;
    }
    if (fd < 3 && !(sched_task_fds(task)->closed_stdio_mask & (1u << fd))) return 0;
    return 1;
}

/**
 * @brief Duplicate one PTY descriptor at or above a requested minimum.
 * @param task Descriptor-table owner; required.
 * @param old_fd Open PTY or implicit PTY-stream descriptor.
 * @param minimum_fd Inclusive lowest descriptor number to allocate.
 * @param flags Per-descriptor flags; only FD_CLOEXEC is retained.
 * @return New descriptor or negative errno with source and target references intact.
 */
int task_pty_duplicate_fd(struct task *task, int old_fd, int minimum_fd,
                          uint32_t flags)
{
    if (!task) return -RELIEFOS_EBADF;
    struct task_pty_fd *source = task_pty_fd_for_fd(task, old_fd);
    int stream = task_pty_stream_for_fd(task, old_fd);
    int candidate;
    if (stream < 0 && !(source && source->endpoint)) {
        return -RELIEFOS_EBADF;
    }
    if (minimum_fd < 0 || (uint64_t)minimum_fd >= sched_task_limits(task)->nofile.rlim_cur) return -RELIEFOS_EINVAL;
    int ret = task_pty_ensure_fd(task, old_fd, &source);
    if (ret < 0) return ret;
    candidate = task_unused_fd(task, minimum_fd);
    if (candidate < 0) return candidate;
    /* Retain before growing because growth moves the task-owned alias vector. */
    struct task_pty_fd retained = *source;
    ++retained.description->references;
    struct task_pty_fd *entry;
    ret = sched_task_pty_alloc(task, &entry);
    if (ret < 0) {
        task_pty_release_entry(&retained);
        return ret;
    }
    *entry = retained;
    entry->fd = candidate;
    entry->flags = flags;
    /* A fresh descriptor never inherits FD_CLOEXEC, so a reused stdio slot
     * also drops the implicit close-on-exec bit. */
    if (candidate < 3) {
        sched_task_fds(task)->closed_stdio_mask &= ~(1u << candidate);
        sched_task_fds(task)->cloexec_stdio_mask &= ~(1u << candidate);
    }
    return candidate;
}

/**
 * @brief Retain a PTY alias in a caller-owned SCM_RIGHTS transfer entry.
 * @param task Descriptor-table owner; required.
 * @param fd Open PTY or implicit PTY stream descriptor.
 * @param out Caller-owned transfer entry receiving one retained reference.
 * @return Zero on success or negative errno without changing output ownership.
 */
int task_pty_export_fd(struct task *task, int fd, struct task_pty_fd *out)
{
    if (!task || !out || task_file_for_fd(task, fd)) return -RELIEFOS_EBADF;
    struct task_pty_fd *entry;
    int ret = task_pty_ensure_fd(task, fd, &entry);
    if (ret < 0) return ret;
    *out = *entry;
    ++out->description->references;
    out->flags = 0;
    return 0;
}

/**
 * @brief Import a retained PTY transfer entry into a task descriptor table.
 * @param task Receiving descriptor-table owner; required.
 * @param source Caller-owned live transfer entry; it remains owned by caller.
 * @param flags Per-descriptor flags to install on the received alias.
 * @return New descriptor or negative errno; source ownership is unchanged.
 * @note The source is snapshotted before possible table growth. If it points into
 * this same task's alias vector, growth may invalidate the caller's borrowed pointer.
 */
int task_pty_import_fd(struct task *task, const struct task_pty_fd *source, uint32_t flags)
{
    if (!task || !source || !source->used || !source->description) return -RELIEFOS_EBADF;
    int fd = task_unused_fd(task, 0);
    if (fd < 0) return fd;
    /* SCM_RIGHTS sources can be aliases in this same vector. Snapshot and
     * retain before allocating a slot, which may relocate that vector. */
    struct task_pty_fd retained = *source;
    ++retained.description->references;
    struct task_pty_fd *entry;
    int ret = sched_task_pty_alloc(task, &entry);
    if (ret < 0) {
        task_pty_release_entry(&retained);
        return ret;
    }
    *entry = retained;
    entry->fd = fd;
    entry->flags = flags;
    if (fd < 3) {
        sched_task_fds(task)->closed_stdio_mask &= ~(1u << fd);
        sched_task_fds(task)->cloexec_stdio_mask &= ~(1u << fd);
    }
    return fd;
}

/**
 * @brief Replace a descriptor with a retained PTY endpoint or implicit stream.
 * @param task Descriptor-table owner; required.
 * @param old_fd Open PTY or implicit PTY-stream descriptor.
 * @param new_fd Target descriptor, required to be below RLIMIT_NOFILE.
 * @return Target descriptor on success or negative errno without losing source ownership.
 */
int task_pty_dup2_fd(struct task *task, int old_fd, int new_fd)
{
    if (!task) return -RELIEFOS_EBADF;
    struct task_pty_fd *source = task_pty_fd_for_fd(task, old_fd);
    int stream = task_pty_stream_for_fd(task, old_fd);
    if (!task || (stream < 0 && !(source && source->endpoint)) || new_fd < 0)
        return -RELIEFOS_EBADF;
    if (old_fd == new_fd) return new_fd;
    if ((uint64_t)new_fd >= sched_task_limits(task)->nofile.rlim_cur) return -RELIEFOS_EBADF;
    int ret = task_pty_ensure_fd(task, old_fd, &source);
    if (ret < 0) return ret;
    struct task_pty_fd retained = *source;
    ++retained.description->references;
    retained.fd = new_fd;
    retained.flags = 0;
    struct task_pty_fd *entry = task_pty_fd_for_fd(task, new_fd);
    if (!entry) {
        ret = sched_task_pty_alloc(task, &entry);
        if (ret < 0) {
            task_pty_release_entry(&retained);
            return ret;
        }
    }
    /* Allocate/validate before replacing any existing target reference. */
    syscall_record_locks_close(task, task_descriptor_for_fd(task, new_fd));
    clear_task_file(task_descriptor_for_fd(task, new_fd));
    if (entry->used) task_pty_release_entry(entry);
    *entry = retained;
    if (new_fd < 3) {
        sched_task_fds(task)->closed_stdio_mask &= ~(1u << new_fd);
        /* dup2() clears FD_CLOEXEC on the new descriptor, including the
         * implicit stdio slot that has no task_file entry of its own. */
        sched_task_fds(task)->cloexec_stdio_mask &= ~(1u << new_fd);
    }
    return new_fd;
}
