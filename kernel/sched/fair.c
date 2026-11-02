#include "internal.h"
#include <reliefnt/power.h>

static struct task *fair_task(struct eevdf_entity *entity);

static bool fair_runnable(const struct task *task);

static uint32_t fair_count(uint32_t cpu);

static uint32_t fair_choose_cpu(const struct task *task);

static struct task *fair_pick_cpu(uint32_t cpu);


/**
 * @brief Sample nondecreasing fairness time independent of pending PIT IRQs.
 * @return Microseconds in the shared scheduling domain, with legacy fallback.
 * Caller holds scheduler_lock or exclusive bootstrap ownership. The ACPI PM
 * register is shared by CPUs and needs no TSC offset assumption. Preserve
 * fractional cycles and extend 24/32-bit wrap; ordinary scheduling samples
 * within one wrap. Source loss/mask changes reanchor at the previous floor.
 * No allocation, callback or clock/user ABI update occurs.
 */
uint64_t fair_now(void)
{
    uint64_t now = time_uptime_us();
    uint32_t reference, mask;
    if (power_acpi_pm_timer_read(&reference, &mask) &&
        (mask == 0x00ffffffu || mask == UINT32_MAX)) {
        reference &= mask;
        if (!fair_reference.valid || fair_reference.mask != mask) {
            fair_reference = (struct fair_clock_reference){
                .microseconds = now > fair_clock ? now : fair_clock,
                .last = reference, .mask = mask, .valid = true};
        } else {
            uint64_t cycles = (reference - fair_reference.last) & mask;
            uint64_t scaled = cycles * 1000000u + fair_reference.remainder;
            uint64_t elapsed = scaled / ACPI_PM_TIMER_HZ;
            fair_reference.remainder = scaled % ACPI_PM_TIMER_HZ;
            fair_reference.last = reference;
            if (UINT64_MAX - fair_reference.microseconds < elapsed)
                fair_reference.microseconds = UINT64_MAX;
            else fair_reference.microseconds += elapsed;
        }
        if (fair_reference.microseconds > now)
            now = fair_reference.microseconds;
    } else fair_reference.valid = false;
    /* Neither a source transition nor legacy interpolation can rewind V. */
    if (now > fair_clock) fair_clock = now;
    return fair_clock;
}

static struct task *fair_task(struct eevdf_entity *entity)
{
    return (struct task *)((char *)entity - __builtin_offsetof(struct task, fair));
}

static bool fair_runnable(const struct task *task)
{
    return task->kind == TASK_KIND_USER &&
        (task->state == TASK_READY || task->state == TASK_RUNNING) &&
        (!task->vfork_child || kernel_signal_fatal_pending(task)) &&
        (task->entry || (task->image && task->image_len) || (task->flags & TASK_FLAG_PENDING_LOAD)) &&
        task->stack_top && sched_task_as(task)->cr3;
}

void fair_account_cpu(uint32_t cpu, uint64_t now)
{
    struct task *task = fair_running[cpu];
    if (task && task->fair.initialized && now > fair_started[cpu])
        eevdf_account(&task->fair, now - fair_started[cpu]);
    fair_started[cpu] = now;
}

void fair_stop_cpu(uint32_t cpu)
{
    fair_account_cpu(cpu, fair_now());
    fair_running[cpu] = NULL;
}

static uint32_t fair_count(uint32_t cpu)
{
    return fair_queues[cpu].nr_active;
}

static uint32_t fair_choose_cpu(const struct task *task)
{
    uint32_t best = SCHED_CPU_NONE, count = UINT32_MAX;
    uint32_t cpus = smp_cpu_count();
    if (!cpus) cpus = 1;
    if (cpus > SMP_MAX_CPUS) cpus = SMP_MAX_CPUS;
    for (uint32_t cpu = 0; cpu < cpus; ++cpu) {
        if (!smp_cpu_online(cpu) || !task_cpu_allowed(task, cpu)) continue;
        uint32_t n = fair_count(cpu);
        if (n < count || (n == count && cpu == task->last_cpu)) {
            best = cpu; count = n;
        }
    }
    return best;
}

void fair_forget(struct task *task)
{
    if (task->fair.queued) eevdf_dequeue(&fair_queues[task->fair.cpu], &task->fair);
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; ++cpu)
        if (fair_running[cpu] == task) fair_running[cpu] = NULL;
}

void fair_sync(void)
{
    uint64_t now = fair_now();
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; ++cpu) fair_account_cpu(cpu, now);
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        struct eevdf_entity *e = &task->fair;
        bool runnable = fair_runnable(task);
        if (e->queued && !runnable) {
            if (task->state == TASK_EXITED) eevdf_dequeue(&fair_queues[e->cpu], e);
            else if (!e->delayed) eevdf_sleep(&fair_queues[e->cpu], e);
        }
        if (!runnable) continue;
        if (e->queued && e->weight != eevdf_weight(task->priority)) {
            eevdf_reweight(&fair_queues[e->cpu], e, task->priority);
        }
        if (e->queued && !task_cpu_allowed(task, e->cpu) && task->running_cpu == SCHED_CPU_NONE)
            eevdf_dequeue(&fair_queues[e->cpu], e);
        if (!e->queued) {
            uint32_t cpu = task->running_cpu != SCHED_CPU_NONE ? task->running_cpu : fair_choose_cpu(task);
            if (cpu == SCHED_CPU_NONE) continue;
            e->cpu = cpu;
            eevdf_enqueue(&fair_queues[cpu], e, task->priority);
        } else eevdf_set_delayed(e, false);
    }
}

static struct task *fair_pick_cpu(uint32_t cpu)
{
    struct eevdf_entity *e = eevdf_pick(&fair_queues[cpu]);
    if (!e) return NULL;
    struct task *task = fair_task(e);
    if (task->state != TASK_READY ||
        (task->running_cpu != SCHED_CPU_NONE && task->running_cpu != cpu) ||
        !task_cpu_allowed(task, cpu)) return NULL;
    return task;
}

struct task *fair_select(uint32_t cpu)
{
    fair_sync();
    struct task *task = fair_pick_cpu(cpu);
    if (task) return task;
    /* Idle CPUs pull runnable, unreserved work. Transfer bounded lag, not an
     * absolute virtual timestamp from a different CPU's fairness domain. */
    struct task *steal = NULL;
    uint32_t largest = 0;
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *candidate = tasks[i];
        struct eevdf_entity *e = &candidate->fair;
        if (!e->queued || e->delayed || e->cpu == cpu || candidate->state != TASK_READY ||
            candidate->running_cpu != SCHED_CPU_NONE || !task_cpu_allowed(candidate, cpu)) continue;
        uint32_t count = fair_count(e->cpu);
        if (!steal || count > largest) { steal = candidate; largest = count; }
    }
    if (steal) {
        struct eevdf_entity *e = &steal->fair;
        eevdf_dequeue(&fair_queues[e->cpu], e);
        e->cpu = cpu;
        eevdf_enqueue(&fair_queues[cpu], e, steal->priority);
        task = fair_pick_cpu(cpu);
    }
    return task;
}

void task_fair_bind(struct task *task)
{
    eevdf_bind(&task->fair, &((struct sched_task_object *)task)->fair_node);
}

void task_fair_reset(struct task *task)
{
    task->fair = (struct eevdf_entity){0};
    task_fair_bind(task);
}
