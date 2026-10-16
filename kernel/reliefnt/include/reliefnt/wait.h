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

#define KERNEL_WAIT_QUEUE_MAX 64u

struct kernel_wait_queue {
    struct kernel_spinlock lock;
    struct task *waiters[KERNEL_WAIT_QUEUE_MAX];
    uint32_t count;
};

void kernel_wait_queue_init(struct kernel_wait_queue *queue);
int kernel_wait_queue_add(struct kernel_wait_queue *queue, struct task *task);
void kernel_wait_queue_remove(struct kernel_wait_queue *queue, struct task *task);
uint32_t kernel_wait_queue_wake_one(struct kernel_wait_queue *queue);
uint32_t kernel_wait_queue_wake_all(struct kernel_wait_queue *queue);
/** @brief Read the generation of globally published pollable-readiness changes. */
uint64_t kernel_poll_readiness_generation(void);
/** @brief Publish a pollable-readiness change and wake all global poll waiters. */
void kernel_poll_readiness_notify(void);
/**
 * @brief Register and block the current task if readiness is unchanged.
 * @param generation Generation sampled before checking descriptors.
 * @param wake_tick One-tick fallback deadline, or zero for no fallback.
 * @return 1 if registered and blocked, 0 if readiness changed, -1 if the
 *         queue is full or the current task cannot wait.
 */
int kernel_poll_wait_current(uint64_t generation, uint64_t wake_tick);
/** @brief Remove a task's poll registration if one is still present. */
void kernel_poll_wait_remove(struct task *task);
/** @brief True when task remains registered on the global poll wait queue. */
bool kernel_poll_wait_registered(const struct task *task);
/**
 * @brief Register the current task on queue and block it without a deadline.
 *
 * The caller must have already determined that no data/space/pending event is
 * available under the global syscall execution lock. The task is left
 * registered until the next retry of the same syscall; callers remove it on
 * entry to avoid stale registrations after a timer wakeup.
 */
void kernel_wait_queue_block_current(struct kernel_wait_queue *queue);

#endif
