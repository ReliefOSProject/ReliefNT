/*
 * ReliefOS wait queue interface.
 * Provides the common waiter registration and wakeup primitive used by IPC,
 * device, and filesystem code.
 */
#ifndef RELIEFNT_WAIT_H
#define RELIEFNT_WAIT_H

#include <reliefnt/lock.h>
#include <reliefnt/types.h>

struct task;

/**
 * @brief Intrusive task wait list with an atomic conservative readiness hint.
 * @note The queue lock protects head/tail, each linked task's wait links, and
 *       count. A task may belong to at most one wait queue at a time. Queue
 *       storage must remain alive until all linked tasks are woken or removed.
 */
struct kernel_wait_queue {
    struct kernel_spinlock lock;
    struct task *head;
    struct task *tail;
    uint32_t count;
    /* Atomic hint: high word is count, low word is the minimum task threshold. */
    uint64_t fast_state;
};

/**
 * @brief Initialize an empty wait queue and its spinlock.
 * @param queue Queue storage owned by the caller.
 */
void kernel_wait_queue_init(struct kernel_wait_queue *queue);

/**
 * @brief Link a task into a queue, moving it from an earlier queue if needed.
 * @param queue Destination queue protected by its own spinlock.
 * @param task Task whose intrusive wait link is being published.
 * @return Zero on success, or -1 for invalid arguments or count overflow.
 * @note Queue storage must remain alive until this task is removed or woken.
 */
int kernel_wait_queue_add(struct kernel_wait_queue *queue, struct task *task);

/**
 * @brief Unlink a task if it is still registered on the supplied queue.
 * @param queue Queue that owns the task link.
 * @param task Task to detach; a task linked elsewhere is left unchanged.
 * @note Do not free queue storage until all linked tasks are detached.
 */
void kernel_wait_queue_remove(struct kernel_wait_queue *queue, struct task *task);

/** @brief Wake one eligible task, returning one when a task was woken. */
uint32_t kernel_wait_queue_wake_one(struct kernel_wait_queue *queue);

/** @brief Wake every task currently linked to the queue and return that count. */
uint32_t kernel_wait_queue_wake_all(struct kernel_wait_queue *queue);

/**
 * @brief Wake all linked tasks whose minimum resource requirement is available.
 * @param queue Queue whose task links are inspected under its lock.
 * @param available Resource amount currently available to waiters.
 * @return Number of tasks woken; stopped tasks remain stopped.
 */
uint32_t kernel_wait_queue_wake_satisfied(struct kernel_wait_queue *queue, uint32_t available);

/** @brief Read the generation of globally published pollable-readiness changes. */
uint64_t kernel_poll_readiness_generation(void);

/** @brief Publish a pollable-readiness change and wake all global poll waiters. */
void kernel_poll_readiness_notify(void);

/**
 * @brief Register and block the current task if readiness is unchanged.
 * @param generation Generation sampled before checking descriptors.
 * @param wake_tick One-tick fallback deadline, or zero for no fallback.
 * @return 1 if registered and blocked, 0 if readiness changed, -1 if the
 *         current task cannot wait or is linked to a different queue.
 */
int kernel_poll_wait_current(uint64_t generation, uint64_t wake_tick);

/**
 * @brief Remove a task's poll registration if one is still present.
 * @param task Task whose global poll link should be removed; null is ignored.
 */
void kernel_poll_wait_remove(struct task *task);

/**
 * @brief Report whether a task is linked to the global poll wait queue.
 * @param task Task to inspect; null is reported as unregistered.
 * @return True when the task's current queue is the global poll queue.
 */
bool kernel_poll_wait_registered(const struct task *task);

/**
 * @brief Register and block the current task on a queue without a threshold.
 * @param queue Queue protected by its own spinlock; null is ignored.
 * @note Queue storage must remain alive until the task is woken or removed.
 */
void kernel_wait_queue_block_current(struct kernel_wait_queue *queue);

/**
 * @brief Register and block the current task until a threshold is satisfied.
 * @param queue Queue protected by its own spinlock; null is ignored.
 * @param minimum Minimum available resource amount that permits wake_satisfied.
 * @note Queue storage must remain alive until the task is woken or removed.
 */
void kernel_wait_queue_block_current_minimum(struct kernel_wait_queue *queue, uint32_t minimum);

#endif
