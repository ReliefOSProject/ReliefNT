/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_PIDFD_H
#define RELIEFNT_PIDFD_H

#include <reliefnt/types.h>

struct task;
struct task_file;

/** @brief Open a close-on-exec descriptor retaining one stable PID object.
 * @param caller Calling task, borrowed under execution ownership.
 * @param pid Positive process or thread ID. @param flags Linux pidfd_open flags.
 * @return New file descriptor or negative Linux errno.
 */
int64_t syscall_pidfd_open(struct task *caller, int32_t pid, uint32_t flags);
/** @brief Recheck authorization and signal the identity pinned by an OFD.
 * @param caller Calling task under execution ownership. @param fd Its pidfd.
 * @param sig Linux signal number or zero for an existence/permission check.
 * @param pointer Optional user siginfo address. @param flags Linux signal scope.
 * @return Zero, negative Linux errno, or a private pre-enqueue retry result.
 */
int64_t syscall_pidfd_send_signal(struct task *caller, int fd, int sig,
                                 uint64_t pointer, uint32_t flags);
/** @brief Observe or consume a child wait event using a pinned PID identity.
 * @param caller Calling task under execution ownership. @param fd Its pidfd.
 * @param infop Optional user siginfo address. @param usage Optional raw ABI rusage address.
 * @param options Linux waitid event selection and flags.
 * @return Zero, negative Linux errno, or a private blocking/retry result.
 */
int64_t syscall_pidfd_waitid(struct task *caller, int fd, uint64_t infop, uint64_t usage,
                             uint32_t options);

/** @brief Report exit/reap readiness without consuming the child event.
 * @param file Borrowed pidfd description under execution ownership.
 * @return Linux poll event mask, zero while the observed target is live.
 */
short task_pidfd_poll(const struct task_file *file);

#endif
