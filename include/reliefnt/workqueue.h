/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_WORKQUEUE_H
#define RELIEFNT_WORKQUEUE_H
#include <reliefnt/types.h>
struct module;
struct kthread;
struct kernel_work;
typedef void (*kernel_work_fn)(struct kernel_work *, void *);

/* Core-private state after initialization. Static work/context must live until
 * cancellation completes; module work is core-allocated and ledger-owned. */
struct kernel_work {
    struct kernel_work *next;
    kernel_work_fn function;
    void *context;
    struct module *owner;
    struct kthread *running_thread;
    uint64_t deadline, issued, pending_sequence, running_sequence, completed, cancelled;
    uint32_t state, running_cpu;
    bool allocated;
};
#define KERNEL_WORK_INIT(fn, argument) { .function=(fn), .context=(argument) }

/** @brief Allocate work whose callback/context are retained by their owner.
 * @param function Required callback. @param context Borrowed stable argument.
 * @param owner COMING/LIVE module, or NULL for permanent code.
 * @param out Required owned handle output, NULL on failure.
 * @return Zero or EINVAL/ENOMEM/owner registration error. Task context, no
 * queue lock spans allocation. Module cleanup disables/cancels before drain;
 * module work storage survives explicit destroy until its ledger releases it.
 */
int kernel_work_create(kernel_work_fn function, void *context, struct module *owner, struct kernel_work **out);
/** @brief Coalesce an immediate generation without executing callbacks.
 * @param work Live static/core-allocated handle with stable context/owner.
 * @return True when queued, false if already pending, disabled or owner closed.
 * IRQ/task safe, allocation-free. A running callback may queue one successor;
 * each admitted generation pins module execution until completion/cancellation.
 */
bool kernel_queue_work(struct kernel_work *work);
/** @brief Coalesce work for a future monotonic tick without moving an existing deadline.
 * @param work Stable live handle. @param delay_ticks Delay from time_ticks().
 * @return True on enqueue, false on duplicate, overflow or closed admission.
 * IRQ/task safe; same lifetime/owner rules as kernel_queue_work.
 */
bool kernel_queue_delayed_work(struct kernel_work *work, uint64_t delay_ticks);
/** @brief Remove pending work and wait for an active callback to return.
 * @param work Stable handle retained by its caller through return.
 * @return Zero, EINVAL, EBUSY for another cancellation, EDEADLK for self-wait,
 * or EWOULDBLOCK outside waitable task context. Callers stop external producers
 * before freeing context; later unrelated enqueues remain allowed. Holds no
 * queue lock while waiting; suspends a depth-one execution owner if necessary.
 */
int kernel_work_cancel_sync(struct kernel_work *work);
/** @brief Complete the current queued/running generation, including a pending delay.
 * @param work Stable caller-retained handle.
 * @return Zero or EINVAL/EBUSY/EDEADLK/EWOULDBLOCK. Explicit task-context flush
 * may execute a pending generation immediately under execution ownership;
 * later requeues are outside its snapshot. Hold no callback-required locks.
 */
int kernel_work_flush(struct kernel_work *work);
/** @brief Disable/cancel an allocated handle before releasing its storage.
 * @param work Owned core-allocated handle. @return Zero or cancellation errno.
 * Task context; consumes permanent-code work on success. Module storage stays
 * ledger-owned until unload, with callback admission permanently disabled.
 */
int kernel_work_destroy(struct kernel_work *work);
/** @brief Check whether any queued deadline is due without executing work.
 * @return True for a due queue hint. Task/IRQ safe, allocation-free.
 */
bool kernel_workqueue_due(void);
/** @brief Notify scheduler CPUs once when a queued deadline becomes due.
 * IRQ/task safe, called after publishing timer ticks; no callback is entered.
 */
void kernel_workqueue_tick(void);
/** @brief Read the next runnable generation's absolute tick deadline.
 * @return Earliest deadline, or UINT64_MAX while no generation can run.
 * IRQ/task safe hint; running successors are excluded until completion.
 */
uint64_t kernel_workqueue_deadline(void);
/** @brief Allocate stopped per-CPU workers during serialized kernel startup.
 * @return Zero or allocation error; partial startup is completely unwound.
 * Boot/task context before user scheduling; permanent core code owns workers.
 */
int kernel_workqueue_start_workers(void);
/** @brief Wake one online consumer for an already-published ready deadline.
 * IRQ/task safe, allocation-free; rotates bound CPUs without broadcasting
 * lock contention. No provider callback is entered here.
 */
void kernel_workqueue_wake_workers(void);
/** @brief Execute ready generations on a retained worker stack.
 * @param budget Maximum callbacks. @param budget_us Budget between callbacks.
 * @return Completed callbacks. Requires depth-one execution ownership; never
 * runs callbacks under the queue lock. Callbacks retain existing subsystem
 * locks/wait scopes, must not access caller user operands, and may requeue.
 * The batch budget cannot preempt a writer-held callback; an unlocked wait
 * may be timer-preempted on its retained kernel-thread stack.
 */
uint32_t kernel_workqueue_run(uint32_t budget, uint64_t budget_us);
#endif
