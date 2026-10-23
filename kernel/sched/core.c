#include "internal.h"

static void sched_request_reschedule_cpu(uint32_t cpu);

static bool task_frame_valid(const struct task *task, const struct trap_frame *frame);

struct task **tasks;

uint32_t task_count;

static volatile uint32_t sched_resched_pending[SMP_MAX_CPUS];
uint32_t task_capacity;

uint32_t next_pid = 1;

uint32_t current_pid[SMP_MAX_CPUS];

struct task *current_task[SMP_MAX_CPUS];

uint32_t next_session_id = 1;

uint64_t scheduler_ticks;

uint64_t scheduler_loads[3];

struct cpu_runtime_accounting cpu_accounting[SMP_MAX_CPUS];

struct kernel_spinlock scheduler_lock = KERNEL_SPINLOCK_INIT;

struct eevdf_queue fair_queues[SMP_MAX_CPUS];

struct task *fair_running[SMP_MAX_CPUS];

uint64_t fair_started[SMP_MAX_CPUS];

uint64_t fair_clock;

uint32_t scheduler_cpu_index(void)
{
    uint32_t cpu = smp_current_cpu();
    return cpu < SMP_MAX_CPUS ? cpu : 0;
}

static void sched_request_reschedule_cpu(uint32_t cpu)
{
    if (cpu < SMP_MAX_CPUS)
        __atomic_store_n(&sched_resched_pending[cpu], 1, __ATOMIC_RELEASE);
}

void sched_request_reschedule_all_online(void)
{
    uint32_t count = smp_cpu_count();
    uint32_t current = scheduler_cpu_index();
    if (count > SMP_MAX_CPUS) count = SMP_MAX_CPUS;
    for (uint32_t cpu = 0; cpu < count; ++cpu)
        if (cpu == current || smp_cpu_online(cpu)) sched_request_reschedule_cpu(cpu);
    if (current >= count) sched_request_reschedule_cpu(current);
}

void sched_notify_runnable(void)
{
    sched_request_reschedule_all_online();
}

bool sched_current_cpu_needs_resched(void)
{
    return __atomic_load_n(&sched_resched_pending[scheduler_cpu_index()],
                           __ATOMIC_ACQUIRE) != 0;
}

uint64_t sched_all_cpu_mask(void)
{
    uint32_t count = smp_cpu_count();
    if (count == 0) {
        count = 1;
    }
    if (count >= 64u) {
        return UINT64_MAX;
    }
    return (1ULL << count) - 1ULL;
}

bool task_cpu_allowed(const struct task *task, uint32_t cpu)
{
    uint64_t mask = task && task->affinity_mask ? task->affinity_mask : sched_all_cpu_mask();
    return cpu < 64u && (mask & (1ULL << cpu)) != 0;
}

uint32_t scheduler_current_pid(void)
{
    return current_pid[scheduler_cpu_index()];
}

static bool task_frame_valid(const struct task *task, const struct trap_frame *frame)
{
    return task && frame && (*sched_task_as(task)).cr3 && task->stack_low && task->stack_top &&
           frame->rip >= RELIEFNT_USER_BASE && frame->rip < RELIEFNT_USER_TOP &&
           frame->rsp >= RELIEFNT_USER_BASE && frame->rsp <= RELIEFNT_USER_TOP &&
           frame->cs == RELIEFNT_USER_CS && frame->ss == RELIEFNT_USER_DS &&
           (frame->rflags & (1ULL << 9));
}

/**
 * @brief Reset the task table, pid/session counters, and tick statistics to their initial state.
 */
void sched_init(void)
{
    kernel_spin_init(&scheduler_lock);
    tasks = NULL;
    task_count = 0;
    task_capacity = 0;
    next_pid = 1;
    for (uint32_t i = 0; i < SMP_MAX_CPUS; ++i) {
        current_pid[i] = 0;
        current_task[i] = NULL;
        __atomic_store_n(&sched_resched_pending[i], 0, __ATOMIC_RELAXED);
    }
    next_session_id = 1;
    scheduler_ticks = 0;
    for (unsigned i = 0; i < 3; ++i) scheduler_loads[i] = 0;
    for (uint32_t i = 0; i < SMP_MAX_CPUS; ++i) {
        fair_queues[i] = (struct eevdf_queue){0};
        fair_running[i] = NULL;
        fair_started[i] = 0;
        cpu_accounting[i] = (struct cpu_runtime_accounting){
            .started = smp_cpu_online(i)};
    }
    fair_clock = 0;
    console_info("[reliefnt] EEVDF scheduler initialized\n");
}

/**
 * @brief Create the pid-0 kernel idle task and mark it the running task.
 */
void sched_create_idle_task(void)
{
    struct task *task = alloc_task_slot();
    if (!task) {
        return;
    }
    task_zero(task);
    task->pid = 0;
    task->parent_pid = 0;
    task->process_group = 0;
    task->process_session = 0;
    task_copy_name(task, "idle");
    task->entry = 0;
    task->stack_top = 0;
    task->stack_low = 0;
    task->wake_tick = 0;
    task->exit_code = 0;
    task->image = NULL;
    task->image_len = 0;
    for (size_t i = 0; i < sizeof((*sched_task_as(task))); ++i) {
        ((uint8_t *)sched_task_as(task))[i] = 0;
    }
    for (size_t i = 0; i < sizeof(task->frame); ++i) {
        ((uint8_t *)&task->frame)[i] = 0;
    }
    task->state = TASK_RUNNING;
    task->kind = TASK_KIND_KERNEL;
    task->flags = 0;
    task->pty_id = 0;
    task_clear_identity(task);
    task_copy_cwd(task, "/");
}

/**
 * @brief Make pid the running task and demote every other RUNNING task back to READY.
 */
void sched_set_running(uint32_t pid)
{
    uint64_t flags;
    uint32_t cpu = scheduler_cpu_index();
    bool made_ready = false;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    struct task *selected = NULL;
    for (uint32_t i = 0; i < task_count; ++i) {
        if (tasks[i]->pid == pid) {
            selected = tasks[i];
            break;
        }
    }
    if (!selected || selected->kind != TASK_KIND_USER ||
        selected->state == TASK_EXITED || !(*sched_task_as(selected)).cr3 ||
        !selected->frame.rip || (selected->frame.cs & 3ULL) != 3ULL ||
        (selected->frame.ss & 3ULL) != 3ULL ||
        !(selected->frame.rflags & (1ULL << 9))) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
        return;
    }
    fair_stop_cpu(cpu);
    cpu_accounting_settle(cpu);
    current_pid[cpu] = pid;
    current_task[cpu] = selected;
    for (uint32_t i = 0; i < task_count; ++i) {
        if (tasks[i] == selected) {
            tasks[i]->state = TASK_RUNNING;
            tasks[i]->running_cpu = cpu;
            tasks[i]->last_cpu = cpu;
        } else if (tasks[i]->state == TASK_RUNNING && tasks[i]->running_cpu == cpu) {
            tasks[i]->state = TASK_READY;
            tasks[i]->running_cpu = SCHED_CPU_NONE;
            made_ready = true;
        }
    }
    if (selected->fair.queued && selected->fair.cpu != cpu)
        eevdf_dequeue(&fair_queues[selected->fair.cpu], &selected->fair);
    fair_sync();
    fair_running[cpu] = selected;
    fair_started[cpu] = fair_now();
    if (made_ready) sched_request_reschedule_all_online();
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

/** @brief Account a tick and queue expirations; signal actions run on return to user mode. */
void sched_on_tick(void)
{
    bool woke = false;
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    /* Each CPU owns its reschedule request; another CPU's scheduler round must
     * not consume this tick's preemption decision. */
    sched_request_reschedule_cpu(scheduler_cpu_index());
    ++scheduler_ticks;
    if (scheduler_ticks % (5 * RELIEFNT_TICK_HZ) == 0) {
        uint32_t active = 0;
        /* TASK_BLOCKED represents interruptible waits (including vfork), not
         * Linux D-state I/O waits. Do not count sleepers or the pid-0 idle task. */
        for (uint32_t i = 0; i < task_count; ++i)
            if (tasks[i]->pid && (tasks[i]->state == TASK_RUNNING || tasks[i]->state == TASK_READY)) ++active;
        sched_load_update(scheduler_loads, active);
    }
    cpu_accounting_sync();
    for (uint32_t i = 0; i < task_count; ++i) {
        if (tasks[i]->state == TASK_BLOCKED && !tasks[i]->vfork_child && tasks[i]->wake_tick &&
            tasks[i]->wake_tick <= scheduler_ticks) {
            tasks[i]->wake_tick = 0;
            tasks[i]->state = TASK_READY;
            woke = true;
            /* A blocked task may still be reserved by the CPU that is
             * finishing its syscall.  Preserve that reservation; the owner
             * will either reclaim it or release it when selecting another
             * task. */
        }
    }
    sched_alarm_expire();
    sched_posix_timer_expire();
    if (woke) sched_request_reschedule_all_online();
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

/**
 * Request local-APIC preemption. The BSP PIT alone advances wall time,
 * sleepers and CPU usage; an AP tick must never manufacture CPU time.
 */
void sched_on_cpu_tick(void)
{
    uint32_t cpu = scheduler_cpu_index();
    uint64_t flags;

    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    sched_request_reschedule_cpu(cpu);
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

void sched_yield_current(void)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    uint32_t cpu = scheduler_cpu_index();
    fair_account_cpu(cpu, fair_now());
    struct task *task = sched_current_task();
    if (task && task->fair.queued) eevdf_yield(&task->fair);
    sched_request_reschedule_cpu(cpu);
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

/**
 * @brief Request a normal scheduling decision on the current CPU.
 * @note The caller holds the IRQ-disabled syscall execution transaction and
 *       will soon reach the normal user-return scheduling boundary. This
 *       changes only the local reschedule flag; it does not yield the task or
 *       alter scheduler accounting, task state, or remote CPU flags.
 */
void sched_request_reschedule_current(void)
{
    sched_request_reschedule_cpu(scheduler_cpu_index());
}

/**
 * @brief Return the pid of the currently running task (0 before the first switch).
 */
uint32_t sched_current_pid(void)
{
    return scheduler_current_pid();
}

int sched_get_task_affinity(uint32_t pid, uint64_t *mask)
{
    uint64_t flags;
    int result = -2;
    if (!mask) {
        return -22;
    }
    if (!pid) {
        pid = scheduler_current_pid();
    }
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    for (uint32_t i = 0; i < task_count; ++i) {
        if (tasks[i]->pid == pid && tasks[i]->state != TASK_EXITED) {
            *mask = tasks[i]->affinity_mask ? tasks[i]->affinity_mask : sched_all_cpu_mask();
            result = 0;
            break;
        }
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return result;
}

int sched_set_task_affinity(uint32_t pid, uint64_t mask)
{
    uint64_t flags;
    uint64_t allowed = sched_all_cpu_mask();
    int result = -2;
    if (!pid) {
        pid = scheduler_current_pid();
    }
    if (!mask || !(mask & allowed)) {
        return -22;
    }
    mask &= allowed;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    for (uint32_t i = 0; i < task_count; ++i) {
        if (tasks[i]->pid == pid && tasks[i]->state != TASK_EXITED) {
            tasks[i]->affinity_mask = mask;
            result = 0;
            break;
        }
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return result;
}

/**
 * @brief Return the struct task for the currently running pid, or NULL.
 */
struct task *sched_current_task(void)
{
    struct task *task = current_task[scheduler_cpu_index()];
    /* current_task is NULL only when no user task owns this CPU (idle), where
     * the pid-0 idle task is still the historical answer for sched_find(). */
    return task ? task : sched_find(scheduler_current_pid());
}

/**
 * Save the interrupted Ring-3 state before its CPU releases the task. The
 * frame copy and ownership change share one scheduler-lock transaction so no
 * other CPU can claim a partially published return context.
 */
bool sched_capture_current_user_frame(const struct trap_frame *frame)
{
    uint64_t flags;
    uint32_t cpu = scheduler_cpu_index();
    struct task *current = NULL;

    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    current = current_task[cpu];
    if (!current || current->pid != current_pid[cpu] || current->kind != TASK_KIND_USER ||
        current->state == TASK_EXITED || current->running_cpu != cpu) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
        return false;
    }
    fair_stop_cpu(cpu);
    /* execve has already installed a fresh address space and reset the saved
     * frame for the pending image.  The trap frame passed here still belongs
     * to the replaced program; copying it would resurrect the old RIP and
     * register state. Keep the replacement frame intact for the normal
     * image-loader path; selection will retire this CPU's reservation. */
    if (!(current->flags & TASK_FLAG_PENDING_LOAD)) {
        if (!task_frame_valid(current, frame)) {
            kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
            return false;
        }
        current->frame = *frame;
    }
    if (current->state == TASK_RUNNING) {
        current->state = TASK_READY;
    }
    /* Saving registers does not retire CR3 or the caller's task pointer.
     * Keep ownership until selection switches to the kernel page tables. */
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return true;
}

/**
 * Save a syscall/timer return frame while preserving the current CPU's fair
 * accounting interval.  This fast path is deliberately narrower than full
 * capture: callers must schedule normally for signals, newly runnable work,
 * a blocked/exited task, an image replacement, or an affinity change.
 */
bool sched_try_resume_current_user_frame(const struct trap_frame *frame)
{
    uint64_t flags;
    uint32_t cpu = scheduler_cpu_index();
    bool resume = false;

    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    struct task *current = current_task[cpu];
    if (current && current->pid == current_pid[cpu] &&
        current->kind == TASK_KIND_USER && current->state == TASK_RUNNING &&
        current->running_cpu == cpu && !(current->flags & TASK_FLAG_PENDING_LOAD) &&
        !((__atomic_load_n(&current->pending_signals, __ATOMIC_ACQUIRE) |
           __atomic_load_n(sched_task_process_pending(current), __ATOMIC_ACQUIRE)) &
          ~current->blocked_signals) &&
        !__atomic_load_n(&sched_resched_pending[cpu], __ATOMIC_ACQUIRE) &&
        task_cpu_allowed(current, cpu) && task_frame_valid(current, frame)) {
        current->frame = *frame;
        resume = true;
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return resume;
}

/**
 * A remote exit may mark a task terminal while it is still executing in
 * Ring-3 on this CPU.  Keep its address space and descriptor table alive
 * until this CPU reaches a scheduling boundary, then drop only this CPU's
 * reservation.  Reapers and slot recycling require running_cpu == NONE.
 */
void sched_quiesce_exited_current(void)
{
    uint32_t cpu = scheduler_cpu_index();
    uint32_t pid;
    uint64_t flags;
    uint32_t gpu_owner = 0;

    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    fair_stop_cpu(cpu);
    cpu_accounting_settle(cpu);
    pid = current_pid[cpu];
    for (uint32_t i = 0; i < task_count; ++i) {
        if (tasks[i]->pid == pid && tasks[i]->state == TASK_EXITED) {
            if (tasks[i]->running_cpu == cpu ||
                tasks[i]->running_cpu == SCHED_CPU_NONE) {
                tasks[i]->running_cpu = SCHED_CPU_NONE;
                current_pid[cpu] = 0;
                current_task[cpu] = NULL;
                gpu_owner = pid;
            }
            break;
        }
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    if (gpu_owner) svga_gpu_release_owner(gpu_owner);
}

/**
 * @brief Retire the old CR3 and select this CPU's earliest eligible deadline.
 * Caller holds the execution transaction with interrupts disabled and has
 * saved the live frame. scheduler_lock serializes ownership publication.
 * @return Reserved user task, or NULL after retiring into the kernel CR3.
 */
struct task *sched_select_next_user(void)
{
    struct task *best = NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    uint32_t cpu = scheduler_cpu_index();
    struct task *old = sched_find(current_pid[cpu]);
    fair_stop_cpu(cpu);
    best = fair_select(cpu);
    cpu_accounting_settle(cpu);
    if (!best || best != old) {
        /* The execution transaction pins task metadata around this handoff.
         * Switch CR3 before publishing the old task to remote CPUs/reapers,
         * including the no-runnable-task path that enters the idle loop. */
        paging_load_cr3(paging_kernel_cr3());
        if (old && old->running_cpu == cpu) old->running_cpu = SCHED_CPU_NONE;
        current_pid[cpu] = 0;
        current_task[cpu] = NULL;
    }
    /* Same-task continuation skips the retirement above entirely: nothing is
     * published for reapers to act on, and the return stub reloads the task
     * CR3 (still the required full refresh) either way. */
    if (best) {
        best->state = TASK_RUNNING;
        best->running_cpu = cpu;
        best->last_cpu = cpu;
        current_pid[cpu] = best->pid;
        current_task[cpu] = best;
        fair_running[cpu] = best;
        fair_started[cpu] = fair_now();
    }
    __atomic_store_n(&sched_resched_pending[cpu], 0, __ATOMIC_RELEASE);
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return best;
}

/**
 * @brief Retry selection without bypassing EEVDF eligibility or CPU ownership.
 */
struct task *sched_reclaim_current_user(void)
{
    return sched_select_next_user();
}

/**
 * @brief Return a pointer to the task's saved trap frame, or NULL for a NULL task.
 */
struct trap_frame *sched_task_frame(struct task *task)
{
    if (!task || task->kind != TASK_KIND_USER || task->state == TASK_EXITED ||
        !task_frame_valid(task, &task->frame)) {
        if (task) {
            console_error("[reliefnt] rejected invalid user frame pid=%u rip=0x%llx rsp=0x%llx cr3=0x%llx\n",
                           task->pid,
                           (unsigned long long)task->frame.rip,
                           (unsigned long long)task->frame.rsp,
                           (unsigned long long)(*sched_task_as(task)).cr3);
        }
        return NULL;
    }
    return &task->frame;
}

/**
 * @brief Return the task's address-space CR3, or 0 for a NULL task.
 */
uint64_t sched_task_cr3(struct task *task)
{
    return task ? (*sched_task_as(task)).cr3 : 0;
}

/**
 * @brief Return the process group of pid (0 = current), or a negative error.
 */
int64_t sched_process_identity(uint32_t pid, bool session)
{
    if (!pid) pid = sched_current_pid();
    uint64_t flags;
    int64_t result = -LINUX_ESRCH;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    const struct task *task = sched_find(pid);
    /* A zombie still owns its identity. Reaped slots stay in the task table
     * until recycled, but must no longer be visible through a Linux PID. */
    if (task && task->kind == TASK_KIND_USER &&
        (task->state != TASK_EXITED || (task->flags & TASK_FLAG_WAITABLE_CHILD))) {
        result = session ? task->process_session : task->process_group;
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return result;
}

/**
 * @brief Read or set a user task's nice-style priority, clamping new values to [-20, 19].
 */
int sched_task_priority(uint32_t pid, int priority, int set)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    struct task *task = sched_find(pid);
    if (!task || task->pid == 0 || task->kind != TASK_KIND_USER ||
        task->state == TASK_EXITED) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
        return -1;
    }
    if (set) {
        for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; ++cpu) fair_account_cpu(cpu, fair_now());
        if (priority < -20) priority = -20;
        if (priority > 19) priority = 19;
        task->priority = priority;
        if (task->fair.initialized)
            eevdf_reweight(&fair_queues[task->fair.cpu], &task->fair, priority);
    }
    int result = task->priority;
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return result;
}

/**
 * @brief Print one console line per task with pid, identity, name, state, and flags.
 */
void sched_dump(void)
{
    for (uint32_t i = 0; i < task_count; ++i) {
        console_debug("[reliefnt] task[%u] pid=%u ppid=%u uid=%u role=%u name=%s state=%u kind=%u flags=0x%x\n",
                       i,
                       tasks[i]->pid,
                       tasks[i]->parent_pid,
                       tasks[i]->uid,
                       tasks[i]->role,
                       tasks[i]->name,
                       tasks[i]->state,
                       tasks[i]->kind,
                       tasks[i]->flags);
    }
}
