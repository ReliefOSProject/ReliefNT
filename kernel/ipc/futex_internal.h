/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_FUTEX_INTERNAL_H
#define RELIEFNT_FUTEX_INTERNAL_H

#include <reliefnt/lock.h>
#include <reliefnt/sched.h>

/* The caller owns the outer execution scope. Queue/owner helpers require this lock. */
extern struct kernel_spinlock futex_lock;
extern struct task *futex_waiters;

/**
 * @brief Resolve page-in/COW before a futex operation acquires futex_lock.
 * @param address User range start. @param length Range length in bytes.
 * @param writing Whether the operation will modify the range.
 * @return Zero, a syscall contention marker, or negative EFAULT.
 */
int futex_prepare_user(uint64_t address, uint64_t length, bool writing);

/**
 * @brief Validate a futex word and derive its private-MM or shared-frame key.
 * @param task Current execution-owned task. @param address User word address.
 * @param operation Linux futex operation and flags.
 * @param domain Receives the private page-table root, or zero for shared words.
 * @param key Receives the user address or physical byte address.
 * @return Zero, a syscall contention marker, or negative errno.
 * Called without futex_lock. The caller retains the address-space lifetime.
 */
int futex_key(struct task *task, uint64_t address, uint32_t operation,
              uint64_t *domain, uint64_t *key);

/**
 * @brief Convert an optional Linux timespec into a scheduler tick deadline.
 * @param timeout User timespec address, or zero for no deadline.
 * @param operation Selects relative WAIT or absolute monotonic/realtime time.
 * @param out Receives zero or the rounded, saturating tick deadline.
 * @return Zero, a syscall contention marker, or negative errno.
 * User access occurs before futex_lock is acquired.
 */
int futex_deadline(uint64_t timeout, uint32_t operation, uint64_t *out);

/**
 * @brief Remove a waiter and release its PI dependency and retained shared page.
 * @param task Queued task retained by the caller.
 * Requires futex_lock. Completion state is owned by the calling operation.
 */
void futex_dequeue_locked(struct task *task);

/**
 * @brief Detect incompatible ordinary/PI waiters on an exact futex key.
 * @param domain Private-MM or shared domain. @param key Futex word identity.
 * @param pi Whether the proposed operation uses PI ownership.
 * @return True if the key already has an incompatible waiter.
 * Requires futex_lock.
 */
bool futex_queue_conflicts_locked(uint64_t domain, uint64_t key, bool pi);

/**
 * @brief Execute a PI futex operation for the current execution-owned task.
 * @param task Calling task. @param address User lock word.
 * @param operation LOCK_PI, LOCK_PI2, TRYLOCK_PI or UNLOCK_PI and flags.
 * @param timeout Optional absolute timespec for the blocking lock operations.
 * @return Zero, negative errno, or a syscall continuation marker.
 * Acquires futex_lock and retains a shared frame while its waiter is queued.
 */
int64_t futex_pi_syscall(struct task *task, uint64_t address,
                         uint32_t operation, uint64_t timeout);

/**
 * @brief Mark robust PI owner death and transfer ownership to a queued waiter.
 * @param task Exiting owner. @param address Original user lock word address.
 * @param word Writable resident alias retained by the caller.
 * Requires futex_lock and the owner's address-space lifetime.
 */
void futex_pi_robust_exit_locked(struct task *task, uint64_t address, uint32_t *word);

/**
 * @brief Finish remaining nonrobust PI waits before their owner is reclaimed.
 * @param task Exiting owner whose task slot remains valid during this call.
 * Requires futex_lock. Woken waiters receive negative ESRCH.
 */
void futex_pi_owner_exit_locked(struct task *task);

#endif
