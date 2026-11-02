/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"
#include <reliefnt/fsnotify.h>
#include <reliefnt/fsnotify_vfs.h>
#include <reliefnt/swap.h>
#include <linux/inotify.h>

/** @brief Publish a queue change to poll, epoll and blocking readers.
 * @param context Unused: readiness is published on the shared wait queue.
 * Called by the fsnotify core under execution ownership without reentering it.
 */
static void inotify_wake(void *context)
{
    (void)context;
    kernel_poll_readiness_notify();
}

/** @brief Create one anonymous inotify open-file description.
 * @param flags Linux IN_NONBLOCK/IN_CLOEXEC flags, already narrowed to 32 bits.
 * @return New descriptor, negative errno, or the per-real-UID instance quota.
 * The sole core reference moves into the descriptor; OFD aliases share it.
 */
int64_t syscall_inotify_init(uint32_t flags)
{
    struct task *task = sched_current_task();
    struct fsnotify_instance *instance = NULL;
    struct task_file *file = NULL;
    int fd, rc;
    if (flags & ~(IN_NONBLOCK | IN_CLOEXEC)) return -RELIEFOS_EINVAL;
    if (!task) return -RELIEFOS_ESRCH;
    rc = fsnotify_create(task->uid, inotify_wake, NULL, &instance);
    if (rc) return rc;
    fd = task_allocate_fd(task, 0, &file);
    if (fd < 0) { fsnotify_put(instance); return fd; }
    file->kind = TASK_FILE_KIND_INOTIFY;
    file->flags = flags & IN_NONBLOCK;
    file->fd_flags = (flags & IN_CLOEXEC) ? RELIEFOS_FD_CLOEXEC : 0;
    file->node.type = RELIEFOS_FS_TYPE_DEVICE;
    file->aux = (uint64_t)(uintptr_t)instance;
    return fd;
}

/** @brief Resolve a descriptor to its shared core, preserving Linux errors.
 * @param task Borrowed execution-owned caller. @param fd Native descriptor.
 * @param out Receives the borrowed instance on success.
 * @return Zero, EBADF for an invalid descriptor, EINVAL for a different kind.
 */
static int inotify_instance_for_fd(struct task *task, int fd,
                                   struct fsnotify_instance **out)
{
    struct task_file *file = task_file_for_io(task, fd);
    *out = NULL;
    if (!file) return -RELIEFOS_EBADF;
    if (file->kind != TASK_FILE_KIND_INOTIFY || !file->aux) return -RELIEFOS_EINVAL;
    *out = (struct fsnotify_instance *)(uintptr_t)file->aux;
    return 0;
}

/** @brief Add or replace a watch using a captured and DAC-checked VFS object.
 * @param fd Inotify descriptor pinned by syscall entry. @param pathname User path.
 * @param mask Linux event/behavior mask. @return Watch descriptor or negative errno.
 * Path resolution retains the original pathname and cwd through retry; the core
 * receives the live inode identity while the held path is still valid.
 */
int64_t syscall_inotify_add_watch(int fd, uint64_t pathname, uint32_t mask)
{
    struct task *task = sched_current_task();
    struct fsnotify_instance *instance;
    struct vfs_lookup_result found = {0};
    struct fsnotify_key key;
    int rc;
    if (!mask || (mask & ~FSNOTIFY_SUPPORTED_MASK) ||
        (mask & (IN_MASK_ADD | IN_MASK_CREATE)) == (IN_MASK_ADD | IN_MASK_CREATE))
        return -RELIEFOS_EINVAL;
    rc = inotify_instance_for_fd(task, fd, &instance);
    if (rc) return rc;
    rc = resolve_user_object_at_flags(vfs_system_namespace(), task, LINUX_AT_FDCWD,
        pathname, false, false, mask & IN_DONT_FOLLOW ? 0 : VFS_LOOKUP_FOLLOW, &found);
    if (rc) return rc;
    const struct storage_node *node = vfs_path_node(&found.path);
    if (!node) rc = -RELIEFOS_ENOENT;
    else if ((mask & IN_ONLYDIR) && node->type != RELIEFOS_FS_TYPE_DIR)
        rc = -RELIEFOS_ENOTDIR;
    if (!rc) rc = fs_permissions_check_path(task, &found.path, FS_ACCESS_READ, false);
    if (!rc) rc = vfs_notify_key ? vfs_notify_key(&found.path, &key) : -RELIEFOS_EOPNOTSUPP;
    if (!rc) rc = fsnotify_add_watch(instance, &key, mask);
    vfs_path_put(&found.path);
    return rc;
}

/** @brief Remove one watch, leaving its queued IN_IGNORED record readable.
 * @param fd Inotify descriptor. @param wd Watch identifier.
 * @return Zero or negative errno; caller owns the syscall execution writer.
 */
int64_t syscall_inotify_rm_watch(int fd, int wd)
{
    struct fsnotify_instance *instance;
    int rc = inotify_instance_for_fd(sched_current_task(), fd, &instance);
    return rc ? rc : fsnotify_remove_watch(instance, wd);
}

/** @brief Return the queue's poll-readable bits for a borrowed canonical OFD.
 * @param file Live inotify description. @return POLLIN/POLLRDNORM if nonempty.
 * The core queue and file remain execution-owned during this snapshot.
 */
short task_inotify_poll(const struct task_file *file)
{
    return file && file->aux &&
        fsnotify_ready((const struct fsnotify_instance *)(uintptr_t)file->aux)
        ? POLLIN | POLLRDNORM : 0;
}

/** @brief Copy the queued byte count to FIONREAD's checked integer operand.
 * @param task Current execution-owned caller. @param file Pinned inotify OFD.
 * @param output User pointer to an int. @return Zero, EFAULT or retry token.
 * The count is sampled only after user-page preparation, which may wait.
 */
int64_t task_inotify_fionread(struct task *task, const struct task_file *file,
                             uint64_t output)
{
    int rc = swap_prepare_user_range(task, output, sizeof(int), true);
    if (rc == SWAP_STORAGE_CONTENDED) return KERNEL_SYSCALL_SWAP_CONTENDED;
    if (rc < 0 || !user_range_writable(output, sizeof(int))) return -RELIEFOS_EFAULT;
    size_t bytes = fsnotify_pending_bytes((const struct fsnotify_instance *)(uintptr_t)file->aux);
    int value = bytes > INT32_MAX ? INT32_MAX : (int)bytes;
    __builtin_memcpy((void *)(uintptr_t)output, &value, sizeof(value));
    return 0;
}

/** @brief Read complete inotify records with Linux copy-fault semantics.
 * @param task Current caller. @param file Pinned canonical inotify OFD.
 * @param buffer User output. @param count Available bytes in this scalar/iovec.
 * @return Whole-record bytes, negative errno, or an interruptible retry token.
 * Each bounded page preparation can yield, so the head is re-peeked afterward.
 * Linux consumes the faulting event and returns EFAULT even after a successful
 * record. Private prerequisite contention instead preserves the head and
 * returns an accepted prefix, preventing a retry from duplicating records.
 */
int64_t task_inotify_read(struct task *task, struct task_file *file,
                          uint64_t buffer, uint64_t count)
{
    struct fsnotify_instance *instance = (struct fsnotify_instance *)(uintptr_t)file->aux;
    uint64_t total = 0;
    if ((int64_t)count < 0) return -RELIEFOS_EINVAL;
    if (count > 0x7ffff000u) count = 0x7ffff000u;
    if (count && (buffer >= RELIEFNT_USER_TOP || count > RELIEFNT_USER_TOP - buffer))
        return -RELIEFOS_EFAULT;
    kernel_poll_wait_remove(task);
    for (;;) {
        const struct inotify_event *event = NULL;
        uint64_t generation = kernel_poll_readiness_generation();
        size_t length = fsnotify_peek(instance, &event);
        if (!length) {
            if (total) return (int64_t)total;
            if (file->flags & RELIEFOS_O_NONBLOCK) return -RELIEFOS_EAGAIN;
            if (sched_task_pending(task) & ~task->blocked_signals) return -RELIEFOS_EINTR;
            if (kernel_poll_wait_current(generation, 0) >= 0) return KERNEL_SYSCALL_BLOCKED;
            return -RELIEFOS_EAGAIN;
        }
        if (length > count - total) return total ? (int64_t)total : -RELIEFOS_EINVAL;
        int prep = swap_prepare_user_range(task, buffer + total, length, true);
        if (prep == SWAP_STORAGE_CONTENDED)
            return total ? (int64_t)total : KERNEL_SYSCALL_SWAP_CONTENDED;
        if (prep < 0) {
            if (fsnotify_peek(instance, &event)) fsnotify_pop(instance);
            return -RELIEFOS_EFAULT;
        }
        size_t current = fsnotify_peek(instance, &event);
        /* Another reader may have consumed or replaced the head while the
         * page-in gate was dropped. Repeat before touching unprepared bytes. */
        if (!current || current > length) continue;
        if (!user_range_writable(buffer + total, current)) {
            fsnotify_pop(instance);
            return -RELIEFOS_EFAULT;
        }
        __builtin_memcpy((void *)(uintptr_t)(buffer + total), event, current);
        fsnotify_pop(instance);
        total += current;
    }
}
