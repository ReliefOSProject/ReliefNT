/*
 * ReliefOS IPC syscall support: anonymous pipes and descriptor endpoints.
 */
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/sched.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/object.h>
#include <reliefnt/heap.h>
#include <reliefnt/wait.h>
#include <reliefnt/futex.h>
#include <reliefnt/sysv_shm.h>
#include <reliefos/fs_abi.h>

/* Linux's default is sixteen pages. Capacity is per pipe, not a global pool. */
#define TASK_PIPE_RING_CAP (TASK_PIPE_CAP + 1u)
#define TASK_PIPE_BUF 4096u
#define TASK_PIPE_PAGE 4096u
#define TASK_PIPE_USER_MAX (1024u * 1024u)
#define TASK_PIPE_USER_SOFT_PAGES 16384u

struct pipe_user {
    struct pipe_user *next;
    uint32_t uid;
    uint32_t pipes;
    uint64_t pages;
};

static struct pipe_user *pipe_users;

struct task_pipe {
    uint8_t used;
    uint8_t reserved[3];
    uint32_t readers;
    uint32_t writers;
    uint32_t reader_generation, writer_generation;
    uint32_t handle;
    bool named;
    struct storage_node node;
    uint32_t head;
    uint32_t tail;
    uint32_t capacity;
    uint8_t *data;
    struct task_pipe *next;
    struct pipe_user *owner;
    struct kernel_wait_queue wait_read;
    struct kernel_wait_queue wait_write;
};

static struct task_pipe *task_pipes;

/** @brief Check Linux pipe resource-limit privilege in the current credentials.
 * @return True for CAP_SYS_RESOURCE or CAP_SYS_ADMIN, false without a current task.
 */
static bool task_pipe_privileged(void)
{
    const struct task *task = sched_current_task();
    return task && (task->cap_effective & ((1ULL << CAP_SYS_RESOURCE) | (1ULL << CAP_SYS_ADMIN)));
}

/** @brief Allocate a stable pipe object, ring and per-UID accounting entry.
 * @param node Optional FIFO identity, NULL for an anonymous pipe.
 * @param error Negative error output on failure; must be writable.
 * @return Published object under execution ownership, or NULL with no resources retained.
 */
static struct task_pipe *task_pipe_create(const struct storage_node *node, int *error)
{
    const struct task *task = sched_current_task();
    uint32_t uid = task ? task->uid : 0;
    struct pipe_user *owner = pipe_users;
    while (owner && owner->uid != uid) owner = owner->next;
    bool new_owner = !owner;
    if (new_owner) {
        owner = kernel_malloc(sizeof(*owner));
        if (!owner) { *error = -RELIEFOS_ENOMEM; return NULL; }
        *owner = (struct pipe_user){.uid = uid};
    }
    uint32_t capacity = !task_pipe_privileged() && owner->pages >= TASK_PIPE_USER_SOFT_PAGES
                            ? 2u * TASK_PIPE_PAGE : TASK_PIPE_CAP;
    struct task_pipe *pipe = kernel_malloc(sizeof(*pipe));
    if (!pipe) { if (new_owner) kernel_free(owner); *error = -RELIEFOS_ENOMEM; return NULL; }
    *pipe = (struct task_pipe){.used = 1, .capacity = capacity, .owner = owner};
    pipe->data = kernel_malloc((size_t)capacity + 1u);
    if (!pipe->data) {
        kernel_free(pipe); if (new_owner) kernel_free(owner);
        *error = -RELIEFOS_ENOMEM; return NULL;
    }
    kernel_wait_queue_init(&pipe->wait_read);
    kernel_wait_queue_init(&pipe->wait_write);
    pipe->handle = kernel_object_insert(kernel_objects(), pipe, KERNEL_OBJECT_PIPE);
    if (!pipe->handle) {
        kernel_free(pipe->data); kernel_free(pipe); if (new_owner) kernel_free(owner);
        *error = -RELIEFOS_ENFILE; return NULL;
    }
    if (node) { pipe->named = true; pipe->node = *node; }
    if (new_owner) { owner->next = pipe_users; pipe_users = owner; }
    ++owner->pipes;
    owner->pages += capacity / TASK_PIPE_PAGE;
    pipe->next = task_pipes;
    task_pipes = pipe;
    return pipe;
}

/** @brief Unlink and free an unreferenced pipe and its ring under execution ownership.
 * @param pipe Live object with no read or write endpoint references.
 */
static void task_pipe_destroy(struct task_pipe *pipe)
{
    struct task_pipe **link = &task_pipes;
    while (*link && *link != pipe) link = &(*link)->next;
    if (*link) *link = pipe->next;
    (void)kernel_object_remove(kernel_objects(), pipe->handle, KERNEL_OBJECT_PIPE, NULL);
    if (pipe->owner) {
        struct pipe_user *owner = pipe->owner;
        owner->pages -= pipe->capacity / TASK_PIPE_PAGE;
        if (!--owner->pipes) {
            struct pipe_user **user = &pipe_users;
            while (*user && *user != owner) user = &(*user)->next;
            if (*user) *user = owner->next;
            kernel_free(owner);
        }
    }
    kernel_free(pipe->data);
    kernel_free(pipe);
}

static struct task_pipe *task_pipe_for_file(const struct task_file *file)
{
    if (!file || !(file->flags & TASK_FILE_FLAG_PIPE)) {
        return NULL;
    }
    return (struct task_pipe *)kernel_object_lookup(kernel_objects(), file->aux,
                                                    KERNEL_OBJECT_PIPE);
}

/** @brief Apply Linux pipe sizing while retaining byte order and per-UID accounting.
 * @param file Pinned pipe open-file description under execution ownership.
 * @param setting True to set capacity, false to query it.
 * @param bytes Requested unsigned int capacity; zero rounds up to one page.
 * @return Capacity or negative EBADF, EINVAL, EPERM, EBUSY, or ENOMEM. On
 * failure the ring, buffered bytes and accounting remain unchanged.
 */
int64_t task_pipe_capacity(struct task_file *file, bool setting, uint32_t bytes)
{
    struct task_pipe *pipe = task_pipe_for_file(file);
    if (!pipe) return -RELIEFOS_EBADF;
    if (!setting) return pipe->capacity;
    if (bytes > (1u << 30)) return -RELIEFOS_EINVAL;
    uint32_t capacity = TASK_PIPE_PAGE;
    while (capacity < bytes) capacity <<= 1;
    if (!task_pipe_privileged() && capacity > TASK_PIPE_USER_MAX) return -RELIEFOS_EPERM;
    uint32_t used = (pipe->head + pipe->capacity + 1u - pipe->tail) % (pipe->capacity + 1u);
    if (used > capacity) return -RELIEFOS_EBUSY;
    if (capacity == pipe->capacity) return capacity;
    if (capacity > pipe->capacity && pipe->owner && !task_pipe_privileged() &&
        pipe->owner->pages + (capacity - pipe->capacity) / TASK_PIPE_PAGE > TASK_PIPE_USER_SOFT_PAGES)
        return -RELIEFOS_EPERM;
    uint8_t *data = kernel_malloc((size_t)capacity + 1u);
    if (!data) return -RELIEFOS_ENOMEM;
    uint32_t first = pipe->capacity + 1u - pipe->tail;
    if (first > used) first = used;
    __builtin_memcpy(data, pipe->data + pipe->tail, first);
    if (used > first) __builtin_memcpy(data + first, pipe->data, used - first);
    uint8_t *old = pipe->data;
    if (pipe->owner)
        pipe->owner->pages = pipe->owner->pages - pipe->capacity / TASK_PIPE_PAGE + capacity / TASK_PIPE_PAGE;
    pipe->data = data;
    pipe->capacity = capacity;
    pipe->tail = 0;
    pipe->head = used;
    kernel_free(old);
    (void)kernel_wait_queue_wake_all(&pipe->wait_write);
    kernel_poll_readiness_notify();
    return capacity;
}

/**
 * @brief Acquire read/write endpoint counts for a newly owned description.
 * @param file Pipe description under the kernel execution lock.
 */
void task_pipe_retain(struct task_file *file)
{
    struct task_pipe *pipe = task_pipe_for_file(file);
    if (!pipe) return;
    if ((file->flags & RELIEFOS_O_ACCMODE) != RELIEFOS_O_RDONLY) {
        ++pipe->writers;
        ++pipe->writer_generation;
    }
    if ((file->flags & RELIEFOS_O_ACCMODE) != RELIEFOS_O_WRONLY) {
        ++pipe->readers;
        ++pipe->reader_generation;
    }
    kernel_poll_readiness_notify();
}

/**
 * @brief Release endpoint counts, wake peers and reclaim the final pipe object.
 * @param file Owned pipe description under the kernel execution lock.
 */
void task_pipe_release(struct task_file *file)
{
    struct task_pipe *pipe = task_pipe_for_file(file);
    if (!pipe) return;
    if ((file->flags & RELIEFOS_O_ACCMODE) != RELIEFOS_O_RDONLY) {
        if (pipe->writers) --pipe->writers;
        if (!pipe->writers) (void)kernel_wait_queue_wake_all(&pipe->wait_read);
    }
    if ((file->flags & RELIEFOS_O_ACCMODE) != RELIEFOS_O_WRONLY && pipe->readers) {
        --pipe->readers;
        if (!pipe->readers) (void)kernel_wait_queue_wake_all(&pipe->wait_write);
    }
    if (!pipe->readers && !pipe->writers) {
        (void)kernel_wait_queue_wake_all(&pipe->wait_read);
        (void)kernel_wait_queue_wake_all(&pipe->wait_write);
        task_pipe_destroy(pipe);
    }
    kernel_poll_readiness_notify();
}

/** @brief Allocate one descriptor and retain its pipe endpoint on success.
 * @param task Owning task under execution ownership.
 * @param pipe_handle Live generation-checked pipe handle.
 * @param write_end Nonzero for the write endpoint, zero for the read endpoint.
 * @return New descriptor or negative EINVAL/descriptor-allocation error.
 */
static int alloc_task_pipe_fd(struct task *task, uint32_t pipe_handle, int write_end)
{
    if (!task || !kernel_object_lookup(kernel_objects(), pipe_handle, KERNEL_OBJECT_PIPE))
        return -RELIEFOS_EINVAL;
    struct task_file *file;
    int fd = task_allocate_fd(task, 0, &file);
    if (fd < 0) return fd;
    file->flags = TASK_FILE_FLAG_PIPE | (write_end ? TASK_FILE_FLAG_PIPE_WRITE : 0) |
                  (write_end ? RELIEFOS_O_WRONLY : RELIEFOS_O_RDONLY);
    file->aux = pipe_handle;
    task_pipe_retain(file);
    return fd;
}

/**
 * @brief Read available pipe bytes, distinguishing EOF and a waiting writer.
 * @param file Pinned description under the kernel execution lock.
 * @param buffer Validated writable destination for length bytes.
 * @param length Maximum byte count.
 * @return Byte count, negative errno, or an interruptible retry.
 */
int task_pipe_read(struct task_file *file, void *buffer, uint32_t length)
{
    struct task_pipe *pipe = task_pipe_for_file(file);
    uint32_t count = 0;
    bool was_full;
    if (!pipe || (file->flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_WRONLY) return -RELIEFOS_EBADF;
    if (length == 0) return 0;
    was_full = (pipe->head + 1u) % (pipe->capacity + 1u) == pipe->tail;
    kernel_wait_queue_remove(&pipe->wait_read, sched_current_task());
    /* Copy contiguous runs per iteration: a 64 KiB ring drained byte-by-byte
     * costs more than the syscalls it serves. */
    while (pipe->tail != pipe->head && count < length) {
        uint32_t chunk = pipe->tail < pipe->head ? pipe->head - pipe->tail
                                                : (pipe->capacity + 1u) - pipe->tail;
        if (chunk > length - count) chunk = length - count;
        __builtin_memcpy((uint8_t *)buffer + count, pipe->data + pipe->tail, chunk);
        pipe->tail = (pipe->tail + chunk) % (pipe->capacity + 1u);
        count += chunk;
    }
    if (count) {
        uint32_t used = (pipe->head + (pipe->capacity + 1u) - pipe->tail) % (pipe->capacity + 1u);
        /* Retrying an atomic writer before its complete request fits only
         * switches to a task that immediately blocks again. Wake every
         * satisfiable writer so cancellation cannot strand available space. */
        (void)kernel_wait_queue_wake_satisfied(&pipe->wait_write, pipe->capacity - used);
        /* Only full -> nonfull adds POLLOUT readiness. Atomic writers can
         * require more space; their threshold wakeups above are independent. */
        if (was_full) kernel_poll_readiness_notify();
        return (int)count;
    }
    if (!pipe->writers) return 0;
    if (file->flags & RELIEFOS_O_NONBLOCK) return -RELIEFOS_EAGAIN;
    kernel_wait_queue_block_current(&pipe->wait_read);
    return -RELIEFOS_EAGAIN;
}

/**
 * @brief Write pipe data atomically up to PIPE_BUF and signal a missing reader.
 * @param file Pinned description under the kernel execution lock.
 * @param buffer Validated source for length bytes.
 * @param length Requested byte count.
 * @return Byte count or negative errno, including EPIPE and EAGAIN.
 */
int task_pipe_write(struct task_file *file, const void *buffer, uint32_t length)
{
    struct task_pipe *pipe = task_pipe_for_file(file);
    uint32_t count = 0;
    if (!pipe || (file->flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_RDONLY) return -RELIEFOS_EBADF;
    if (length == 0) return 0;
    if (!pipe->readers) {
        (void)sched_signal_user_task(sched_current_pid(), 13); /* SIGPIPE */
        return -RELIEFOS_EPIPE;
    }
    kernel_wait_queue_remove(&pipe->wait_write, sched_current_task());
    {
        uint32_t used = (pipe->head + (pipe->capacity + 1u) - pipe->tail) % (pipe->capacity + 1u);
        uint32_t free_bytes = pipe->capacity - used;
        /* Small nonblocking writes are all-or-nothing. Larger ones may
         * consume the available space, as Linux/POSIX require. Blocking
         * writes retain their existing full-request wait policy. */
        uint32_t minimum = (file->flags & RELIEFOS_O_NONBLOCK) && length > TASK_PIPE_BUF
                               ? 1u : length;
        if (length <= pipe->capacity && free_bytes < minimum) {
            if (file->flags & RELIEFOS_O_NONBLOCK) return -RELIEFOS_EAGAIN;
            kernel_wait_queue_block_current_minimum(&pipe->wait_write, length);
            return -RELIEFOS_EAGAIN;
        }
    }
    while (count < length) {
        uint32_t next = (pipe->head + 1U) % (pipe->capacity + 1u);
        if (next == pipe->tail) {
            if (count) {
                (void)kernel_wait_queue_wake_all(&pipe->wait_read);
                kernel_poll_readiness_notify();
                return (int)count;
            }
            if (file->flags & RELIEFOS_O_NONBLOCK) return -RELIEFOS_EAGAIN;
            kernel_wait_queue_block_current_minimum(&pipe->wait_write, 1);
            return -RELIEFOS_EAGAIN;
        }
        uint32_t used = (pipe->head + (pipe->capacity + 1u) - pipe->tail) % (pipe->capacity + 1u);
        uint32_t chunk = pipe->capacity - used;
        if (chunk > (pipe->capacity + 1u) - pipe->head) chunk = (pipe->capacity + 1u) - pipe->head;
        if (chunk > length - count) chunk = length - count;
        __builtin_memcpy(pipe->data + pipe->head, (const uint8_t *)buffer + count, chunk);
        pipe->head = (pipe->head + chunk) % (pipe->capacity + 1u);
        count += chunk;
    }
    (void)kernel_wait_queue_wake_all(&pipe->wait_read);
    kernel_poll_readiness_notify();
    return (int)count;
}

/**
 * @brief Report endpoint readiness and FIFO writer-generation HUP semantics.
 * @param file Pinned description under the kernel execution lock.
 * @param events Requested poll events.
 * @return Ready bits and unconditional HUP/ERR/NVAL notifications.
 */
short task_pipe_poll(const struct task_file *file, short events)
{
    const struct task_pipe *pipe = task_pipe_for_file(file);
    short result = 0;
    if (!pipe) {
        return POLLNVAL;
    }
    if ((file->flags & RELIEFOS_O_ACCMODE) != RELIEFOS_O_RDONLY) {
        if (pipe->readers == 0) {
            result |= POLLERR;
        } else if (events & POLLOUT) {
            uint32_t next = (pipe->head + 1U) % (pipe->capacity + 1u);
            if (next != pipe->tail) {
                result |= POLLOUT;
            }
        }
    }
    if ((file->flags & RELIEFOS_O_ACCMODE) != RELIEFOS_O_WRONLY) {
        if (pipe->tail != pipe->head) result |= events & POLLIN;
        if (pipe->writers == 0 && (!pipe->named || file->aux2 != pipe->writer_generation)) {
            result |= POLLHUP;
        }
    }
    return result;
}

/**
 * @brief Open or resume a FIFO using Linux partner-generation rendezvous.
 * @param task Current task with the execution lock held.
 * @param node FIFO identity, NULL only on retry of an existing open.
 * @param flags Linux open flags; O_PATH must be handled by the caller.
 * @param path Canonical path for descriptor diagnostics.
 * @return Published descriptor, negative errno, or KERNEL_SYSCALL_BLOCKED while waiting for a partner.
 */
int task_fifo_open(struct task *task, const struct storage_node *node,
                   uint32_t flags, const char *path)
{
    if (!task) return -RELIEFOS_ESRCH;
    struct task_file *file = task->fifo_open_file;
    struct task_pipe *pipe = file ? task_pipe_for_file(file) : NULL;
    if (!file) {
        if (!node) return -RELIEFOS_EINVAL;
        for (struct task_pipe *candidate = task_pipes; candidate; candidate = candidate->next) {
            if (candidate->named && candidate->node.volume_id == node->volume_id &&
                candidate->node.first_cluster == node->first_cluster && candidate->node.flags == node->flags) {
                pipe = candidate;
                break;
            }
        }
        uint32_t access = flags & RELIEFOS_O_ACCMODE;
        if (access == RELIEFOS_O_ACCMODE) return -RELIEFOS_EINVAL;
        if (access == RELIEFOS_O_WRONLY && (flags & RELIEFOS_O_NONBLOCK) && (!pipe || !pipe->readers))
            return -6; /* Linux ENXIO, without publishing any endpoint. */
        file = kernel_malloc(sizeof(*file));
        if (!file) return -RELIEFOS_ENOMEM;
        *file = (struct task_file){.used = 1, .node = *node, .flags = flags | TASK_FILE_FLAG_PIPE,
            .fd_flags = (flags & RELIEFOS_O_CLOEXEC) ? RELIEFOS_FD_CLOEXEC : 0};
        if (access != RELIEFOS_O_RDONLY) file->flags |= TASK_FILE_FLAG_PIPE_WRITE;
        int ret = storage_inode_get(node, &file->inode);
        if (ret < 0) { kernel_free(file); return ret; }
        if (!pipe) {
            pipe = task_pipe_create(node, &ret);
            if (!pipe) { (void)storage_inode_put(file->inode); kernel_free(file); return ret; }
        }
        file->aux = pipe->handle;
        /* A nonblocking reader suppresses HUP until a writer has existed. */
        file->aux2 = access == RELIEFOS_O_WRONLY ? pipe->reader_generation : pipe->writer_generation;
        if (access == RELIEFOS_O_RDONLY && pipe->writers) file->aux2 = pipe->writer_generation - 1u;
        if (path) {
            uint32_t i = 0;
            for (; path[i] && i + 1 < sizeof(file->path); ++i) file->path[i] = path[i];
            file->path[i] = 0;
        }
        task_pipe_retain(file);
        task->fifo_open_file = file;
        (void)kernel_wait_queue_wake_all(&pipe->wait_read);
    }
    uint32_t access = file->flags & RELIEFOS_O_ACCMODE;
    if (!(file->flags & RELIEFOS_O_NONBLOCK) &&
        ((access == RELIEFOS_O_RDONLY && !pipe->writers && file->aux2 == pipe->writer_generation) ||
         (access == RELIEFOS_O_WRONLY && !pipe->readers && file->aux2 == pipe->reader_generation))) {
        /* This is a pipe rendezvous, not pending disk I/O. The blocked
         * sentinel releases the storage transaction before scheduling. */
        kernel_wait_queue_block_current(&pipe->wait_read);
        return KERNEL_SYSCALL_BLOCKED;
    }
    kernel_wait_queue_remove(&pipe->wait_read, task);
    struct task_file *destination;
    int fd = task_allocate_fd(task, 0, &destination);
    if (fd < 0) { task_fifo_cancel(task); return fd; }
    *destination = *file;
    task->fifo_open_file = NULL;
    kernel_free(file);
    return fd;
}

/**
 * @brief Create two descriptors for a resource-backed anonymous pipe.
 * @param user_ptr Validated user array for two int descriptors.
 * @return Zero, negative descriptor/allocation error, or EFAULT; publishes
 * neither endpoint on failure. Caller holds execution ownership.
 */
int syscall_ipc_pipe(uint64_t user_ptr)
{
    struct task *task = sched_current_task();
    if (!task || !user_range_writable(user_ptr, sizeof(int) * 2u))
        return -RELIEFOS_EFAULT;
    int error = 0;
    struct task_pipe *pipe = task_pipe_create(NULL, &error);
    if (!pipe) return error;
    int read_fd = alloc_task_pipe_fd(task, pipe->handle, 0);
    if (read_fd < 0) { task_pipe_destroy(pipe); return read_fd; }
    int write_fd = alloc_task_pipe_fd(task, pipe->handle, 1);
    if (write_fd < 0) {
        /* Releasing the single endpoint also destroys the pipe. */
        task_discard_file_fd(task, read_fd);
        return write_fd;
    }
    ((int *)(uintptr_t)user_ptr)[0] = read_fd;
    ((int *)(uintptr_t)user_ptr)[1] = write_fd;
    return 0;
}

int syscall_ipc_pipe2(uint64_t user_ptr, uint64_t flags)
{
    flags = (uint32_t)flags;
    struct task *task = sched_current_task();
    int fds[2];
    struct task_file *read_file;
    struct task_file *write_file;
    if (flags & ~(uint32_t)(RELIEFOS_O_NONBLOCK | RELIEFOS_O_CLOEXEC)) {
        return -RELIEFOS_EINVAL;
    }
    if (!task || !user_range_writable(user_ptr, sizeof(fds))) return -RELIEFOS_EFAULT;
    int result = syscall_ipc_pipe(user_ptr);
    if (result < 0 || !task) return result;
    fds[0] = ((const int *)(uintptr_t)user_ptr)[0];
    fds[1] = ((const int *)(uintptr_t)user_ptr)[1];
    read_file = task_file_for_fd(task, fds[0]);
    write_file = task_file_for_fd(task, fds[1]);
    if (!read_file || !write_file) return -RELIEFOS_EBADF;
    if (flags & RELIEFOS_O_NONBLOCK) {
        read_file->flags |= RELIEFOS_O_NONBLOCK;
        write_file->flags |= RELIEFOS_O_NONBLOCK;
    }
    if (flags & RELIEFOS_O_CLOEXEC) {
        read_file->fd_flags |= RELIEFOS_FD_CLOEXEC;
        write_file->fd_flags |= RELIEFOS_FD_CLOEXEC;
    }
    return 0;
}

int syscall_ipc_owns(uint64_t number)
{
    switch (number) {
    case __NR_shmget:
    case __NR_shmat:
    case __NR_shmdt:
    case __NR_shmctl:
        return 1;
    case LINUX_SYS_MSGGET:
    case LINUX_SYS_SEMGET:
    case LINUX_SYS_SEMOP:
    case LINUX_SYS_SEMCTL:
    case LINUX_SYS_SEMTIMEDOP:
    case LINUX_SYS_MSGSND:
    case LINUX_SYS_MSGRCV:
    case LINUX_SYS_MSGCTL:
    case LINUX_SYS_PIPE:
    case LINUX_SYS_PIPE2:
    case LINUX_SYS_DUP:
    case LINUX_SYS_DUP2:
    case LINUX_SYS_FORK:
    case LINUX_SYS_VFORK:
    case LINUX_SYS_EXECVE:
    case LINUX_SYS_EXIT:
    case LINUX_SYS_WAIT4:
        return 1;
    default:
        return 0;
    }
}

int64_t syscall_ipc_dispatch(uint64_t number, uint64_t a0, uint64_t a1,
                             uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    if (number == __NR_shmget || number == __NR_shmat ||
        number == __NR_shmdt || number == __NR_shmctl)
        return syscall_sysv_shm(number, a0, a1, a2);
    if (number == LINUX_SYS_PIPE2) return syscall_ipc_pipe2(a0, a1);
    if ((number >= LINUX_SYS_SEMGET && number <= LINUX_SYS_SEMCTL) || number == LINUX_SYS_SEMTIMEDOP)
        return syscall_sysv_sem(number, a0, a1, a2, a3);
    if (number >= LINUX_SYS_MSGGET && number <= LINUX_SYS_MSGCTL)
        return syscall_sysv_msg(number, a0, a1, a2, a3, a4);
    return syscall_dispatch_regs_legacy(number, a0, a1, a2, a3, a4, a5);
}
