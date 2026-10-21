/*
 * ReliefOS wait queue implementation.
 * Wakeup changes task state through the scheduler's public ready operation.
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

/* fast_state is only a hint for conservative lock-free early returns. The
 * waiter arrays and count remain authoritative under queue->lock. */
static uint64_t kernel_wait_queue_fast_state(const struct kernel_wait_queue *queue)
{
    return __atomic_load_n(&queue->fast_state, __ATOMIC_ACQUIRE);
}

static void kernel_wait_queue_publish_fast_state(struct kernel_wait_queue *queue)
{
    uint64_t state = 0;
    if (queue->count) {
        uint32_t minimum = UINT32_MAX;
        for (uint32_t i = 0; i < queue->count; ++i) {
            if (queue->minimum[i] < minimum) minimum = queue->minimum[i];
        }
        state = ((uint64_t)queue->count << 32) | minimum;
    }
    __atomic_store_n(&queue->fast_state, state, __ATOMIC_RELEASE);
}

/* Publish a conservative nonempty/minimum-zero hint before adding or changing
 * a waiter. Readers that see it must take the queue lock and recheck the
 * canonical entries after registration starts. A concurrent publisher may
 * still have read the old empty hint before this store: IRQ-facing callers
 * must retain their condition/epoch recheck around registration and sleep. */
static void kernel_wait_queue_publish_busy_fast_state(struct kernel_wait_queue *queue)
{
    __atomic_store_n(&queue->fast_state, (uint64_t)1u << 32, __ATOMIC_RELEASE);
}

void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{
    if (!queue) {
        return;
    }
    kernel_spin_init(&queue->lock);
    queue->count = 0;
    for (uint32_t i = 0; i < KERNEL_WAIT_QUEUE_MAX; ++i) {
        queue->waiters[i] = NULL;
        queue->minimum[i] = 0;
    }
    __atomic_store_n(&queue->fast_state, 0, __ATOMIC_RELEASE);
}

static int kernel_wait_queue_add_minimum(struct kernel_wait_queue *queue,
                                         struct task *task, uint32_t minimum)
{
    uint64_t flags;
    if (!queue || !task) {
        return -1;
    }
    if (task->waiting_queue && task->waiting_queue != queue)
        kernel_wait_queue_remove(task->waiting_queue, task);
    kernel_spin_lock_irqsave(&queue->lock, &flags);
    for (uint32_t i = 0; i < queue->count; ++i) {
        if (queue->waiters[i] == task) {
            kernel_wait_queue_publish_busy_fast_state(queue);
            queue->minimum[i] = minimum;
            kernel_wait_queue_publish_fast_state(queue);
            kernel_spin_unlock_irqrestore(&queue->lock, flags);
            return 0;
        }
    }
    if (queue->count >= KERNEL_WAIT_QUEUE_MAX) {
        kernel_spin_unlock_irqrestore(&queue->lock, flags);
        return -1;
    }
    /* Registration and task->waiting_queue publication happen under the
     * execution transaction, but IRQ publishers may still inspect the hint. */
    kernel_wait_queue_publish_busy_fast_state(queue);
    queue->minimum[queue->count] = minimum;
    queue->waiters[queue->count++] = task;
    task->waiting_queue = queue;
    kernel_wait_queue_publish_fast_state(queue);
    kernel_spin_unlock_irqrestore(&queue->lock, flags);
    return 0;
}

int kernel_wait_queue_add(struct kernel_wait_queue *queue, struct task *task)
{
    return kernel_wait_queue_add_minimum(queue, task, 0);
}

void kernel_wait_queue_remove(struct kernel_wait_queue *queue, struct task *task)
{
    uint64_t flags;
    /* Registration and task teardown are serialized by the global syscall
     * execution lock. Once a registration starts, add publishes BUSY before
     * task->waiting_queue can refer to this queue. */
    if (!queue || !task || !kernel_wait_queue_fast_state(queue)) {
        return;
    }
    kernel_spin_lock_irqsave(&queue->lock, &flags);
    for (uint32_t i = 0; i < queue->count; ++i) {
        if (queue->waiters[i] == task) {
            for (uint32_t j = i + 1; j < queue->count; ++j) {
                queue->waiters[j - 1] = queue->waiters[j];
                queue->minimum[j - 1] = queue->minimum[j];
            }
            queue->waiters[--queue->count] = NULL;
            queue->minimum[queue->count] = 0;
            if (task->waiting_queue == queue) task->waiting_queue = NULL;
            break;
        }
    }
    kernel_wait_queue_publish_fast_state(queue);
    kernel_spin_unlock_irqrestore(&queue->lock, flags);
}

static uint32_t kernel_wait_queue_wake_one_raw(struct kernel_wait_queue *queue,
                                              uint32_t available)
{
    uint64_t flags;
    struct task *task = NULL;
    uint64_t state;
    if (!queue) {
        return 0;
    }
    state = kernel_wait_queue_fast_state(queue);
    if (!state || (available != UINT32_MAX && (uint32_t)state > available)) {
        return 0;
    }
    kernel_spin_lock_irqsave(&queue->lock, &flags);
    for (uint32_t index = 0; index < queue->count; ++index) {
        if (queue->minimum[index] > available) continue;
        task = queue->waiters[index];
        /* Keep the registration linked while the scheduler uses the task.
         * Exit cleanup takes this queue lock before freeing task storage.
         * Interruptible wake also leaves SIGSTOP/vfork suspension intact. */
        sched_wake_interruptible(task);
        for (uint32_t i = index + 1; i < queue->count; ++i) {
            queue->waiters[i - 1] = queue->waiters[i];
            queue->minimum[i - 1] = queue->minimum[i];
        }
        queue->waiters[--queue->count] = NULL;
        queue->minimum[queue->count] = 0;
        if (task->waiting_queue == queue) task->waiting_queue = NULL;
        break;
    }
    kernel_wait_queue_publish_fast_state(queue);
    kernel_spin_unlock_irqrestore(&queue->lock, flags);
    if (task) {
        return 1;
    }
    return 0;
}

static uint32_t kernel_wait_queue_wake_all_raw(struct kernel_wait_queue *queue)
{
    uint32_t count = 0;
    while (kernel_wait_queue_wake_one_raw(queue, UINT32_MAX)) {
        ++count;
    }
    return count;
}

uint32_t kernel_wait_queue_wake_one(struct kernel_wait_queue *queue)
{
    return kernel_wait_queue_wake_one_raw(queue, UINT32_MAX);
}

uint32_t kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{
    return kernel_wait_queue_wake_all_raw(queue);
}

uint32_t kernel_wait_queue_wake_satisfied(struct kernel_wait_queue *queue, uint32_t available)
{
    uint32_t count = 0;
    while (kernel_wait_queue_wake_one_raw(queue, available)) ++count;
    return count;
}

uint64_t kernel_poll_readiness_generation(void)
{
    return __atomic_load_n(&poll_readiness_generation, __ATOMIC_ACQUIRE);
}

void kernel_poll_readiness_notify(void)
{
    __atomic_add_fetch(&poll_readiness_generation, 1, __ATOMIC_SEQ_CST);
    /* Keep each task linked until sched_wake_interruptible() has finished.
     * The queue lock pins its task storage against exit cleanup, which also
     * removes waiting_queue under this lock. Unlike sched_mark_ready(), this
     * wake must leave TASK_STOPPED tasks stopped. Holding the lock across the
     * bounded (64-task) batch prevents a busy publisher from repeatedly
     * waking pollers that register after this generation change. */
    uint64_t flags;
    kernel_spin_lock_irqsave(&poll_waiters.lock, &flags);
    uint32_t count = poll_waiters.count;
    while (count-- && poll_waiters.count) {
        struct task *task = poll_waiters.waiters[0];
        sched_wake_interruptible(task);
        for (uint32_t i = 1; i < poll_waiters.count; ++i) {
            poll_waiters.waiters[i - 1] = poll_waiters.waiters[i];
            poll_waiters.minimum[i - 1] = poll_waiters.minimum[i];
        }
        poll_waiters.waiters[--poll_waiters.count] = NULL;
        poll_waiters.minimum[poll_waiters.count] = 0;
        if (task->waiting_queue == &poll_waiters) task->waiting_queue = NULL;
        kernel_wait_queue_publish_fast_state(&poll_waiters);
    }
    kernel_spin_unlock_irqrestore(&poll_waiters.lock, flags);
}

int kernel_poll_wait_current(uint64_t generation, uint64_t wake_tick)
{
    struct task *task = sched_current_task();
    uint64_t flags;
    uint32_t i;
    bool found = false;

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
    for (i = 0; i < poll_waiters.count; ++i) {
        if (poll_waiters.waiters[i] == task) {
            found = true;
            break;
        }
    }
    if (!found) {
        if (poll_waiters.count >= KERNEL_WAIT_QUEUE_MAX) {
            kernel_spin_unlock_irqrestore(&poll_waiters.lock, flags);
            return -1;
        }
        /* Keep lock-free removers from treating a partially published poll
         * registration as an empty queue. Generation validation and notify's
         * unconditional lock still close the readiness check-to-sleep race. */
        kernel_wait_queue_publish_busy_fast_state(&poll_waiters);
        poll_waiters.minimum[poll_waiters.count] = 0;
        poll_waiters.waiters[poll_waiters.count++] = task;
        task->waiting_queue = &poll_waiters;
    }
    kernel_wait_queue_publish_fast_state(&poll_waiters);
    /* Holding poll_waiters.lock across the state transition prevents a
     * publisher that increments the generation after the equality check
     * from emptying the queue until the task is visibly blocked. */
    sched_sleep_current_until(wake_tick);
    kernel_spin_unlock_irqrestore(&poll_waiters.lock, flags);
    return 1;
}

void kernel_poll_wait_remove(struct task *task)
{
    uint64_t flags;
    /* Poll registration/removal for a task is within the syscall execution
     * transaction. The BUSY publication precedes any new task pointer. */
    if (!task || !kernel_wait_queue_fast_state(&poll_waiters)) return;
    kernel_spin_lock_irqsave(&poll_waiters.lock, &flags);
    for (uint32_t i = 0; i < poll_waiters.count; ++i) {
        if (poll_waiters.waiters[i] != task) continue;
        for (uint32_t j = i + 1; j < poll_waiters.count; ++j) {
            poll_waiters.waiters[j - 1] = poll_waiters.waiters[j];
            poll_waiters.minimum[j - 1] = poll_waiters.minimum[j];
        }
        poll_waiters.waiters[--poll_waiters.count] = NULL;
        poll_waiters.minimum[poll_waiters.count] = 0;
        if (task->waiting_queue == &poll_waiters) task->waiting_queue = NULL;
        break;
    }
    kernel_wait_queue_publish_fast_state(&poll_waiters);
    kernel_spin_unlock_irqrestore(&poll_waiters.lock, flags);
}

bool kernel_poll_wait_registered(const struct task *task)
{
    uint64_t flags;
    bool found = false;
    if (!task) return false;
    kernel_spin_lock_irqsave(&poll_waiters.lock, &flags);
    for (uint32_t i = 0; i < poll_waiters.count; ++i) {
        if (poll_waiters.waiters[i] == task) {
            found = true;
            break;
        }
    }
    kernel_spin_unlock_irqrestore(&poll_waiters.lock, flags);
    return found;
}

void kernel_wait_queue_block_current_minimum(struct kernel_wait_queue *queue, uint32_t minimum)
{
    struct task *task;
    if (!queue) {
        return;
    }
    task = sched_current_task();
    if (!task || task->pid == 0 || task->state == TASK_EXITED) {
        return;
    }
    if (kernel_wait_queue_add_minimum(queue, task, minimum) < 0) {
        return;
    }
    /* The syscall epilogue rewinds the interrupted int 0x80 and retries it
     * after wakeup, so returning EAGAIN internally is not observable to user
     * space unless the descriptor is O_NONBLOCK (handled by the caller). */
    sched_block_current();
}

void kernel_wait_queue_block_current(struct kernel_wait_queue *queue)
{
    kernel_wait_queue_block_current_minimum(queue, 0);
}
