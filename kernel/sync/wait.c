/*
 * ReliefOS wait queue implementation.
 * IRQ wakeups only walk task-owned intrusive links and never allocate memory.
 */
#include <reliefnt/sched.h>
#include <reliefnt/wait.h>

/* Poll/select/epoll share one conservative readiness queue. The generation
 * closes the check-to-sleep race for publishers that cannot share the syscall
 * execution transaction (notably input IRQ publication). */
static struct kernel_wait_queue poll_waiters = {
    .lock = KERNEL_SPINLOCK_INIT,
};
static uint64_t poll_readiness_generation;

/**
 * @brief Register a task with a minimum resource threshold.
 * @param queue Queue receiving the task link.
 * @param task Task to link; it may already belong to another queue.
 * @param minimum Minimum available resource amount needed for a satisfied wake.
 * @param block_current Publish the current task's blocked state before unlock.
 * @return Zero on success, or -1 for invalid input, conflicting registration,
 *         or count overflow.
 */
static int kernel_wait_queue_add_minimum(struct kernel_wait_queue *queue,
                                         struct task *task, uint32_t minimum,
                                         bool block_current);

/**
 * @brief Read the conservative fast hint without taking the queue lock.
 * @param queue Queue whose hint is published with release ordering.
 * @return Zero for a known-empty queue, otherwise its encoded count/minimum.
 */
static uint64_t kernel_wait_queue_fast_state(const struct kernel_wait_queue *queue)
{
    return queue ? __atomic_load_n(&queue->fast_state, __ATOMIC_ACQUIRE) : 0;
}

/**
 * @brief Recompute and publish the queue's count and minimum threshold hint.
 * @param queue Queue whose intrusive links are stable under its lock.
 * @note The high word stores task count and the low word stores the smallest
 *       wait_minimum. A zero value is reserved for an empty queue.
 */
static void kernel_wait_queue_publish_fast_state(struct kernel_wait_queue *queue)
{
    uint64_t state = 0;
    if (queue->count) {
        uint32_t minimum = UINT32_MAX;
        for (struct task *task = queue->head; task; task = task->wait_next) {
            if (task->wait_minimum < minimum) minimum = task->wait_minimum;
        }
        state = ((uint64_t)queue->count << 32) | minimum;
    }
    __atomic_store_n(&queue->fast_state, state, __ATOMIC_RELEASE);
}

/**
 * @brief Publish a conservative nonempty hint before linking a task.
 * @param queue Queue being modified under its lock.
 * @note Lock-free readers that see this hint recheck the canonical list while
 *       locked. A publisher that read the old empty hint before this store must
 *       retain its condition/generation recheck around registration and sleep.
 */
static void kernel_wait_queue_publish_busy_fast_state(struct kernel_wait_queue *queue)
{
    __atomic_store_n(&queue->fast_state, (uint64_t)1u << 32, __ATOMIC_RELEASE);
}

/**
 * @brief Link a task at the queue tail while the queue lock is held.
 * @param queue Queue that owns the intrusive list.
 * @param task Unlinked task to append.
 * @param minimum Minimum resource amount that permits a satisfied wake.
 */
static void kernel_wait_queue_link_locked(struct kernel_wait_queue *queue,
                                          struct task *task, uint32_t minimum)
{
    task->wait_prev = queue->tail;
    task->wait_next = NULL;
    task->wait_minimum = minimum;
    task->waiting_queue = queue;
    if (queue->tail) queue->tail->wait_next = task;
    else queue->head = task;
    queue->tail = task;
    ++queue->count;
}

/**
 * @brief Detach a task's intrusive link while its owning queue is locked.
 * @param queue Queue whose list contains the task.
 * @param task Candidate task to unlink.
 * @return True when the task was linked to queue and removed.
 */
static bool kernel_wait_queue_unlink_locked(struct kernel_wait_queue *queue,
                                            struct task *task)
{
    if (task->waiting_queue != queue) return false;
    if (task->wait_prev) task->wait_prev->wait_next = task->wait_next;
    else queue->head = task->wait_next;
    if (task->wait_next) task->wait_next->wait_prev = task->wait_prev;
    else queue->tail = task->wait_prev;
    task->waiting_queue = NULL;
    task->wait_next = NULL;
    task->wait_prev = NULL;
    task->wait_minimum = 0;
    if (queue->count) --queue->count;
    return true;
}

/**
 * @brief Initialize an empty wait queue and its spinlock.
 * @param queue Queue storage owned by the caller; null is ignored.
 */
void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{
    if (!queue) return;
    kernel_spin_init(&queue->lock);
    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;
    __atomic_store_n(&queue->fast_state, 0, __ATOMIC_RELEASE);
}

/**
 * @brief Register a task without a resource threshold.
 * @param queue Queue receiving the task link.
 * @param task Task to link; it may already belong to another queue.
 * @return Zero on success, or -1 for invalid input or count overflow.
 */
int kernel_wait_queue_add(struct kernel_wait_queue *queue, struct task *task)
{
    return kernel_wait_queue_add_minimum(queue, task, 0, false);
}

/**
 * @brief Register a task with its minimum resource threshold.
 * @param queue Queue receiving the task link.
 * @param task Task to link; it may already belong to another queue.
 * @param minimum Minimum available resource amount needed for a satisfied wake.
 * @param block_current True only when task is current; atomically publish sleep.
 * @return Zero on success, or -1 for invalid input, conflicting registration,
 *         or count overflow.
 */
static int kernel_wait_queue_add_minimum(struct kernel_wait_queue *queue,
                                         struct task *task, uint32_t minimum,
                                         bool block_current)
{
    uint64_t flags;
    if (!queue || !task) return -1;
    if (task->waiting_queue && task->waiting_queue != queue)
        kernel_wait_queue_remove(task->waiting_queue, task);

    kernel_spin_lock_irqsave(&queue->lock, &flags);
    if (task->waiting_queue == queue) {
        kernel_wait_queue_publish_busy_fast_state(queue);
        task->wait_minimum = minimum;
        kernel_wait_queue_publish_fast_state(queue);
        if (block_current) sched_block_current();
        kernel_spin_unlock_irqrestore(&queue->lock, flags);
        return 0;
    }
    if (task->waiting_queue || queue->count == UINT32_MAX) {
        kernel_spin_unlock_irqrestore(&queue->lock, flags);
        return -1;
    }
    kernel_wait_queue_publish_busy_fast_state(queue);
    kernel_wait_queue_link_locked(queue, task, minimum);
    kernel_wait_queue_publish_fast_state(queue);
    /* Wakers take this same queue lock before consulting scheduler state.
     * Publishing TASK_BLOCKED here prevents wake-before-sleep from detaching
     * a still-running task and then leaving it blocked without a queue link. */
    if (block_current) sched_block_current();
    kernel_spin_unlock_irqrestore(&queue->lock, flags);
    return 0;
}

/**
 * @brief Remove a task's link when it still belongs to the supplied queue.
 * @param queue Queue expected to own task; null is ignored.
 * @param task Task to detach; null or a different queue is ignored.
 */
void kernel_wait_queue_remove(struct kernel_wait_queue *queue, struct task *task)
{
    uint64_t flags;
    if (!queue || !task || !kernel_wait_queue_fast_state(queue)) return;
    kernel_spin_lock_irqsave(&queue->lock, &flags);
    (void)kernel_wait_queue_unlink_locked(queue, task);
    kernel_wait_queue_publish_fast_state(queue);
    kernel_spin_unlock_irqrestore(&queue->lock, flags);
}

/**
 * @brief Wake the first task whose threshold is satisfied.
 * @param queue Queue whose task links are inspected under its lock.
 * @param available Resource amount available; UINT32_MAX wakes any waiter.
 * @return One when a task was detached and passed to the scheduler, else zero.
 */
static uint32_t kernel_wait_queue_wake_one_raw(struct kernel_wait_queue *queue,
                                               uint32_t available)
{
    uint64_t flags;
    struct task *task;
    uint64_t state;
    if (!queue) return 0;
    state = kernel_wait_queue_fast_state(queue);
    if (!state || (available != UINT32_MAX && (uint32_t)state > available))
        return 0;

    kernel_spin_lock_irqsave(&queue->lock, &flags);
    for (task = queue->head; task; task = task->wait_next) {
        if (task->wait_minimum > available) continue;
        /* Keep the task linked while the scheduler uses it. Exit cleanup takes
         * this queue lock before releasing task storage. */
        sched_wake_interruptible(task);
        (void)kernel_wait_queue_unlink_locked(queue, task);
        break;
    }
    kernel_wait_queue_publish_fast_state(queue);
    kernel_spin_unlock_irqrestore(&queue->lock, flags);
    return task ? 1u : 0u;
}

/**
 * @brief Detach and wake every task currently linked to the queue.
 * @param queue Queue whose waiters are woken; null has no waiters.
 * @return Number of tasks detached and passed to the scheduler.
 */
static uint32_t kernel_wait_queue_wake_all_raw(struct kernel_wait_queue *queue)
{
    uint32_t count = 0;
    while (kernel_wait_queue_wake_one_raw(queue, UINT32_MAX)) ++count;
    return count;
}

/**
 * @brief Wake one linked task regardless of its minimum resource threshold.
 * @param queue Queue whose first waiter is woken.
 * @return One when a waiter was woken, otherwise zero.
 */
uint32_t kernel_wait_queue_wake_one(struct kernel_wait_queue *queue)
{
    return kernel_wait_queue_wake_one_raw(queue, UINT32_MAX);
}

/**
 * @brief Wake and detach every linked task.
 * @param queue Queue whose tasks are woken; null has no waiters.
 * @return Number of tasks detached and passed to the scheduler.
 */
uint32_t kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{
    return kernel_wait_queue_wake_all_raw(queue);
}

/**
 * @brief Wake tasks whose minimum resource threshold is now available.
 * @param queue Queue whose eligible task links are removed.
 * @param available Resource amount currently available.
 * @return Number of tasks woken; stopped tasks remain stopped.
 */
uint32_t kernel_wait_queue_wake_satisfied(struct kernel_wait_queue *queue,
                                          uint32_t available)
{
    uint32_t count = 0;
    while (kernel_wait_queue_wake_one_raw(queue, available)) ++count;
    return count;
}

/**
 * @brief Read the generation of globally published pollable-readiness changes.
 * @return Current generation with acquire ordering.
 */
uint64_t kernel_poll_readiness_generation(void)
{
    return __atomic_load_n(&poll_readiness_generation, __ATOMIC_ACQUIRE);
}

/**
 * @brief Publish readiness and wake every task registered on the poll queue.
 * @note The queue lock pins task storage while the scheduler wake runs. The
 *       generation changes before locking so scanners can avoid missed wakeups.
 */
void kernel_poll_readiness_notify(void)
{
    uint32_t count;
    uint64_t flags;
    __atomic_add_fetch(&poll_readiness_generation, 1, __ATOMIC_SEQ_CST);
    kernel_spin_lock_irqsave(&poll_waiters.lock, &flags);
    count = poll_waiters.count;
    while (count-- && poll_waiters.head) {
        struct task *task = poll_waiters.head;
        sched_wake_interruptible(task);
        (void)kernel_wait_queue_unlink_locked(&poll_waiters, task);
    }
    kernel_wait_queue_publish_fast_state(&poll_waiters);
    kernel_spin_unlock_irqrestore(&poll_waiters.lock, flags);
}

/**
 * @brief Register and block the current task if poll readiness is unchanged.
 * @param generation Generation sampled before scanning descriptors.
 * @param wake_tick One-tick fallback deadline, or zero for no fallback.
 * @return One when the task was registered and blocked, zero when the
 *         generation changed, or -1 when the task cannot wait.
 */
int kernel_poll_wait_current(uint64_t generation, uint64_t wake_tick)
{
    struct task *task = sched_current_task();
    uint64_t flags;
    if (!task || !task->pid || task->state == TASK_EXITED) return -1;

    kernel_spin_lock_irqsave(&poll_waiters.lock, &flags);
    if (__atomic_load_n(&poll_readiness_generation, __ATOMIC_ACQUIRE) != generation) {
        kernel_spin_unlock_irqrestore(&poll_waiters.lock, flags);
        return 0;
    }
    if (task->waiting_queue && task->waiting_queue != &poll_waiters) {
        kernel_spin_unlock_irqrestore(&poll_waiters.lock, flags);
        return -1;
    }
    if (!task->waiting_queue) {
        if (poll_waiters.count == UINT32_MAX) {
            kernel_spin_unlock_irqrestore(&poll_waiters.lock, flags);
            return -1;
        }
        kernel_wait_queue_publish_busy_fast_state(&poll_waiters);
        kernel_wait_queue_link_locked(&poll_waiters, task, 0);
    }
    kernel_wait_queue_publish_fast_state(&poll_waiters);
    /* Keep registration and the visible blocked state atomic to publishers. */
    sched_sleep_current_until(wake_tick);
    kernel_spin_unlock_irqrestore(&poll_waiters.lock, flags);
    return 1;
}

/**
 * @brief Remove a task from the global poll queue if it remains linked.
 * @param task Task whose poll registration should be detached; null is ignored.
 */
void kernel_poll_wait_remove(struct task *task)
{
    uint64_t flags;
    if (!task || !kernel_wait_queue_fast_state(&poll_waiters)) return;
    kernel_spin_lock_irqsave(&poll_waiters.lock, &flags);
    (void)kernel_wait_queue_unlink_locked(&poll_waiters, task);
    kernel_wait_queue_publish_fast_state(&poll_waiters);
    kernel_spin_unlock_irqrestore(&poll_waiters.lock, flags);
}

/**
 * @brief Check whether a task is linked to the global poll queue.
 * @param task Task to inspect; null is unregistered.
 * @return True when task->waiting_queue names the poll queue.
 */
bool kernel_poll_wait_registered(const struct task *task)
{
    uint64_t flags;
    bool found;
    if (!task) return false;
    kernel_spin_lock_irqsave(&poll_waiters.lock, &flags);
    found = task->waiting_queue == &poll_waiters;
    kernel_spin_unlock_irqrestore(&poll_waiters.lock, flags);
    return found;
}

/**
 * @brief Register and block the current task until a queue wakes it.
 * @param queue Queue whose lifetime extends through task unlink or wake; null is ignored.
 */
void kernel_wait_queue_block_current(struct kernel_wait_queue *queue)
{
    kernel_wait_queue_block_current_minimum(queue, 0);
}

/**
 * @brief Register and block the current task with a minimum resource threshold.
 * @param queue Queue whose lifetime extends through task unlink or wake; null is ignored.
 * @param minimum Minimum resource amount required by a satisfied wake.
 */
void kernel_wait_queue_block_current_minimum(struct kernel_wait_queue *queue,
                                             uint32_t minimum)
{
    struct task *task;
    if (!queue) return;
    task = sched_current_task();
    if (!task || task->pid == 0 || task->state == TASK_EXITED) return;
    /* The syscall epilogue retries after a wake; registration and scheduler
     * state must become visible atomically to IRQ and other CPU publishers. */
    (void)kernel_wait_queue_add_minimum(queue, task, minimum, true);
}
