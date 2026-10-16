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
            queue->minimum[i] = minimum;
            kernel_spin_unlock_irqrestore(&queue->lock, flags);
            return 0;
        }
    }
    if (queue->count >= KERNEL_WAIT_QUEUE_MAX) {
        kernel_spin_unlock_irqrestore(&queue->lock, flags);
        return -1;
    }
    queue->minimum[queue->count] = minimum;
    queue->waiters[queue->count++] = task;
    task->waiting_queue = queue;
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
    if (!queue || !task) {
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
    kernel_spin_unlock_irqrestore(&queue->lock, flags);
}

static uint32_t kernel_wait_queue_wake_one_raw(struct kernel_wait_queue *queue,
                                              uint32_t available)
{
    uint64_t flags;
    struct task *task = NULL;
    if (!queue) {
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
        poll_waiters.minimum[poll_waiters.count] = 0;
        poll_waiters.waiters[poll_waiters.count++] = task;
        task->waiting_queue = &poll_waiters;
    }
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
    if (!task) return;
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
