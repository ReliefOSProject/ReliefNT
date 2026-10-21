#include "internal.h"

static uint64_t cpu_accounting_add(uint64_t value, uint64_t delta);

/**
 * @brief Bounded, always-NUL-terminated copy for task snapshot string fields.
 */
static void snapshot_name(char *dst, size_t dst_len, const char *src);


static uint64_t cpu_accounting_add(uint64_t value, uint64_t delta)
{
    return delta > UINT64_MAX - value ? UINT64_MAX : value + delta;
}

void cpu_accounting_settle(uint32_t cpu)
{
    if (!smp_cpu_online(cpu)) {
        cpu_accounting[cpu].started = false;
        return;
    }
    if (!cpu_accounting[cpu].started) {
        cpu_accounting[cpu].last_tick = scheduler_ticks;
        cpu_accounting[cpu].started = true;
        return;
    }
    uint64_t delta = scheduler_ticks - cpu_accounting[cpu].last_tick;
    cpu_accounting[cpu].last_tick = scheduler_ticks;
    struct task *owner = current_task[cpu];
    /* BLOCKED/READY/EXITED can still own the live CPU until its syscall or
     * interrupt handoff retires the reservation. Bill that interval to the
     * owner, not idle time, and settle it before publishing another owner. */
    if (owner && owner->pid && owner->pid == current_pid[cpu] && owner->running_cpu == cpu) {
        cpu_accounting[cpu].busy_ticks = cpu_accounting_add(cpu_accounting[cpu].busy_ticks, delta);
        owner->cpu_ticks = cpu_accounting_add(owner->cpu_ticks, delta);
    } else {
        cpu_accounting[cpu].idle_ticks = cpu_accounting_add(cpu_accounting[cpu].idle_ticks, delta);
    }
}

void cpu_accounting_sync(void)
{
    uint32_t count = smp_cpu_count();
    if (count > SMP_MAX_CPUS) count = SMP_MAX_CPUS;
    for (uint32_t cpu = 0; cpu < count; ++cpu) cpu_accounting_settle(cpu);
}

/**
 * @brief Return the total number of scheduler ticks since init.
 */
uint64_t sched_tick_count(void)
{
    return scheduler_ticks;
}

/**
 * @brief Read a consistent snapshot of the Linux Q16 load averages.
 * @param loads Output array of three 1/5/15-minute averages; must not be NULL.
 * @return None. The scheduler lock serializes readers with BSP tick sampling.
 */
void sched_load_averages(uint64_t loads[3])
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    for (unsigned i = 0; i < 3; ++i) loads[i] = scheduler_loads[i];
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

/**
 * @brief Write the accumulated busy and idle tick counters into the caller's outputs.
 */
void sched_cpu_ticks(uint64_t *busy_ticks, uint64_t *idle_ticks)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    cpu_accounting_sync();
    uint64_t busy = 0, idle = 0;
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; ++cpu) {
        busy = cpu_accounting_add(busy, cpu_accounting[cpu].busy_ticks);
        idle = cpu_accounting_add(idle, cpu_accounting[cpu].idle_ticks);
    }
    if (busy_ticks) {
        *busy_ticks = busy;
    }
    if (idle_ticks) {
        *idle_ticks = idle;
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

void sched_cpu_ticks_per_cpu(uint64_t *busy_ticks, uint64_t *idle_ticks,
                             uint32_t capacity)
{
    uint64_t flags;
    uint32_t count = capacity < SMP_MAX_CPUS ? capacity : SMP_MAX_CPUS;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    cpu_accounting_sync();
    for (uint32_t i = 0; i < count; ++i) {
        if (busy_ticks) {
            busy_ticks[i] = cpu_accounting[i].busy_ticks;
        }
        if (idle_ticks) {
            idle_ticks[i] = cpu_accounting[i].idle_ticks;
        }
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

void sched_cpu_runtime_snapshot(uint64_t *busy_ticks, uint64_t *idle_ticks,
                                uint32_t *current_pids, uint32_t *ready_counts,
                                uint32_t capacity)
{
    uint64_t flags;
    uint32_t count = capacity < SMP_MAX_CPUS ? capacity : SMP_MAX_CPUS;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    cpu_accounting_sync();
    for (uint32_t cpu = 0; cpu < count; ++cpu) {
        if (busy_ticks) {
            busy_ticks[cpu] = cpu_accounting[cpu].busy_ticks;
        }
        if (idle_ticks) {
            idle_ticks[cpu] = cpu_accounting[cpu].idle_ticks;
        }
        if (current_pids) {
            current_pids[cpu] = current_pid[cpu];
        }
        if (ready_counts) {
            uint32_t ready = 0;
            for (uint32_t i = 0; i < task_count; ++i) {
                if (tasks[i]->state == TASK_READY && task_cpu_allowed(tasks[i], cpu)) {
                    ++ready;
                }
            }
            ready_counts[cpu] = ready;
        }
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

uint64_t sched_task_cpu_ticks(const struct task *task, bool process)
{
    uint64_t flags, result = 0;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    cpu_accounting_sync();
    if (task && !process) result = task->cpu_ticks;
    else if (task) {
        uint32_t group = sched_task_tgid(task);
        for (uint32_t i = 0; i < task_count; ++i)
            if (sched_task_tgid(tasks[i]) == group)
                result = cpu_accounting_add(result, tasks[i]->cpu_ticks);
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return result;
}

/**
 * @brief Count tasks by RUNNING/READY/BLOCKED state and report each plus the total count.
 */
void sched_task_counts(uint32_t *out_task_count, uint32_t *running_tasks,
                       uint32_t *ready_tasks, uint32_t *sleeping_tasks)
{
    uint32_t running = 0;
    uint32_t ready = 0;
    uint32_t sleeping = 0;
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    for (uint32_t i = 0; i < task_count; ++i) {
        if (tasks[i]->state == TASK_RUNNING) {
            ++running;
        } else if (tasks[i]->state == TASK_READY) {
            ++ready;
        } else if (tasks[i]->state == TASK_BLOCKED) {
            ++sleeping;
        }
    }
    if (out_task_count) {
        *out_task_count = task_count;
    }
    if (running_tasks) {
        *running_tasks = running;
    }
    if (ready_tasks) {
        *ready_tasks = ready;
    }
    if (sleeping_tasks) {
        *sleeping_tasks = sleeping;
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

/**
 * @brief Bounded, always-NUL-terminated copy for task snapshot string fields.
 */
static void snapshot_name(char *dst, size_t dst_len, const char *src)
{
    size_t i = 0;
    if (!dst || dst_len == 0) {
        return;
    }
    if (src) {
        while (i + 1 < dst_len && src[i]) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = 0;
}

/**
 * @brief Fill out[] with up to capacity task snapshots, storing the tick count via tick.
 */
uint32_t sched_snapshot(struct task_snapshot_info *out, uint32_t capacity, uint64_t *tick)
{
    uint64_t flags;
    uint32_t result;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    cpu_accounting_sync();
    if (tick) {
        *tick = scheduler_ticks;
    }
    if (!out || capacity == 0) {
        result = task_count;
        kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
        return result;
    }
    uint32_t n = task_count < capacity ? task_count : capacity;
    for (uint32_t i = 0; i < n; ++i) {
        struct task_snapshot_info *dst = &out[i];
        dst->pid = tasks[i]->pid;
        dst->parent_pid = tasks[i]->parent_pid;
        dst->state = (uint32_t)tasks[i]->state;
        dst->kind = (uint32_t)tasks[i]->kind;
        dst->flags = tasks[i]->flags;
        dst->uid = tasks[i]->uid;
        dst->role = tasks[i]->role;
        dst->session_id = tasks[i]->session_id;
        dst->memory_kib = address_space_user_memory_kib(sched_task_as(tasks[i]));
        dst->cpu_ticks = tasks[i]->cpu_ticks;
        dst->priority = tasks[i]->priority;
        dst->pending_signals = tasks[i]->pending_signals;
        dst->wake_tick = tasks[i]->wake_tick;
        dst->entry = tasks[i]->entry;
        dst->cr3 = (*sched_task_as(tasks[i])).cr3;
        dst->affinity_mask = tasks[i]->affinity_mask;
        snapshot_name(dst->name, sizeof(dst->name), tasks[i]->name);
        snapshot_name(dst->username, sizeof(dst->username), tasks[i]->username);
    }
    result = n;
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return result;
}
