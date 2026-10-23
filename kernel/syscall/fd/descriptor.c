#include "../internal.h"

static struct task_file *task_file_promote(struct task_file *source);

/**
 * @brief Release every file and PTY descriptor reference owned by a task table.
 * @param task Descriptor-table owner; NULL is accepted.
 */
static void clear_task_files(struct task *task);


/**
 * @brief Release a descriptor or canonical open-file description and its resources.
 * @param file Descriptor slot or stable OFD to clear; null and unused entries are ignored.
 * @return Backing inode release result, or zero when no inode release is needed.
 * @note A final canonical release detaches weak epoll watches before releasing
 * OFD-owned resources. SCM queue and in-flight I/O references use this same path.
 */
int clear_task_file_result(struct task_file *file)
{
    /**
 * @brief Descriptor cleanup can be reached both from the immediate exit path and from later zombie reaping. A released pipe end must only decrement its shared reference count once.
 */
    if (!file || !file->used) {
        return 0;
    }
    if (file->description) {
        struct task_file *description = file->description;
        int result = 0;
        *file = (struct task_file){0};
        if (--description->references == 0) {
            syscall_flock_release_owner(description);
            result = clear_task_file_result(description);
            kernel_free(description);
        }
        task_socket_collect();
        return result;
    }
    if (file->references == 0) task_epoll_remove_file_description(file);
    syscall_flock_release_owner(file);
    if (file->drm_prime_file) drm_prime_file_close(file);
    if (file->flags & TASK_FILE_FLAG_DEV_NODE) {
        if (file->node.first_cluster == STORAGE_DEV_KIND_CONSOLE && file->aux &&
            !(file->flags & TASK_FILE_FLAG_PATH))
            pty_transfer_put((uint32_t)file->aux, TASK_PTY_ENDPOINT_SLAVE);
        if (file->drm_file) drm_file_close(file);
        if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_PCM)
            (void)audio_device_close(file);
        if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_CONTROL)
            (void)audio_control_close(file);
        if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_MIXER)
            (void)audio_mixer_close(file);
        if (file->node.flags & STORAGE_NODE_FLAG_AUDIO_TIMER)
            (void)audio_timer_close(file);
        if (file->node.first_cluster == STORAGE_DEV_KIND_AUDIO &&
            !(file->node.flags & (STORAGE_NODE_FLAG_AUDIO_PCM |
                                  STORAGE_NODE_FLAG_AUDIO_CONTROL |
                                  STORAGE_NODE_FLAG_AUDIO_MIXER)))
            (void)audio_oss_close(file);
    }
    int result = storage_inode_put(file->inode);
    file->inode = NULL;
    task_pipe_release(file);
    task_socket_release(file);
    task_inet_release(file);
    task_shm_release(file);
    if ((file->flags & TASK_FILE_FLAG_EPOLL) && file->aux) {
        task_epoll_destroy((struct task_epoll *)(uintptr_t)file->aux);
    }
    if ((file->flags & TASK_FILE_FLAG_TIMERFD) && file->aux) {
        kernel_free((void *)(uintptr_t)file->aux);
    }
    if ((file->flags & TASK_FILE_FLAG_DEV_NODE) &&
        (file->node.first_cluster == STORAGE_DEV_KIND_KEYBOARD ||
         file->node.first_cluster == STORAGE_DEV_KIND_MOUSE) &&
        file->aux2 != 0) {
        input_evdev_release(file->node.first_cluster, file->aux2);
    }
    file->used = 0;
    file->kind = 0;
    file->node.type = 0;
    file->node.flags = 0;
    file->node.first_cluster = 0;
    file->node.volume_id = 0;
    file->node.size = 0;
    file->offset = 0;
    file->aux = 0;
    file->aux2 = 0;
    file->input_vt = 0;
    file->input_vt_manual = 0;
    file->audio_file = NULL;
    file->audio_control_file = NULL;
    file->audio_oss_file = NULL;
    file->audio_timer_file = NULL;
    file->drm_file = NULL;
    file->drm_prime_file = NULL;
    file->drm_prime_ino = 0;
    file->drm_prime_size = 0;
    file->drm_prime_uid = 0;
    file->drm_prime_gid = 0;
    file->read_cursor = (struct storage_read_cursor){0};
    file->flags = 0;
    file->fd_flags = 0;
    file->flock_type = 0;
    file->path[0] = 0;
    return result;
}

/**
 * @brief Clear an OFD slot and release any resources still attached to it.
 * @param file Descriptor or description slot; null and unused values are ignored.
 */
void clear_task_file(struct task_file *file)
{
    (void)clear_task_file_result(file);
}

/**
 * @brief Promote an inline descriptor into a stable heap-owned OFD when needed.
 * @param source Live descriptor or canonical description; caller owns its lifetime.
 * @return Borrowed canonical description, or null when allocation fails.
 * @note Promotion does not add a reference. Newly promoted descriptions begin
 * with the source descriptor's reference; epoll watch lists start empty.
 */
static struct task_file *task_file_promote(struct task_file *source)
{
    if (source->description) return source->description;
    if (source->references) return source;
    struct task_file *description = kernel_malloc(sizeof(*description));
    if (!description) return NULL;
    *description = *source;
    description->references = 1;
    description->epoll_watches = NULL;
    description->fd_flags = 0;
    /* Promotion moves the OFD, not its locks. Retarget the lock table before
     * dup/fork or an I/O pin can retire/reuse the inline descriptor slot. */
    if (source->flock_type) {
        for (uint32_t i = 0; i < RELIEFOS_FLOCK_MAX; ++i) {
            struct reliefos_flock_entry *entry = &reliefos_flocks[i];
            if (!entry->used) continue;
            if (entry->owner == source) entry->owner = description;
            for (uint32_t j = 0; j < entry->shared_count; ++j)
                if (entry->shared_owners[j] == source)
                    entry->shared_owners[j] = description;
        }
    }
    source->description = description;
    return description;
}

/**
 * @brief Install one descriptor alias and increment the source OFD reference.
 * @param destination Unused descriptor slot to initialize; must not alias source.
 * @param source Live source descriptor or canonical OFD.
 * @return Zero on success, -EBADF for invalid arguments, or -ENOMEM on promotion failure.
 */
int task_file_reference(struct task_file *destination, struct task_file *source)
{
    if (!source || !source->used || !destination) return -RELIEFOS_EBADF;
    struct task_file *description = task_file_promote(source);
    if (!description) return -RELIEFOS_ENOMEM;
    *destination = (struct task_file){.used = 1, .description = description};
    ++description->references;
    return 0;
}

/**
 * @brief Retain a transient reference to a stable canonical file description.
 * @param source Live descriptor or description; caller must serialize against close.
 * @return Canonical OFD with one additional reference, or null for invalid input
 * or allocation failure during promotion.
 */
struct task_file *task_file_get(struct task_file *source)
{
    if (!source || !source->used) return NULL;
    struct task_file *description = task_file_promote(source);
    if (description) ++description->references;
    return description;
}

/**
 * @brief Drop one transient OFD reference and perform final cleanup when needed.
 * @param description Canonical file description returned by task_file_get().
 * @note The final put detaches epoll watchers before releasing description-owned
 * resources, so no watch can extend or outlive the OFD.
 */
void task_file_put(struct task_file *description)
{
    if (description && --description->references == 0) {
        clear_task_file(description);
        kernel_free(description);
    }
}

/**
 * @brief Release every file and PTY descriptor reference owned by a task table.
 * @param task Descriptor-table owner; NULL is accepted.
 */
static void clear_task_files(struct task *task)
{
    if (!task) {
        return;
    }
    for (uint32_t i = 0; i < sched_task_file_capacity(task); ++i) {
        struct task_file *file = sched_task_file_at(task, i);
        if (file) {
            syscall_record_locks_close(task, file);
            clear_task_file(file);
        }
    }
    for (uint32_t i = 0; i < SCHED_TASK_STDIO_MAX; ++i) {
        syscall_record_locks_close(task, &sched_task_fds(task)->stdio_files[i]);
        clear_task_file(&sched_task_fds(task)->stdio_files[i]);
    }
    for (uint32_t i = 0; i < sched_task_pty_capacity(task); ++i)
        task_pty_release_entry(sched_task_pty_at(task, i));
}

/**
 * @brief Release a final task's descriptor entries and their backing vectors.
 * @param task Task whose file-table reference is already known to be final; NULL is accepted.
 */
void syscall_release_task_files(struct task *task)
{
    if (task) {
        net_close_owner_sockets(task->pid);
    }
    clear_task_files(task);
    sched_task_file_release(task);
    sched_task_pty_release(task);
}

/**
 * @brief Retain copied pipe and PTY endpoint references in a fork child's private table.
 * @param parent Source process, required to prevent accidental use on detached task records.
 * @param child Fork child whose copied endpoint entries require new references.
 * @return Zero on success or a negative errno-style allocation/reference error.
 * @note PTY alias slots are reserved before references are copied. If this fails
 * before the copy pass, no child PTY references are published; clone rollback owns
 * and releases any detached child vector.
 */
int syscall_clone_task_files(const struct task *parent, struct task *child)
{
    if (!parent || !child) {
        return -RELIEFOS_EINVAL;
    }
    int ret = task_pty_materialize_stdio((struct task *)parent);
    if (ret < 0) return ret;
    struct task_fd_table_state *child_files = sched_task_fds(child);
    const struct task_fd_table_state *parent_files = sched_task_fds(parent);
    if (child_files == parent_files) return 0;
    /* White-box callers may have copied the whole table before asking us to
     * retain the child's references. Never let the child alias the parent's
     * independently owned PTY vector. */
    if (child_files->pty_fds == parent_files->pty_fds) {
        child_files->pty_fds = NULL;
        child_files->pty_fd_count = 0;
        child_files->pty_fd_capacity = 0;
    }
    uint32_t pty_count = parent_files->pty_fd_count;
    ret = sched_task_pty_table_reserve(child_files, pty_count);
    if (ret < 0) return ret;
    child_files->pty_fd_count = pty_count;
    /* Finish all allocations before child references are published, so an
     * allocation failure cannot leak half of the inherited descriptor set. */
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < pty_count; ++i) {
            struct task_pty_fd *source = &parent_files->pty_fds[i];
            if (!source->used) continue;
            if (!pass) {
                ret = task_pty_promote(source);
                if (ret < 0) return ret;
            } else {
                *sched_task_pty_at(child, i) = *source;
                ++source->description->references;
            }
        }
        for (uint32_t i = 0; i < sched_task_file_capacity(child) + 3; ++i) {
            struct task_file *source = i < 3 ? &sched_task_fds(parent)->stdio_files[i] :
                sched_task_file_at((struct task *)parent, i - 3);
            struct task_file *destination = i < 3 ? &sched_task_fds(child)->stdio_files[i] :
                sched_task_file_at(child, i - 3);
            if (!source || !source->used) continue;
            if (!pass) {
                if (!task_file_promote(source)) return -RELIEFOS_ENOMEM;
            } else {
                task_file_reference(destination, source);
                destination->fd_flags = source->fd_flags;
            }
        }
    }
    return 0;
}

/**
 * @brief Detach a task from a multiply referenced descriptor table.
 * @param task Task whose descriptor table is shared; required.
 * @return Zero when already private or after a successful copy, otherwise a negative
 * allocation or terminal-reference error while preserving the installed shared table.
 */
int syscall_unshare_task_files(struct task *task)
{
    struct task_fd_table_state *old = task->shared_files;
    if (!old || old->references == 1) return 0;
    int ret = task_pty_materialize_stdio(task);
    if (ret < 0) return ret;
    struct task_fd_table_state *copy = kernel_malloc(sizeof(*copy));
    if (!copy) return -RELIEFOS_ENOMEM;
    *copy = *old;
    copy->references = 1;
    copy->lock_owner = 0;
    copy->file_extra = NULL;
    copy->pty_fds = NULL;
    copy->pty_fd_count = 0;
    copy->pty_fd_capacity = 0;
    if (old->file_extra_capacity) {
        copy->file_extra = kernel_malloc(old->file_extra_capacity * sizeof(struct task_file));
        if (!copy->file_extra) { kernel_free(copy); return -RELIEFOS_ENOMEM; }
        for (uint32_t i = 0; i < old->file_extra_capacity; ++i) copy->file_extra[i] = old->file_extra[i];
    }
    ret = sched_task_pty_table_reserve(copy, old->pty_fd_count);
    if (ret < 0) {
        kernel_free(copy->file_extra);
        kernel_free(copy);
        return ret;
    }
    copy->pty_fd_count = old->pty_fd_count;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < old->pty_fd_count; ++i) {
            struct task_pty_fd *source = &old->pty_fds[i];
            if (!source->used) continue;
            if (!pass) {
                ret = task_pty_promote(source);
                if (ret < 0) {
                    kernel_free(copy->file_extra);
                    kernel_free(copy->pty_fds);
                    kernel_free(copy);
                    return ret;
                }
            } else {
                copy->pty_fds[i] = *source;
                ++source->description->references;
            }
        }
        for (uint32_t i = 0; i < 3 + SCHED_TASK_FILE_MAX + old->file_extra_count; ++i) {
            struct task_file *source = i < 3 ? &old->stdio_files[i] :
                i < 3 + SCHED_TASK_FILE_MAX ? &old->files[i - 3] :
                &old->file_extra[i - 3 - SCHED_TASK_FILE_MAX];
            struct task_file *destination = i < 3 ? &copy->stdio_files[i] :
                i < 3 + SCHED_TASK_FILE_MAX ? &copy->files[i - 3] :
                &copy->file_extra[i - 3 - SCHED_TASK_FILE_MAX];
            if (!source->used) continue;
            if (!pass) {
                if (!task_file_promote(source)) {
                    kernel_free(copy->file_extra);
                    kernel_free(copy->pty_fds);
                    kernel_free(copy);
                    return -RELIEFOS_ENOMEM;
                }
            } else {
                task_file_reference(destination, source);
                destination->fd_flags = source->fd_flags;
            }
        }
    }
    --old->references;
    task->shared_files = copy;
    return 0;
}

/**
 * @brief Closes file and explicit PTY aliases marked FD_CLOEXEC for execve.
 * @param task Process whose existing descriptor table survives the image replacement.
 */
void syscall_close_cloexec_files(struct task *task)
{
    if (!task) {
        return;
    }
    for (uint32_t i = 0; i < sched_task_file_capacity(task); ++i) {
        struct task_file *file = sched_task_file_at(task, i);
        if (file && file->used && (file->fd_flags & 1u)) {
            syscall_record_locks_close(task, file);
            clear_task_file(file);
        }
    }
    for (uint32_t i = 0; i < SCHED_TASK_STDIO_MAX; ++i) {
        if ((sched_task_fds(task)->stdio_files[i].used &&
             (sched_task_fds(task)->stdio_files[i].fd_flags & 1u)) ||
             (sched_task_fds(task)->cloexec_stdio_mask & (1u << i))) {
            syscall_record_locks_close(task, &sched_task_fds(task)->stdio_files[i]);
            clear_task_file(&sched_task_fds(task)->stdio_files[i]);
            sched_task_fds(task)->closed_stdio_mask |= 1u << i;
            sched_task_fds(task)->cloexec_stdio_mask &= ~(1u << i);
        }
    }
    for (uint32_t i = 0; i < sched_task_pty_capacity(task); ++i) {
        struct task_pty_fd *entry = sched_task_pty_at(task, i);
        if (entry->used && (entry->flags & 1u)) {
            int fd = entry->fd;
            task_pty_release_entry(entry);
            if (fd >= 0 && fd < 3) sched_task_fds(task)->closed_stdio_mask |= 1u << fd;
        }
    }
}

struct task_file *task_file_for_fd(struct task *task, int fd)
{
    return task_file_description(task_descriptor_for_fd(task, fd));
}

/**
 * @brief Resolve the stable OFD bound to the current syscall or descriptor.
 * @param task Current task; NULL returns NULL.
 * @param fd Linux descriptor number whose in-flight syscall pin may override lookup.
 * @return Borrowed stable file description, or NULL when the descriptor is not a file.
 * @note Caller serializes with descriptor mutation; the returned pointer is not retained.
 */
struct task_file *task_file_for_io(struct task *task, int fd)
{
    if (task && task->syscall_pty.used && task->syscall_fd == fd) return NULL;
    return task && task->syscall_file && task->syscall_fd == fd
        ? task->syscall_file : task_file_for_fd(task, fd);
}

/**
 * @brief Drop the private endpoint of an interrupted FIFO open.
 * @param task Task under the kernel execution lock; NULL is accepted.
 */
void task_fifo_cancel(struct task *task)
{
    if (!task || !task->fifo_open_file) return;
    struct task_file *file = task->fifo_open_file;
    task->fifo_open_file = NULL;
    task_pipe_release(file);
    (void)storage_inode_put(file->inode);
    kernel_free(file);
}

void task_release_syscall_file(struct task *task)
{
    if (!task) return;
    /* Signal handoff and syscall mismatch invalidate DRM ioctl snapshots here;
     * the vmwgfx hook only queues bounded task-context cleanup. */
    drm_vmwgfx_cancel_task_continuation(task);
    task_sysv_msg_cancel(task);
    task_sysv_sem_cancel(task);
    task_fifo_cancel(task);
    struct task_file *file = task->syscall_file;
    task->syscall_file = NULL;
    if (file && file->io_owner == task->pid) file->io_owner = 0;
    __builtin_memset(&task->regular_io, 0, sizeof(task->regular_io));
    task_pty_release_entry(&task->syscall_pty);
    task->signalfd_waiting = false;
    task->signalfd_wait_mask = 0;
    if (task->signalfd_vectors && task->signalfd_vectors != task->signalfd_fast_vectors)
        kernel_free(task->signalfd_vectors);
    task->signalfd_vectors = NULL;
    task->signalfd_vector_count = task->signalfd_read_flags = 0;
    task->signalfd_vector_bytes = 0;
    task->socket_io_deadline = 0;
    task->socket_io_timed = false;
    task->mmsg = (struct task_mmsg_state){0};
    if (file) task_file_put(file);
}

/**
 * @brief Read Linux descriptor flags (FD_CLOEXEC) for any open descriptor.
 *
 * Linux keeps FD_CLOEXEC in the per-descriptor table entry, not in the shared
 * open file description, so a dup()ed descriptor never observes the original's
 * flag.  ReliefOS splits descriptors across three backing tables: file entries,
 * explicit PTY endpoints and implicit stdin/stdout/stderr streams.  Implicit
 * streams have no task_file entry and store the flag in cloexec_stdio_mask.
 *
 * @param task Descriptor-table owner; the caller runs under the syscall
 *             execution lock so the tables cannot change concurrently.
 * @param fd Descriptor number; only the caller's low 32 bits are meaningful.
 * @return Non-negative flag bitmask (only RELIEFOS_FD_CLOEXEC is defined),
 *         or -RELIEFOS_EBADF when the descriptor is not open.
 */
int task_fd_descriptor_flags(struct task *task, int fd)
{
    struct task_file *descriptor;
    struct task_pty_fd *pty_fd;
    if (!task || fd < 0) return -RELIEFOS_EBADF;
    descriptor = task_descriptor_for_fd(task, fd);
    if (descriptor) return (int)(descriptor->fd_flags & RELIEFOS_FD_CLOEXEC);
    pty_fd = task_pty_fd_for_fd(task, fd);
    if (pty_fd) return (int)(pty_fd->flags & RELIEFOS_FD_CLOEXEC);
    /* Deferred stdio table growth: fd 0..2 exist implicitly until an explicit
     * close or redirection marks the slot closed. */
    if (fd < 3 &&
        !(sched_task_fds(task)->closed_stdio_mask & (1u << (uint32_t)fd))) {
        return (sched_task_fds(task)->cloexec_stdio_mask & (1u << (uint32_t)fd))
                   ? RELIEFOS_FD_CLOEXEC : 0;
    }
    return -RELIEFOS_EBADF;
}

/**
 * @brief Update Linux descriptor flags (FD_CLOEXEC) on exactly one descriptor.
 *
 * The flag is stored per descriptor, so only the addressed slot changes and
 * dup()ed descriptors that share the same open file description keep their own
 * state.  Implicit stdin/stdout/stderr descriptors record the flag in
 * cloexec_stdio_mask so a later execve() really closes them instead of
 * reporting success without saving anything.
 *
 * @param task Descriptor-table owner; the caller runs under the syscall
 *             execution lock so the tables cannot change concurrently.
 * @param fd Descriptor number; only the caller's low 32 bits are meaningful.
 * @param flags Replacement flag bitmask; every bit outside RELIEFOS_FD_CLOEXEC
 *              is ignored, matching Linux F_SETFD/do_vfs_ioctl().
 * @return Zero on success, or -RELIEFOS_EBADF when the descriptor is not open.
 */
int task_fd_set_descriptor_flags(struct task *task, int fd, uint32_t flags)
{
    struct task_file *descriptor;
    struct task_pty_fd *pty_fd;
    uint32_t value = flags & RELIEFOS_FD_CLOEXEC;
    if (!task || fd < 0) return -RELIEFOS_EBADF;
    descriptor = task_descriptor_for_fd(task, fd);
    if (descriptor) {
        descriptor->fd_flags = value;
        return 0;
    }
    pty_fd = task_pty_fd_for_fd(task, fd);
    if (pty_fd) {
        pty_fd->flags = value;
        return 0;
    }
    if (fd < 3 &&
        !(sched_task_fds(task)->closed_stdio_mask & (1u << (uint32_t)fd))) {
        if (value) sched_task_fds(task)->cloexec_stdio_mask |= 1u << (uint32_t)fd;
        else sched_task_fds(task)->cloexec_stdio_mask &= ~(1u << (uint32_t)fd);
        return 0;
    }
    return -RELIEFOS_EBADF;
}

/**
 * @brief Find the lowest free descriptor below RLIMIT_NOFILE across all backing tables.
 * @param task Descriptor-table owner.
 * @param minimum Inclusive lower bound.
 * @return Descriptor number or negative errno.
 */
int task_unused_fd(struct task *task, int minimum)
{
    if (!task || minimum < 0) return -RELIEFOS_EINVAL;
    for (uint64_t fd = (uint32_t)minimum; fd < sched_task_limits(task)->nofile.rlim_cur && fd < INT32_MAX; ++fd) {
        if (task_pty_fd_available(task, (int)fd)) return (int)fd;
    }
    return -RELIEFOS_EMFILE;
}

int task_can_allocate_fd(const struct task *task)
{
    return task_unused_fd((struct task *)task, 0) >= 0;
}

/**
 * @brief Reserve the lowest free file descriptor, growing backing storage if needed.
 * @param task Descriptor-table owner, serialized by the syscall execution lock.
 * @param minimum Inclusive lower bound.
 * @param slot Receives the initialized descriptor entry.
 * @return Descriptor number or negative errno without consuming a slot on failure.
 */
int task_allocate_fd(struct task *task, int minimum, struct task_file **slot)
{
    int fd = task_unused_fd(task, minimum);
    if (fd < 0) return fd;
    struct task_file *file = fd < 3 ? &sched_task_fds(task)->stdio_files[fd] :
        sched_task_file_at(task, (uint32_t)fd - 3);
    if (!file) return -RELIEFOS_ENOMEM;
    *file = (struct task_file){.used = 1};
    if (fd < 3) {
        sched_task_fds(task)->closed_stdio_mask &= ~(1u << fd);
        sched_task_fds(task)->cloexec_stdio_mask &= ~(1u << fd);
    }
    *slot = file;
    return fd;
}

/**
 * @brief Release a file descriptor after a failed multi-fd operation, including stdio slots.
 * @param task Descriptor-table owner.
 * @param fd Reserved file descriptor to roll back.
 */
void task_discard_file_fd(struct task *task, int fd)
{
    syscall_record_locks_close(task, task_descriptor_for_fd(task, fd));
    clear_task_file(task_descriptor_for_fd(task, fd));
    if (fd >= 0 && fd < 3) {
        sched_task_fds(task)->closed_stdio_mask |= 1u << fd;
        sched_task_fds(task)->cloexec_stdio_mask &= ~(1u << fd);
    }
}

/**
 * @brief Allocate an opened descriptor and retain its inode or console terminal.
 * @param task Descriptor-table owner under the execution transaction.
 * @param node Validated backing node, borrowed during initialization.
 * @param flags Validated status and internal device flags.
 * @param path Optional NUL-terminated device or file pathname.
 * @return Descriptor number or negative errno; failed console setup releases
 * the allocated slot and inode reference. O_PATH does not open a terminal.
 */
int alloc_task_fd(struct task *task, const struct storage_node *node, uint32_t flags, const char *path)
{
    if (!task || !node) return -RELIEFOS_EINVAL;
    struct task_file *file;
    int fd = task_allocate_fd(task, 0, &file);
    if (fd < 0) return fd;
    int ret = storage_inode_get(node, &file->inode);
    if (ret < 0) {
        *file = (struct task_file){0};
        return ret;
    }
    file->node = *node;
    file->flags = flags;
    file->fd_flags = (flags & RELIEFOS_O_CLOEXEC) ? RELIEFOS_FD_CLOEXEC : 0;
    copy_text(file->path, sizeof(file->path), path);
    if (task_device_is(file, STORAGE_DEV_KIND_CONSOLE) &&
        !(flags & TASK_FILE_FLAG_PATH)) {
        /* Resolve /dev/console at open time, not again after a VT switch. */
        uint32_t terminal = pty_vt_id(pty_vt_active());
        if (!terminal) {
            task_discard_file_fd(task, fd);
            return -RELIEFOS_ENODEV;
        }
        ret = pty_transfer_get(terminal, TASK_PTY_ENDPOINT_SLAVE);
        if (ret < 0) {
            task_discard_file_fd(task, fd);
            return ret;
        }
        file->aux = terminal;
    }
    return fd;
}

/**
 * @brief Replace a descriptor with a retained source description under the execution lock.
 * @param task Descriptor-table owner; required.
 * @param old_fd Existing source descriptor whose description remains shared.
 * @param new_fd Destination descriptor below RLIMIT_NOFILE.
 * @return Destination descriptor, or negative errno with the original target
 * preserved on allocation or retain failure. Replacement releases its former
 * description and record locks; a distinct destination clears FD_CLOEXEC.
 */
int task_dup2_fd(struct task *task, int old_fd, int new_fd)
{
    struct task_file *old_file = task_file_for_fd(task, old_fd);
    if (!task || !old_file || new_fd < 0) return -RELIEFOS_EBADF;
    if (old_fd == new_fd) return new_fd;
    if ((uint64_t)new_fd >= sched_task_limits(task)->nofile.rlim_cur) return -RELIEFOS_EBADF;
    /* Retain before table growth: an extra-table source can move when the
     * destination is expanded. Failure must also preserve the target. */
    struct task_file retained = {0};
    int ret = task_file_reference(&retained, old_file);
    if (ret < 0) return ret;
    struct task_file *new_file = new_fd < 3 ? &sched_task_fds(task)->stdio_files[new_fd] :
        sched_task_file_at(task, (uint32_t)new_fd - 3);
    if (!new_file) { clear_task_file(&retained); return -RELIEFOS_ENOMEM; }
    struct task_pty_fd *replaced_pty = task_pty_fd_for_fd(task, new_fd);
    if (new_file->used) {
        syscall_record_locks_close(task, new_file);
        clear_task_file(new_file);
    }
    if (replaced_pty) task_pty_release_entry(replaced_pty);
    *new_file = retained;
    if (new_fd < 3) {
        sched_task_fds(task)->closed_stdio_mask &= ~(1u << new_fd);
        /* dup2() clears FD_CLOEXEC on the new descriptor, including the
         * implicit stdio slot that has no task_file entry of its own. */
        sched_task_fds(task)->cloexec_stdio_mask &= ~(1u << new_fd);
    }
    return new_fd;
}

int task_duplicate_file_fd(struct task *task, int old_fd, int minimum_fd,
                                  uint32_t fd_flags)
{
    struct task_file *source = task_file_for_fd(task, old_fd);
    if (!source) return -RELIEFOS_EBADF;
    if (minimum_fd < 0 || (uint64_t)minimum_fd >= sched_task_limits(task)->nofile.rlim_cur) return -RELIEFOS_EINVAL;
    struct task_file retained = {0};
    int ret = task_file_reference(&retained, source);
    if (ret < 0) return ret;
    struct task_file *target;
    int fd = task_allocate_fd(task, minimum_fd, &target);
    if (fd < 0) { clear_task_file(&retained); return fd; }
    *target = retained;
    target->fd_flags = fd_flags;
    return fd;
}

/**
 * @brief Retain selected parent descriptors as a child's standard streams.
 * @param parent Source task; required when any requested descriptor is nonnegative.
 * @param child Destination task whose stdio aliases may be replaced.
 * @param stdin_fd Source for child fd 0, or a negative value to leave it unchanged.
 * @param stdout_fd Source for child fd 1, or a negative value to leave it unchanged.
 * @param stderr_fd Source for child fd 2, or a negative value to leave it unchanged.
 * @return Zero on success or a negative errno; failed PTY alias growth releases the
 * retained source reference without publishing a child descriptor.
 */
int syscall_inherit_task_fds(struct task *parent, struct task *child,
                             int stdin_fd, int stdout_fd, int stderr_fd)
{
    const int requested[3] = {stdin_fd, stdout_fd, stderr_fd};
    if (!child) return -RELIEFOS_EINVAL;
    for (int i = 0; i < 3; ++i) {
        struct task_file *source;
        struct task_file *target = &sched_task_fds(child)->stdio_files[i];
        if (requested[i] < 0) continue;
        source = task_file_for_fd(parent, requested[i]);
        if (parent && requested[i] < (int)SCHED_TASK_STDIO_MAX &&
            (sched_task_fds(parent)->closed_stdio_mask & (1u << (uint32_t)requested[i])) != 0) {
            sched_task_fds(child)->closed_stdio_mask |= 1u << (uint32_t)i;
            continue;
        }
        if (!source && (task_pty_fd_for_fd(parent, requested[i]) ||
                        task_pty_stream_for_fd(parent, requested[i]) >= 0)) {
            struct task_pty_fd *pty_source, *pty_target = task_pty_fd_for_fd(child, i);
            int ret = task_pty_ensure_fd(parent, requested[i], &pty_source);
            if (ret < 0) return ret;
            struct task_pty_fd retained = *pty_source;
            ++retained.description->references;
            if (!pty_target) {
                ret = sched_task_pty_alloc(child, &pty_target);
                if (ret < 0) {
                    task_pty_release_entry(&retained);
                    return ret;
                }
            }
            task_pty_release_entry(pty_target);
            clear_task_file(target);
            *pty_target = retained;
            pty_target->fd = i;
            pty_target->flags = 0;
            sched_task_fds(child)->closed_stdio_mask &= ~(1u << i);
            sched_task_fds(child)->cloexec_stdio_mask &= ~(1u << i);
            continue;
        }
        /* Console-only implicit streams use the child's default output. */
        if (!source && requested[i] <= 2) continue;
        if (!source) {
            return -RELIEFOS_EBADF;
        }
        if (target->used) clear_task_file(target);
        int ret = task_file_reference(target, source);
        if (ret < 0) return ret;
    }
    return 0;
}

/**
 * File can read.
 * @param file Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int file_can_read(const struct task_file *file)
{
    uint32_t acc = file ? (file->flags & RELIEFOS_O_ACCMODE) : RELIEFOS_O_RDONLY;
    return acc == RELIEFOS_O_RDONLY || acc == RELIEFOS_O_RDWR;
}

/**
 * File can write.
 * @param file Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int file_can_write(const struct task_file *file)
{
    uint32_t acc = file ? (file->flags & RELIEFOS_O_ACCMODE) : RELIEFOS_O_RDONLY;
    return acc == RELIEFOS_O_WRONLY || acc == RELIEFOS_O_RDWR;
}
