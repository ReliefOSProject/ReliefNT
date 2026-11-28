/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_CGROUP_H
#define RELIEFNT_CGROUP_H
#include <reliefnt/types.h>

struct task;
struct text_stream;
struct vfs_mount;

/** @brief Open a detached view of the single cgroup v2 hierarchy.
 * @param source Optional copied mount label. @param flags Attachment policy.
 * @param options Optional compatibility options. @param out Owned mount on success.
 * @return Zero or negative errno with empty output. Execution writer required. */
int cgroup2_create_vfs_mount(const char *source, uint64_t flags, const char *options,
                            struct vfs_mount **out);

/** @brief Deliver a bounded batch of deferred notifications and retire dead groups.
 * @param budget Maximum queued groups to process. Execution writer required. */
void cgroup2_service(unsigned budget);

/* The standalone scheduler/proc fixtures can omit the facility. Production
 * always links it through the mount and deferred-work entry points. */
/** @brief Check a prospective child against the parent's ancestor limits.
 * @param parent Borrowed user parent or NULL for root. Caller holds scheduler_lock.
 * @return True if allowed; false records a rejection without charging the child. */
bool cgroup2_fork_allowed(const struct task *parent) __attribute__((weak));
/** @brief Charge one successfully published user task to the inherited group.
 * @param child Fresh task with no charge. @param parent Borrowed parent or NULL.
 * Requires scheduler_lock and a successful admission in the same transaction. */
void cgroup2_fork_commit(struct task *child, const struct task *parent) __attribute__((weak));
/** @brief Remove a first-time exit from the population, retaining waitable charges.
 * @param task Borrowed task immediately before exit under scheduler_lock. */
void cgroup2_task_exit(struct task *task) __attribute__((weak));
/** @brief Release a dead task's charge and group pin once, under scheduler_lock.
 * @param task Borrowed exited task that is no longer waitable. */
void cgroup2_task_reap(struct task *task) __attribute__((weak));
/** @brief Request a worker turn if changes are pending; never under scheduler_lock. */
void cgroup2_kick(void) __attribute__((weak));
/** @brief Write a task's unified membership line, including deleted ancestry.
 * @param task Borrowed published task under execution ownership.
 * @param stream Caller-owned positional text stream. Takes scheduler_lock internally. */
void cgroup2_proc_emit(const struct task *task, struct text_stream *stream) __attribute__((weak));
#endif
