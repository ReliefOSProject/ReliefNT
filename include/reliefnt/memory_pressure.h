/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_MEMORY_PRESSURE_H
#define RELIEFNT_MEMORY_PRESSURE_H
#include <reliefnt/types.h>
struct task;

/* Scheduler-private reclaim state; no task or address-space ABI field changes.
 * Only scalar page-table identities survive an execution-writer transaction. */
struct pressure_cursor {
    uint32_t stage, task_index, pid, flags;
    uint64_t cr3, generation, page;
    uint64_t cache_start, fault_cr3, fault_page, fault_rip;
    bool swap_progress, cache_attempt;
};

/* Per-task optional headroom state. Only scalars and a bounded cursor cross
 * writer release; this is stored outside the public task/MM prefixes. A zero
 * pending-fault generation can promote only while the task still owns its root;
 * exec and teardown discard the token before that root can be retired. The
 * wait_owned bit identifies its current link on the shared pressure queue. */
struct pressure_headroom_state {
    struct pressure_cursor cursor;
    uint64_t cr3, generation, rip, page;
    uint64_t cache_epoch, swap_total, swap_free, wait_tick, wait_free;
    uint32_t waits, storage_waits;
    bool active, cooldown, fault_pending, wait_owned;
};

#define MEMORY_RECLAIM_ATOMIC 1u
#define MEMORY_RECLAIM_NOIO 2u
#define MEMORY_RECLAIM_NO_SELF_MM 4u

/** @brief Advance one bounded scheduler-owned background pressure batch.
 * @param budget_pages Maximum inspected candidates/completions, capped at 32.
 * The caller must establish a separately audited outer-writer context.
 */
void memory_pressure_poll(uint32_t budget_pages);
/** @brief Try one bounded reclaim batch for an ordinary page request.
 * @param pages Required free pages above the emergency reserve.
 * @param flags Explicit atomic, no-I/O and current-MM exclusions.
 * @return Zero when the caller may retry, -EAGAIN for deferred progress,
 * -ENOMEM when no victim exists or repeated snapshot changes prevent a safe
 * selection, or another terminal errno. No allocator lock spans this work.
 */
int memory_try_reclaim(uint32_t pages, uint32_t flags);
/** @brief Reclaim or publish a timed fault wait after an admitted allocation failure.
 * @param task Current live user task, borrowed under the sole outer writer.
 * @param address Faulting user byte address for progress accounting.
 * @return Zero for immediate retry, -EAGAIN after blocking, -ENOMEM if no
 * victim exists, repeated snapshot changes prevent safe selection, or an
 * ample-free scan finds no releasable frame, or another terminal errno.
 * Churn can fail even when a victim exists.
 * No reader, unrelated spin/loader lock, recursive writer or borrowed MM pointer
 * may cross the wait; a foreign storage operation is never cancelled.
 */
int memory_fault_oom(struct task *task, uint64_t address);
/** @brief Bound a fault retry while another accepted storage operation owns I/O.
 * @param task Current execution-owned user task. @param address Fault address.
 * @return -EAGAIN after timed blocking, or terminal negative errno. Does not
 * cancel the other owner's request or submit a replacement slot operation.
 */
int memory_fault_storage_wait(struct task *task, uint64_t address);
/** @brief Scope current-instruction page exclusions to one reclaim attempt.
 * @param task Current execution-owned task. @param rip Interrupted user RIP.
 * @param address Current fault byte address. No pointer survives scope end.
 */
void memory_reclaim_fault_scope_begin(struct task *task, uint64_t rip, uint64_t address);
/** @brief Clear the current fault's temporary reclaim exclusions before scheduling. */
void memory_reclaim_fault_scope_end(void);
#ifdef CONFIG_SWAP
/** @brief Advance one bounded, non-OOM batch for a failed swapoff recovery.
 * @param cursor Caller-owned scalar-only cursor, initially zero.
 * @param pages Target ordinary free pages. @param limit Maximum frames this
 * batch may release. @param released Required actual released-frame count.
 * @return Zero on target, -EAGAIN for bounded progress, -EBUSY for foreign
 * storage ownership, or terminal negative errno. The sole outer writer must
 * be held without a storage token or unrelated locks; no pointers are retained.
 */
int memory_reclaim_swapoff_step(struct pressure_cursor *cursor, uint64_t pages,
                                uint32_t limit, uint32_t *released);
/** @brief Check whether a user fault needs an outer-writer headroom decision.
 * @param task Current task protected by the execution reader or writer.
 * @return True below low or for an existing continuation/cooldown transition.
 * Reads scalar allocator and policy state only; no I/O or scheduling.
 */
bool memory_headroom_needed(struct task *task);
/** @brief Advance optional headroom for an admitted private-anonymous fault.
 * @param task Current user task under sole outer-writer ownership.
 * @param rip Interrupted instruction. @param address Admitted fault address.
 * @return -EAGAIN only after publishing a timed wait, otherwise zero to let
 * the original fault proceed. No OOM selection or borrowed pointer survives.
 */
int memory_headroom_step(struct task *task, uint64_t rip, uint64_t address);
/** @brief Cancel only this task's optional headroom and timed wait.
 * @param task Execution-owned task, nullable; ordinary fault state is separate.
 */
void memory_headroom_cancel(struct task *task);
/** @brief Retire old-MM headroom state at the infallible exec commit.
 * @param task Current execution-owned task, nullable. Unlinks only its owned
 * headroom/fault pressure wait, leaving any unrelated queue or pressure wait.
 * Caller holds no scheduler or unrelated spinlock; no allocation or I/O.
 */
void memory_headroom_exec_cancel(struct task *task);
#endif
/** @brief Cancel a task's own pending pressure wait and stall accounting.
 * @param task Borrowed execution-owned task, nullable; other queues are untouched.
 */
void memory_wait_cancel(struct task *task);
/** @brief Score a live task from owned RAM, swap and its OOM adjustment.
 * @param task User task and MM pinned by execution ownership, nullable.
 * @return Score 0..1000, or -ESRCH for an invalid or released task.
 * This standalone query synchronously walks the user page tables; it must not
 * be used inside the bounded reclaim selector or under a scheduler spinlock.
 */
int oom_score_for_task(struct task *task);
/** @brief Render the standard historical OOM score from owned RAM and slots.
 * @param task Live user task/MM pinned by execution ownership, nullable.
 * @return Score 0..2000, or -ESRCH for an invalid/released task. Synchronously
 * walks the page tables; never call inside the bounded selector or while a
 * scheduler spinlock is held. The internal selector policy remains unchanged.
 */
int oom_proc_score_for_task(struct task *task);
/** @brief Read an inherited OOM adjustment without allocating.
 * @param task Borrowed live user task, nullable, pinned by execution ownership.
 * @return Adjustment -1000..1000, or -ESRCH for an invalid task.
 */
int oom_adj_get(struct task *task);
/** @brief Set an adjustment and applicable same-MM/group policies.
 * @param task Target live user task pinned by the execution writer.
 * @param value Requested adjustment in -1000..1000.
 * @param caller Live capability-bearing caller, borrowed under the writer.
 * @return Zero or -EINVAL/-ESRCH/-EDEADLK/-EACCES; no ownership transfers.
 */
int oom_adj_set(struct task *task, int value, struct task *caller);
#endif
