/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_STORAGE_WAIT_H
#define RELIEFNT_STORAGE_WAIT_H
#include <stdint.h>

/** @brief Begin an execution-owned syscall's storage completion observation.
 * @return Completion generation sampled before any provider condition check.
 * Removes this task's expired registration; resets only the caller's wait hint.
 * The kernel-owned wait queue outlives every loadable transport and task.
 */
uint64_t storage_wait_begin(void);
/** @brief Publish a pending interrupt-driven request's absolute timeout.
 * @param deadline Nonzero absolute tick deadline; caller holds execution ownership.
 * Does not sleep or retain any task/provider pointer; earliest hint wins.
 */
void storage_wait_hint(uint64_t deadline);
/** @brief Publish a hardware completion or retired storage ownership.
 * IRQ/task safe; wakes registered tasks without allocation or driver callbacks.
 */
void storage_completion_notify(void);
/** @brief Read the monotonic generation of completed storage operations.
 * @return Current completion/owner-release generation; safe from IRQ/task context.
 */
uint64_t storage_completion_generation(void);
/** @brief Park a retrying syscall only if no completion raced its condition check.
 * @param generation Snapshot returned by storage_wait_begin before the operation.
 * Execution-owned syscall epilogue only, after all transport locks unwind.
 * Uses the provider timeout hint, otherwise a one-tick polling fallback.
 */
void storage_wait_sleep(uint64_t generation);
#endif
