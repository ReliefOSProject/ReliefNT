/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/swap.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/usercopy.h>
#include <linux/errno.h>

#define SECCOMP_PATH_MAX_INSNS 32768u
#define SECCOMP_PATH_LINK_COST 4u
#define SECCOMP_SUPPORTED_FLAGS (SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_LOG | \
                                 SECCOMP_FILTER_FLAG_TSYNC_ESRCH)

_Static_assert(sizeof(struct seccomp_data) == 64, "native seccomp_data ABI");
_Static_assert(offsetof(struct seccomp_data, arch) == 4 &&
               offsetof(struct seccomp_data, instruction_pointer) == 8 &&
               offsetof(struct seccomp_data, args) == 16, "native seccomp_data offsets");
_Static_assert(sizeof(struct sock_filter) == 8, "classic BPF instruction ABI");
_Static_assert(sizeof(struct sock_fprog) == 16 &&
               offsetof(struct sock_fprog, filter) == 8, "native sock_fprog ABI");

/** @brief Acquire another reference to an already pinned filter.
 * @param filter Existing published node, or NULL for an empty chain.
 */
static void filter_get(struct seccomp_filter *filter)
{
    if (filter) __atomic_add_fetch(&filter->refs, 1, __ATOMIC_RELAXED);
}

/** @brief Release a filter and its exclusive prefix without recursion.
 * @param filter Owned reference, or NULL. May free nodes from the kernel heap.
 */
static void filter_put(struct seccomp_filter *filter)
{
    while (filter && __atomic_sub_fetch(&filter->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        struct seccomp_filter *older = filter->older;
        kernel_free(filter);
        filter = older;
    }
}

/** @brief Copy the pinned parent's seccomp state into an unpublished child.
 * @param child Unpublished task without an owned filter reference.
 * @param parent Parent whose chain remains pinned throughout inheritance.
 */
void seccomp_inherit(struct task *child, const struct task *parent)
{
    child->seccomp_filter = parent->seccomp_filter;
    child->seccomp_count = child->seccomp_filter ? child->seccomp_filter->count : 0;
    filter_get(child->seccomp_filter);
}

/** @brief Detach the retiring task from its filter chain.
 * @param task Quiescent execution-owned task.
 */
void seccomp_release(struct task *task)
{
    struct seccomp_filter *filter = task->seccomp_filter;
    task->seccomp_filter = NULL;
    filter_put(filter);
}

/** @brief Copy a bounded native operand into private kernel storage.
 * @param task Current user task. @param dst Kernel destination.
 * @param src User virtual address. @param size Bytes (at most 32 KiB).
 * @return Zero, a private prerequisite retry, or EFAULT for an invalid range.
 * No scheduler or provider lock may span demand paging of the operand.
 */
static int seccomp_copyin(struct task *task, void *dst, uint64_t src, size_t size)
{
    int ret = swap_prepare_user_range(task, src, size, false);
    if (ret == SWAP_STORAGE_CONTENDED) return KERNEL_SYSCALL_SWAP_CONTENDED;
    if (ret < 0) return -LINUX_EFAULT;
#ifdef CONFIG_SWAP
    ret = user_range_readable(src, size);
    if (ret == -LINUX_EAGAIN) return KERNEL_SYSCALL_SWAP_CONTENDED;
    if (ret) return -LINUX_EFAULT;
#else
    if (!user_range_ok(src, size)) return -LINUX_EFAULT;
#endif
    __builtin_memcpy(dst, (const void *)(uintptr_t)src, size);
    return 0;
}

/** @brief Check ancestry using immutable node identity, not program contents.
 * @param ancestor Candidate older head; NULL is the empty common ancestor.
 * @param head Caller's pinned active chain. @return True if compatible.
 */
static bool filter_is_ancestor(const struct seccomp_filter *ancestor,
                               const struct seccomp_filter *head)
{
    if (!ancestor) return true;
    while (head && head->count > ancestor->count) head = head->older;
    return head == ancestor;
}

/** @brief Select a still-live sibling while the syscall execution writer is held.
 * @param caller Thread requesting the change. @param candidate Borrowed task slot.
 * @return True for a different thread of the same live thread group.
 */
static bool filter_sync_target(const struct task *caller, const struct task *candidate)
{
    return candidate != caller && candidate->pid && candidate->kind == TASK_KIND_USER &&
        candidate->state != TASK_EXITED && sched_task_tgid(candidate) == sched_task_tgid(caller);
}

/** @brief Validate all target threads before publishing a synchronized chain.
 * @param caller Execution-owned caller. @param flags Validated filter flags.
 * @return Zero, the first divergent TID, or -ESRCH when requested. No mutation.
 */
static int64_t filter_can_sync(struct task *caller, uint32_t flags)
{
    struct task *other;
    for (uint32_t i = 0; (other = sched_task_at(i)); ++i) {
        if (!filter_sync_target(caller, other) ||
            filter_is_ancestor(other->seccomp_filter, caller->seccomp_filter))
            continue;
        return flags & SECCOMP_FILTER_FLAG_TSYNC_ESRCH ? -LINUX_ESRCH :
            (int64_t)other->pid;
    }
    return 0;
}

/** @brief Attach a validated node, transferring ownership exactly once.
 * @param caller Current task under the exclusive execution gate.
 * @param next Unpublished node with a sole reference transferred on success.
 * @param flags Validated install flags.
 * @return Zero, -ENOMEM for the path limit, or the TSYNC error/TID. Failure
 * leaves all tasks and the caller-owned node unchanged.
 */
static int64_t filter_attach(struct task *caller, struct seccomp_filter *next,
                              uint32_t flags)
{
    struct seccomp_filter *old = caller->seccomp_filter;
    uint32_t path = next->length + (old ? old->path_insns + SECCOMP_PATH_LINK_COST : 0);
    if (path > SECCOMP_PATH_MAX_INSNS) return -LINUX_ENOMEM;
    if (flags & SECCOMP_FILTER_FLAG_TSYNC) {
        int64_t error = filter_can_sync(caller, flags);
        if (error) return error;
    }
    next->count = old ? old->count + 1 : 1;
    next->path_insns = path;
    next->log = (flags & SECCOMP_FILTER_FLAG_LOG) != 0;
    /* Move the caller's old head reference into next->older. */
    next->older = old;
    caller->seccomp_filter = next;
    caller->seccomp_count = next->count;
    if (flags & SECCOMP_FILTER_FLAG_TSYNC) {
        struct task *other;
        for (uint32_t i = 0; (other = sched_task_at(i)); ++i) {
            if (!filter_sync_target(caller, other)) continue;
            old = other->seccomp_filter;
            filter_get(next);
            other->seccomp_filter = next;
            other->seccomp_count = next->count;
            if (caller->no_new_privs) other->no_new_privs = true;
            filter_put(old);
        }
    }
    return 0;
}

/** @brief Allocate and validate a complete user filter before installing it.
 * @param task Current execution-owned caller. @param flags Validated flags.
 * @param argument User address of the native sock_fprog.
 * @return Zero, a TSYNC conflicting TID, Linux errno, or a private retry.
 * Nothing points to user memory after validation or publication.
 */
static int64_t filter_install(struct task *task, uint32_t flags, uint64_t argument)
{
    struct sock_fprog user;
    int error = seccomp_copyin(task, &user, argument, sizeof(user));
    if (error) return error;
    if (!user.len || user.len > BPF_MAXINSNS || !user.filter) return -LINUX_EINVAL;
    if (!task->no_new_privs && !(task->cap_effective & (UINT64_C(1) << CAP_SYS_ADMIN)))
        return -LINUX_EACCES;

    size_t bytes = user.len * sizeof(struct sock_filter);
    struct seccomp_filter *filter = kernel_malloc(sizeof(*filter) + bytes);
    uint16_t *paths = filter ? kernel_malloc(user.len * sizeof(*paths)) : NULL;
    if (!paths) {
        kernel_free(filter);
        return -LINUX_ENOMEM;
    }
    *filter = (struct seccomp_filter){.refs = 1, .length = user.len};
    error = seccomp_copyin(task, filter->code, (uintptr_t)user.filter, bytes);
    if (!error) error = seccomp_bpf_validate(filter->code, filter->length, paths);
    kernel_free(paths);
    int64_t result = error ? error : filter_attach(task, filter, flags);
    if (result) filter_put(filter);
    return result;
}

/** @brief Check an exact Linux return action without advertising unsupported hooks.
 * @param action Action value without SECCOMP_RET_DATA bits. @return Availability.
 */
static bool filter_action_available(uint32_t action)
{
    switch (action) {
    case SECCOMP_RET_KILL_PROCESS:
    case SECCOMP_RET_KILL_THREAD:
    case SECCOMP_RET_TRAP:
    case SECCOMP_RET_ERRNO:
    case SECCOMP_RET_LOG:
    case SECCOMP_RET_ALLOW:
        return true;
    default:
        return false;
    }
}

/** @brief Apply the filter-install or action-query operation of seccomp(2).
 * @param task Current user task under exclusive execution ownership, nullable.
 * @param operation Native selector. @param flags Native operation flags.
 * @param argument User address of a sock_fprog or u32 action.
 * @return Zero, positive conflicting TID, Linux errno, or an internal
 * prerequisite retry before publication. Unsupported operations fail.
 */
int64_t seccomp_syscall(struct task *task, uint32_t operation, uint32_t flags,
                        uint64_t argument)
{
    if (!task || task->kind != TASK_KIND_USER) return -LINUX_ESRCH;
    switch (operation) {
    case SECCOMP_SET_MODE_FILTER:
        if (flags & ~SECCOMP_SUPPORTED_FLAGS) return -LINUX_EINVAL;
        return filter_install(task, flags, argument);
    case SECCOMP_GET_ACTION_AVAIL: {
        uint32_t action;
        if (flags) return -LINUX_EINVAL;
        int result = seccomp_copyin(task, &action, argument, sizeof(action));
        if (result) return result;
        return filter_action_available(action) ? 0 : -LINUX_EOPNOTSUPP;
    }
    default:
        return -LINUX_EINVAL;
    }
}
