/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SECCOMP_H
#define RELIEFNT_SECCOMP_H

#include <reliefnt/task/types.h>
#include <linux/seccomp.h>

#ifdef CONFIG_SECCOMP_FILTER
#define RELIEFNT_SECCOMP_ACTIONS "kill_process kill_thread trap errno log allow"
/** @brief Test whether a user syscall must pass through the seccomp evaluator.
 * @param task Borrowed task under the execution gate, nullable.
 * @return True when the task has an installed filter chain.
 */
static inline bool seccomp_active(const struct task *task)
{
    return task && task->seccomp_filter;
}

/** @brief Implement the seccomp(2) operations of the native Linux ABI.
 * @param task Current user task under exclusive execution ownership.
 * @param operation Linux SECCOMP_* selector. @param flags Operation flags.
 * @param argument Operation-specific user pointer.
 * @return Zero, a positive conflicting TID, negative Linux errno, or a
 * private pre-usercopy retry result. Successful installs are irreversible.
 */
int64_t seccomp_syscall(struct task *task, uint32_t operation, uint32_t flags,
                        uint64_t argument);

/** @brief Retain the parent's chain before publishing a child.
 * @param child Unpublished task without an owned filter reference.
 * @param parent Borrowed parent whose task and chain are pinned.
 * No allocation, user access or live target replacement takes place.
 */
void seccomp_inherit(struct task *child, const struct task *parent);

/** @brief Drop a quiescent task's seccomp chain reference.
 * @param task Retiring task under exclusive execution ownership.
 */
void seccomp_release(struct task *task);

/** @brief Read the number of filters currently constraining a task.
 * @param task Borrowed task under execution ownership, nullable.
 * @return Installed chain length, or zero when unfiltered.
 */
static inline uint32_t seccomp_filter_count(const struct task *task)
{
    return task ? task->seccomp_count : 0;
}

/** @brief Evaluate the task's policy before any syscall backend sees the frame.
 * @param task Live filtered task under exclusive execution ownership.
 * @param frame Native syscall frame; denials may modify its return register.
 * @return True to dispatch, false to skip (possibly delivering SIGSYS or
 * retiring the task); filtered returns are terminal, not backend retry codes.
 */
bool seccomp_admit_syscall(struct task *task, struct trap_frame *frame);
#endif

#endif
