/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_FILE_LIMITS_H
#define RELIEFNT_FILE_LIMITS_H
#include <reliefnt/sched.h>
#include <reliefnt/signal.h>
#include <linux/errno.h>

/** @brief Reject an oversized user file operation and raise its Linux signal.
 * @param task Execution-owned caller, required. @param notify False after an
 * earlier vector element already accepted bytes; that invocation returns short.
 * @return Negative EFBIG. Does not change the inode, offset or resource limit.
 */
static inline int task_file_limit_exceeded(struct task *task, bool notify)
{
    if (notify) (void)kernel_signal_queue_task(task, LINUX_SIGXFSZ);
    return -EFBIG;
}

/** @brief Bound one regular-file write to the process-wide soft FSIZE limit.
 * @param task Execution-owned caller; NULL for an internal operation.
 * @param offset Resolved starting offset, including O_APPEND resolution.
 * @param count Required in/out byte count, shortened to the permitted prefix.
 * @param notify Raise SIGXFSZ when no bytes of this invocation were accepted.
 * @return Zero, or EFBIG with an optional signal when no prefix is writable.
 * Zero-byte writes never signal. No locks, allocation or ownership transfer.
 */
static inline int task_file_limit_write(struct task *task, uint64_t offset,
                                         uint64_t *count, bool notify)
{
    if (!task || !*count) return 0;
    uint64_t limit = sched_task_limits(task)->fsize.rlim_cur;
    if (offset >= limit) return task_file_limit_exceeded(task, notify);
    if (*count > limit - offset) *count = limit - offset;
    return 0;
}

/** @brief Check an explicit new logical size before truncate or file growth.
 * @param task Execution-owned caller; NULL for an internal operation.
 * @param size Requested nonnegative logical size.
 * @return Zero or EFBIG plus SIGXFSZ. Never shortens an explicit resize.
 */
static inline int task_file_limit_size(struct task *task, uint64_t size)
{
    return task && size > sched_task_limits(task)->fsize.rlim_cur
        ? task_file_limit_exceeded(task, true) : 0;
}
#endif
