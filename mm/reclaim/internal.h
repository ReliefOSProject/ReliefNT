/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_RECLAIM_INTERNAL_H
#define RELIEFNT_RECLAIM_INTERNAL_H
#include <reliefnt/types.h>
struct task;
/** @brief Advance the serialized OOM selector or victim-retirement scan.
 * @param budget Maximum inspected slots or PTE steps, capped at 32.
 * @return -EAGAIN for further progress, -EINPROGRESS while a victim remains,
 * -ENOMEM when no victim exists or repeated snapshot changes prevent a safe
 * selection, or another terminal errno. Sole outer writer required; retained
 * PID identities, never task/MM pointers, cross batches.
 */
int memory_oom_advance(uint32_t budget);
#define PRESSURE_STORAGE_STALL_LIMIT 500u
#define PRESSURE_VICTIM_STALL_LIMIT 3000u
#ifdef CONFIG_SWAP
/** @brief Publish one optional refill wait; bounded separately from faults.
 * @param task Current admitted task. @return -EAGAIN if blocked, else errno.
 */
int memory_headroom_wait(struct task *task);
/** @brief Detach only this task's headroom/fault-owned pressure queue link.
 * @param task Execution-owned task, nullable. No cursor or stall reset; an
 * unrelated wait on the common pressure queue is not detached.
 */
void memory_headroom_unlink(struct task *task);
#endif
enum pressure_stall_cause { PRESSURE_STALL_NONE, PRESSURE_STALL_STORAGE, PRESSURE_STALL_VICTIM };
/** @brief Reclaim a real frame for a low-free fault, permitting OOM selection.
 * @param pages Required ordinary free pages after the failed attempt.
 * @return Zero only after an actual cache/swap frame release and the target,
 * -EAGAIN on bounded progress or contention, -ENOMEM after no-victim or
 * bounded churn, or another terminal errno. The sole outer writer is held;
 * each call charges at most 32 candidates/completions.
 */
int memory_try_reclaim_fault(uint32_t pages);
/** @brief Reclaim real frames without selecting an OOM victim.
 * @param pages Required ordinary free-page count, retained as 64-bit.
 * @return Zero only after an actual cache/swap frame release and the target,
 * -EAGAIN on bounded progress or contention, -ENOMEM after an unproductive
 * scan, or another terminal errno. Sole outer writer and no unrelated lock
 * are required; at most 32 candidates/completions are inspected per call.
 */
int memory_try_reclaim_no_oom(uint64_t pages);
/** @brief Read the last serialized foreground batch's deferred-stop reason.
 * @return NONE, STORAGE or VICTIM; caller holds the execution writer.
 */
enum pressure_stall_cause memory_reclaim_stall_cause(void);
/** @brief Wake one pressure waiter after observed free pages or retirement. */
void memory_pressure_notify(void);
#endif
