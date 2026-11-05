/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SCHED_KERNEL_H
#define RELIEFNT_SCHED_KERNEL_H
#include <reliefnt/types.h>
#include <reliefnt/storage_context.h>
struct task;
struct kernel_sched_context {
    struct task *previous;
    uint64_t cr3,fs;
    bool kernel_mode,restore_fair;
    struct storage_io_context storage;
};
/** @brief Publish a kernel CPU owner while retaining the prior task reservation.
 * @param task Stable owned kernel task. @param context Caller-owned saved slot.
 * Depth-one execution writer, IRQs disabled; switches to kernel CR3, settles
 * accounting and pauses user fairness. No task/stack reference is released.
 */
void sched_kernel_context_enter(struct task *task,struct kernel_sched_context *context);
/** @brief Restore the reserved task/CR3 after a kernel context yields.
 * @param task Exact current kernel owner. @param context Matching saved slot.
 * Same writer/IRQ context as enter; settles kernel CPU time before publication.
 */
void sched_kernel_context_leave(struct task *task,struct kernel_sched_context *context);
/** @brief Retire a fully joined kernel task so its scheduler slot may recycle.
 * @param task Quiescent owned kernel task. Execution writer required; no live
 * stack or current CPU owner may still refer to it. Does not free stack memory.
 */
void sched_destroy_kernel_task(struct task *task);
/** @brief Publish a kernel thread's state against timer/proc scheduler readers.
 * @param task Owned kernel task. @param state Valid internal task-state value.
 * IRQ/task safe; scheduler lock only, no callback or execution-gate acquisition.
 */
void sched_kernel_task_state(struct task *task,unsigned state);
/** @brief Read the nondecreasing scheduler clock for a kernel/user time budget.
 * @return Microseconds in the existing fairness domain, extended with ACPI PM
 * time while a PIT interrupt is delayed. Execution-owned dispatcher context;
 * takes only scheduler_lock, without allocation, callbacks or a public clock change.
 */
uint64_t sched_kernel_clock_us(void);
#endif
