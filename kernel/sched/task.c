#include "internal.h"
#ifdef CONFIG_SWAP
/** @brief Cancel a task's optional pressure-refill state and its owned queue link.
 * @param task Execution-owned task, nullable; borrowed without ownership transfer.
 * No MM reference is retained. The native reclaim producer provides this hook;
 * standalone scheduler fixtures may omit it, so callers must check it first.
 */
extern void memory_headroom_cancel(struct task *) __attribute__((weak));
#endif
#include <reliefnt/lock_profile.h>
#include <reliefnt/userland.h>

/** @brief Invalidate OOM holder snapshots after final task resource release.
 * Production supplies the allocation-free notifier; standalone scheduler
 * fixtures may omit it. No additional lock or reference is taken.
 */
extern void sched_oom_membership_changed(void) __attribute__((weak));

static void (*memory_release_notify)(void);

/** @brief Register the built-in pressure waiter's retirement notification.
 * @param notify Stable callback, or NULL before publication. The callback
 * wakes waiters only; it never tears down the releasing task.
 */
void sched_set_memory_release_notify(void (*notify)(void))
{ __atomic_store_n(&memory_release_notify, notify, __ATOMIC_RELEASE); }

static uint64_t user_task_count_locked(uint32_t uid);

/** @brief Initialize Linux v6.12 native x86-64 resource defaults.
 * @param task Fresh task whose private limits state receives the defaults.
 */
static void task_init_limits(struct task *task);

/**
 * @brief Release private queued signals and shared signals only after the last thread retires.
 * @param task Exited, quiescent task under the kernel execution lock.
 */
static void sched_release_task_signals(struct task *task);


bool thread_group_pending(uint32_t tgid, const struct task *except)
{
    for (uint32_t i = 0; i < task_count; ++i) {
        const struct task *task = tasks[i];
        if (task && task != except && task->pid && sched_task_tgid(task) == tgid &&
            (task->state != TASK_EXITED || task->running_cpu != SCHED_CPU_NONE
#ifdef CONFIG_SWAP
             || !(task->flags & TASK_FLAG_RESOURCES_RELEASED)
#endif
             )) return true;
    }
    return false;
}

static uint64_t user_task_count_locked(uint32_t uid)
{
    uint64_t count = 0;
    for (uint32_t i = 0; i < task_count; ++i) {
        const struct task *task = tasks[i];
        if (task && task->pid && task->kind == TASK_KIND_USER && task->uid == uid &&
            (task->state != TASK_EXITED || task->running_cpu != SCHED_CPU_NONE ||
             ((task->flags & TASK_FLAG_WAITABLE_CHILD) && task->parent_pid))) ++count;
    }
    return count;
}

uint64_t sched_user_task_count(uint32_t uid)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    uint64_t count = user_task_count_locked(uid);
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return count;
}

bool task_nproc_denied_locked(const struct task *task)
{
    return task && task->uid &&
        !(task->cap_effective & ((1ULL << CAP_SYS_RESOURCE) | (1ULL << CAP_SYS_ADMIN))) &&
        user_task_count_locked(task->uid) >= sched_task_limits(task)->nproc.rlim_cur;
}

/**
 * @brief Clear every byte of a task struct so it starts from a well-defined zero state.
 */
void task_zero(struct task *task)
{
    if (!task) {
        return;
    }
    for (size_t i = 0; i < sizeof(*task); ++i) {
        ((uint8_t *)task)[i] = 0;
    }
    task_fair_bind(task);
}

/** @brief Drop the resource-limit reference only when a task is reaped/recycled. */
void task_release_limits(struct task *task)
{
    struct task_rlimit_state *shared = task->shared_limits;
    if (!shared) return;
    task->limits = *shared;
    task->limits.references = 1;
    task->shared_limits = NULL;
    if (!--shared->references) kernel_free(shared);
}

/**
 * @brief Initialize native resource defaults without allocating shared limit state.
 * @param task Fresh task whose private limits receive memory-scaled defaults.
 */
static void task_init_limits(struct task *task)
{
    /* Linux's non-KASAN x86-64 default: half of RAM / (16 KiB * 8),
     * with the same [20, FUTEX_TID_MASK] thread-count clamp. */
    uint64_t threads = mm_total_memory_kib() / 128;
    if (threads < 20) threads = 20;
    if (threads > 0x3fffffff) threads = 0x3fffffff;
    task->limits = (struct task_rlimit_state){.references = 1,
        .nofile = {SCHED_TASK_NOFILE_DEFAULT, SCHED_NR_OPEN},
        .sigpending = {threads / 2, threads / 2},
        .nproc = {threads / 2, threads / 2},
        /* INIT_RLIMITS includes CORE even when CONFIG_COREDUMP is disabled. */
        .core = {0, LINUX_RLIM_INFINITY},
        /* Linux v6.14 resource.h MLOCK_LIMIT / INIT_RLIMITS. */
        .memlock = {8ULL * 1024ULL * 1024ULL, 8ULL * 1024ULL * 1024ULL},
        .as = {LINUX_RLIM_INFINITY, LINUX_RLIM_INFINITY},
        /* include/uapi/linux/resource.h:_STK_LIM and INIT_RLIMITS. */
        .stack = {8ULL * 1024ULL * 1024ULL, LINUX_RLIM_INFINITY}};
}

/**
 * @brief Scan forward from next_pid for an unused non-zero pid; 0 when the identifier space is exhausted.
 */
uint32_t task_allocate_pid(void)
{
    for (uint64_t attempt = 0; attempt < UINT32_MAX; ++attempt) {
        uint32_t candidate = next_pid++;
        uint32_t used = 0;
        if (candidate == 0) {
            continue;
        }
        for (uint32_t i = 0; i < task_count; ++i) {
            if (tasks[i] && tasks[i]->pid == candidate) {
                used = 1;
                break;
            }
        }
        if (!used) {
            proc_last_pid = candidate;
            return candidate;
        }
    }
    return 0;
}

/**
 * @brief Recycle a fully retired task or allocate a stable zeroed task record.
 * @return Borrowed unpublished task slot, or NULL on allocation/size failure.
 * Caller holds scheduler_lock. Retires old PID identities before reuse so held
 * proc inodes cannot retarget; releases no filesystem or device resources here.
 */
struct task *alloc_task_slot(void)
{
    for (uint32_t i = 0; i < task_count; ++i) {
        if (tasks[i] && tasks[i]->state == TASK_EXITED &&
            tasks[i]->running_cpu == SCHED_CPU_NONE &&
            (tasks[i]->flags & TASK_FLAG_RESOURCES_RELEASED) &&
            !thread_group_pending(sched_task_tgid(tasks[i]), tasks[i]) &&
            (!(tasks[i]->flags & TASK_FLAG_WAITABLE_CHILD) || tasks[i]->parent_pid == 0)) {
            task_release_limits(tasks[i]);
            fair_forget(tasks[i]);
            sched_task_identity_detach(tasks[i]);
            struct sched_task_object *object = (struct sched_task_object *)tasks[i];
            object->futex_exit = (struct futex_exit_cursor){0};
            object->exec_prepared = NULL;
            object->socket_stage = NULL;
            object->oom_adjustment = object->oom_minimum = 0;
            object->oom_victim_epoch = 0;
            object->pressure_address = object->pressure_free = 0;
            object->pressure_stall_tick = 0;
            object->pressure_cause = object->pressure_stalls = 0;
            object->pressure_fault_cr3 = object->pressure_fault_rip = 0;
            object->pressure_fault_pages[0] = object->pressure_fault_pages[1] = 0;
            object->pressure_fault_pages[2] = object->pressure_fault_pages[3] = 0;
            object->pressure_fault_cursor[0] = object->pressure_fault_cursor[1] = (struct pressure_cursor){0};
#ifdef CONFIG_SWAP
            object->pressure_headroom = (struct pressure_headroom_state){0};
            object->fork_cow_reclaim = (struct fork_cow_reclaim_state){0};
#endif
            object->event_fault_active = false;
            task_zero(tasks[i]);
            return tasks[i];
        }
    }

    if (task_count == task_capacity) {
        uint32_t new_capacity = task_capacity ? task_capacity * 2u : SCHED_TASK_MAX;
        struct task **replacement;
        if (new_capacity < task_capacity || new_capacity > UINT32_MAX / sizeof(*tasks)) {

            return NULL;
        }
        replacement = (struct task **)kernel_malloc(
            (size_t)new_capacity * sizeof(*replacement));
        if (!replacement) {

            return NULL;
        }
        for (uint32_t i = 0; i < new_capacity; ++i) {
            replacement[i] = i < task_count ? tasks[i] : NULL;
        }
        if (tasks) {
            kernel_free(tasks);
        }
        tasks = replacement;
        task_capacity = new_capacity;
    }

    struct sched_task_object *object =
        (struct sched_task_object *)kernel_malloc(sizeof(*object));
    if (!object) {

        return NULL;
    }
    tasks[task_count] = &object->task;
    object->futex_exit = (struct futex_exit_cursor){0};
    object->exec_prepared = NULL;
    object->socket_stage = NULL;
    object->oom_adjustment = object->oom_minimum = 0;
    object->oom_victim_epoch = 0;
    object->pressure_address = object->pressure_free = 0;
    object->pressure_stall_tick = 0;
    object->pressure_cause = object->pressure_stalls = 0;
    object->pressure_fault_cr3 = object->pressure_fault_rip = 0;
    object->pressure_fault_pages[0] = object->pressure_fault_pages[1] = 0;
    object->pressure_fault_pages[2] = object->pressure_fault_pages[3] = 0;
    object->pressure_fault_cursor[0] = object->pressure_fault_cursor[1] = (struct pressure_cursor){0};
#ifdef CONFIG_SWAP
    object->fork_cow_reclaim = (struct fork_cow_reclaim_state){0};
#endif
    object->event_fault_active = false;
    task_zero(tasks[task_count]);
    return tasks[task_count++];
}

/** @brief Borrow the unreleased task's preallocated robust-exit cursor.
 * @param task Task object, nullable. @return Private cursor or NULL.
 */
struct futex_exit_cursor *sched_futex_exit_cursor(struct task *task)
{
    return task ? &((struct sched_task_object *)task)->futex_exit : NULL;
}

/** @brief Transfer a prepared image into an armed task continuation.
 * @param task Execution-owned task. @param prepared Owned image.
 * @return True on transfer; false leaves ownership with the caller.
 */
bool sched_exec_stage(struct task *task, struct task *prepared)
{
    if (!task || !prepared || !task->lookup_scope.armed) return false;
    struct sched_task_object *object = (struct sched_task_object *)task;
    if (object->exec_prepared) return false;
    object->exec_prepared = prepared;
    return true;
}

/** @brief Borrow a task's committed prepared image without detaching it.
 * @param task Owner, nullable. @return Prepared image or NULL.
 */
struct task *sched_exec_staged(const struct task *task)
{ return task ? ((const struct sched_task_object *)task)->exec_prepared : NULL; }

/** @brief Detach a prepared image for one commit or cancellation.
 * @param task Owner, nullable. @return Transferred image or NULL.
 */
struct task *sched_exec_take(struct task *task)
{
    if (!task) return NULL;
    struct sched_task_object *object = (struct sched_task_object *)task;
    struct task *prepared = object->exec_prepared;
    object->exec_prepared = NULL;
    return prepared;
}

/** @brief Borrow a task-owned socket stage. @param task Owner, nullable.
 * @return Current stage or NULL; no ownership transfer.
 */
struct socket_send_stage *sched_socket_stage(const struct task *task)
{ return task ? ((const struct sched_task_object *)task)->socket_stage : NULL; }

/** @brief Install a socket stage only into an empty task slot.
 * @param task Owner. @param stage Caller-owned stage. @return True on transfer.
 */
bool sched_socket_stage_set(struct task *task, struct socket_send_stage *stage)
{
    if (!task || !stage) return false;
    struct sched_task_object *object=(struct sched_task_object *)task;
    if (object->socket_stage) return false;
    object->socket_stage=stage;return true;
}

/** @brief Detach socket stage ownership for completion or cancellation.
 * @param task Owner, nullable. @return Transferred stage or NULL.
 */
struct socket_send_stage *sched_socket_stage_take(struct task *task)
{
    if (!task) return NULL;
    struct sched_task_object *object=(struct sched_task_object *)task;
    struct socket_send_stage *stage=object->socket_stage;
    object->socket_stage=NULL;return stage;
}

/**
 * @brief Allocate and initialize a kernel task with an entry point, no stack, and cleared identity.
 */
uint32_t sched_create_kernel_task(const char *name, uint64_t entry)
{
    uint64_t lock_flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &lock_flags);
    struct task *task = alloc_task_slot();
    uint32_t pid;
    if (!task) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        return 0;
    }
    pid = task_allocate_pid();
    if (!pid) {
        task_zero(task);
        task->state = TASK_EXITED;
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        return 0;
    }
    task_zero(task);
    task->pid = pid;
    task->start_uptime_ms = time_uptime_ms();
    task->parent_pid = 0;
    task->process_group = 0;
    task->process_session = 0;
    task_copy_name(task, name);
    task->entry = entry;
    task->stack_top = 0;
    task->stack_low = 0;
    task->wake_tick = 0;
    task->priority = 0;
    task->affinity_mask = sched_all_cpu_mask();
    task->pending_signals = 0;
    task->timer_pending_signals = 0;
    task_init_limits(task);
    (*sched_task_umask(task)) = 022;
    task->exit_code = 0;
    task->image = NULL;
    task->image_len = 0;
    for (size_t i = 0; i < sizeof((*sched_task_as(task))); ++i) {
        ((uint8_t *)sched_task_as(task))[i] = 0;
    }
    for (size_t i = 0; i < sizeof(task->frame); ++i) {
        ((uint8_t *)&task->frame)[i] = 0;
    }
    task->state = TASK_READY;
    task->running_cpu = SCHED_CPU_NONE;
    task->kind = TASK_KIND_KERNEL;
    task->flags = 0;
    task->pty_id = 0;
    task_clear_identity(task);
    task_copy_cwd(task, "/");
    sched_request_reschedule_all_online();
    console_debug("[reliefnt] task pid=%u name=%s entry=0x%llx\n",
                   task->pid, task->name, (unsigned long long)task->entry);
    ++proc_created;
    kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
    return task->pid;
}

/**
 * @brief Allocate a user task, build its address space and stack, and inherit cwd/identity from parent.
 * @param name Borrowed NUL-terminated task name. @param entry User entry address.
 * @param stack_top Initial exclusive stack top. @param parent_pid Inheriting parent.
 * @param flags Initial task flags. @return Published PID, or zero on failure.
 * Prepares PID identity before scheduler locking and retains root state for the task.
 */
uint32_t sched_create_user_task(const char *name, uint64_t entry, uint64_t stack_top,
                                uint32_t parent_pid, uint32_t flags)
{
    struct task_identity *identity = sched_task_identity_prepare();
    if (!identity) return 0;
    uint64_t lock_flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &lock_flags);
    if (task_nproc_denied_locked(sched_find(parent_pid))) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        sched_task_identity_put(identity);
        return 0;
    }
    struct task *task = alloc_task_slot();
    uint32_t pid;
    if (!task) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        sched_task_identity_put(identity);
        return 0;
    }
    pid = task_allocate_pid();
    if (!pid) {
        task_zero(task);
        task->state = TASK_EXITED;
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        sched_task_identity_put(identity);
        return 0;
    }
    task_zero(task);
    task->pid = pid;
    task->start_uptime_ms = time_uptime_ms();
    task->parent_pid = parent_pid;
    task->pdeath_parent_pid = parent_pid;
    task->parent_exit_signal = 17;
    task->process_group = task->pid;
    task->process_session = task->pid;
    task_copy_name(task, name);
    task->entry = entry;
    task->stack_top = stack_top;
    task->stack_low = stack_top >= (uint64_t)RELIEFNT_USER_STACK_PAGES * 4096ULL ?
                      stack_top - (uint64_t)RELIEFNT_USER_STACK_PAGES * 4096ULL : 0;
    task->wake_tick = 0;
    task->priority = 0;
    task->affinity_mask = sched_all_cpu_mask();
    task->pending_signals = 0;
    task->timer_pending_signals = 0;
    task_init_limits(task);
    (*sched_task_umask(task)) = 022;
    task->exit_code = 0;
    task->image = NULL;
    task->image_len = 0;
    if (!address_space_create(sched_task_as(task)) ||
        !address_space_map_user_stack(sched_task_as(task), stack_top)) {
        address_space_destroy(sched_task_as(task));
        task_copy_name(task, "failed");
        task->entry = 0;
        task->stack_top = 0;
        task->image = NULL;
        task->image_len = 0;
        task->state = TASK_EXITED;
        task->flags = TASK_FLAG_RESOURCES_RELEASED;
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        sched_task_identity_put(identity);
        return 0;
    }
    task->frame.rip = entry;
    task->frame.cs = RELIEFNT_USER_CS;
    task->frame.rflags = 0x202;
    task->frame.rsp = stack_top;
    task->frame.ss = RELIEFNT_USER_DS;
    arch_fpu_task_init(task->fpu_state);
    /* Keep a newly allocated user task off the run queue until its creator
     * has attached the executable metadata and launch arguments. */
    task->state = TASK_BLOCKED;
    task->running_cpu = SCHED_CPU_NONE;
    task->kind = TASK_KIND_USER;
    task->flags = flags;
    sched_task_identity_bind(task, identity);
    task_clear_identity(task);
    task_copy_cwd(task, "/");
    if (!parent_pid) {
        /* Initial user task starts with Linux's initial root capability set. */
        task->cap_permitted = task->cap_effective = (UINT64_C(1) << (CAP_LAST_CAP + 1)) - 1;
    }
    if (parent_pid) {
        struct task *parent = sched_find(parent_pid);
        if (parent) {
            sched_oom_policy_inherit(task, parent);
            task_copy_cwd(task, sched_task_cwd(parent));
            task_copy_identity_from_parent(task, parent);
            task->priority = parent->priority;
            task->limits = *sched_task_limits(parent);
            task->limits.references = 1;
            (*sched_task_umask(task)) = (*sched_task_umask(parent));
            task->process_group = parent->process_group;
            task->process_session = parent->process_session;
            task->controlling_pty_id = parent->controlling_pty_id;
            task->affinity_mask = parent->affinity_mask;
        }
    }
    console_debug("[reliefnt] task pid=%u ppid=%u name=%s user entry=0x%llx stack=0x%llx flags=0x%x\n",
                   task->pid,
                   task->parent_pid,
                   task->name,
                   (unsigned long long)task->entry,
                   (unsigned long long)task->stack_top,
                   task->flags);
    ++proc_created;
    kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
    /* The task is still unpublished to its caller; prepare the root identity
     * after dropping scheduler_lock. Reclaim can use its reserve fallback if
     * the ordinary heap is already exhausted here. */
    uint64_t generation;
    (void)mm_context_ensure_generation(sched_task_as(task), &generation);
    return task->pid;
}

/**
 * @brief Publish terminal task state and log its first exit after releasing the scheduler lock.
 * @param pid Task identifier; PID 1 panics and an absent PID has no exit event.
 * @param code Exit status; repeated calls retain the existing status-update and cleanup semantics.
 * The caller must not hold scheduler_lock. Capture a bounded diagnostic under
 * that lock; defer resource release until the CPU reservation is quiescent.
 */
void sched_exit(uint32_t pid, uint64_t code)
{
    uint64_t flags;
    bool gpu_owner_quiescent = false;
    bool adopted_zombie = false;
    bool exit_event = false;
    uint32_t exit_pid = 0;
    uint64_t exit_code = 0;
    char exit_name[SCHED_TASK_NAME_LEN];
    struct task *exiting = NULL;
    if (pid == 1) bugcheck_panic("Attempted to exit PID 1");

    /* Publish the terminal state atomically, but preserve running_cpu until
     * its owner reaches a scheduling boundary.  A remote CPU may still be
     * executing this task's user frame when a terminal/PTY destroys it. */
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    for (uint32_t i = 0; i < task_count; ++i) {
        if (tasks[i]->pid == pid) {
            exiting = tasks[i];
            exit_event = tasks[i]->state != TASK_EXITED;
            if (exit_event) {
                exit_pid = tasks[i]->pid;
                exit_code = code;
                task_copy_identity_text(exit_name, sizeof(exit_name), tasks[i]->name);
            }
            tasks[i]->state = TASK_EXITED;
            tasks[i]->exit_code = code;
            tasks[i]->child_event = TASK_CHILD_EVENT_NONE;
            tasks[i]->stop_signal = 0;
            gpu_owner_quiescent = tasks[i]->running_cpu == SCHED_CPU_NONE;
            break;
        }
    }
    if (exiting && !thread_group_pending(sched_task_tgid(exiting), exiting)) {
        const struct task *parent = sched_find(exiting->parent_pid);
        if (parent && parent->process_group != exiting->process_group &&
            parent->process_session == exiting->process_session)
            sched_mark_orphaned_group(exiting->process_group);
    }
    for (uint32_t i = 0; i < task_count; ++i) {
        if (exiting && !thread_group_pending(sched_task_tgid(exiting), exiting) &&
            tasks[i]->parent_pid == sched_task_tgid(exiting)) {
            if (tasks[i]->process_session == exiting->process_session &&
                tasks[i]->process_group != exiting->process_group)
                sched_mark_orphaned_group(tasks[i]->process_group);
            struct task *reaper = sched_find(1);
            if (reaper && reaper != exiting && reaper->state != TASK_EXITED) {
                tasks[i]->parent_pid = 1;
                /* Thread siblings follow the parent relationship, but only
                 * process leaders become waitable children of PID 1. */
                if (tasks[i]->pid == sched_task_tgid(tasks[i])) {
                    tasks[i]->parent_exit_signal = 17;
                    tasks[i]->parent_exit_notified = 0;
                    tasks[i]->flags |= TASK_FLAG_WAITABLE_CHILD;
                    if (tasks[i]->state == TASK_EXITED &&
                        tasks[i]->running_cpu == SCHED_CPU_NONE &&
                        !thread_group_pending(tasks[i]->pid, tasks[i])) adopted_zombie = true;
                }
            } else {
                tasks[i]->parent_pid = 0;
                tasks[i]->flags &= ~TASK_FLAG_WAITABLE_CHILD;
            }
        }
    }
    if (exiting && exit_event) {
        /* Linux forget_original_parent reparents each real-parent child after
         * arming its parent-death signal, so a later prctl() can never pair a
         * signal with a parent that has already terminated. */
        struct task *reaper = sched_find(1);
        uint32_t reaper_pid = (reaper && reaper != exiting &&
                               reaper->state != TASK_EXITED) ? 1 : 0;
        for (uint32_t i = 0; i < task_count; ++i) {
            if (tasks[i]->pdeath_parent_pid != exit_pid) continue;
            if (tasks[i]->pdeath_signal) tasks[i]->flags |= TASK_FLAG_PDEATH_NOTIFY;
            tasks[i]->pdeath_parent_pid = reaper_pid;
        }
    }
    if (exiting) {
        /* Detach a dying parent before its task slot can be reused. */
        vfork_parent_exit_locked(exiting);
        /* A remote child may still execute its user frame. Complete only
         * after retirement and the mm_release futex work below. */
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    if (exit_event) {
        console_debug("[reliefnt] scheduler task exited pid=%u name=%s code=%llu\n",
                       exit_pid,
                       exit_name,
                       (unsigned long long)exit_code);
    }
    if (adopted_zombie) {
        struct task *reaper = sched_find(1);
        if (reaper && reaper->state != TASK_EXITED) sched_signal_user_process(1, 17);
    }
    /* Signal delivery takes scheduler locks itself and may terminate a group. */
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (task && (task->flags & TASK_FLAG_ORPHAN_NOTIFY)) {
            task->flags &= ~TASK_FLAG_ORPHAN_NOTIFY;
            uint32_t group = task->process_group;
            sched_signal_kernel_group(group, 1);
            sched_signal_kernel_group(group, 18);
        }
        if (task && (task->flags & TASK_FLAG_PDEATH_NOTIFY)) {
            /* Linux group_send_sig_info(pdeath_signal, ..., PIDTYPE_TGID). */
            task->flags &= ~TASK_FLAG_PDEATH_NOTIFY;
            if (task->pdeath_signal)
                sched_signal_user_process(sched_task_tgid(task), (int)task->pdeath_signal);
        }
    }
    if (gpu_owner_quiescent) {
        uint64_t execution_flags;
        kernel_execution_lock_scoped_irqsave(&execution_flags, LOCK_SCOPE_SCHED);
        sched_release_task_resources(exiting);
        kernel_execution_unlock_irqrestore(execution_flags);
    }
}

/**
 * @brief Release private queued signals and shared signals only after the last thread retires.
 * @param task Exited, quiescent task under the kernel execution lock.
 */
static void sched_release_task_signals(struct task *task)
{
    kernel_signal_flush(task, false, UINT64_MAX);
    if (!thread_group_pending(sched_task_tgid(task), task))
        kernel_signal_flush(task, true, UINT64_MAX);
}

/**
 * @brief Release a quiescent user's owned resources once, including root/cwd.
 * @param task Non-running task under execution ownership, or NULL.
 * Drains pending I/O before releasing OFDs, mappings and filesystem paths;
 * CLONE_FS paths retire only at the final shared holder. Backend cleanup can
 * perform I/O, so the caller must not hold scheduler_lock across this call.
 * Successful release invalidates bounded OOM holder eligibility snapshots.
 */
void sched_release_task_resources(struct task *task)
{
    if (!task || task->kind != TASK_KIND_USER ||
        task->running_cpu != SCHED_CPU_NONE ||
        (task->flags & TASK_FLAG_RESOURCES_RELEASED)) {
        return;
    }
#ifdef CONFIG_SWAP
    /* Recursive exit callers cannot perform a page transaction. The task slot
     * and its shared MM remain owned until an outer scheduler poll finishes. */
    if (!kernel_execution_is_outermost()) return;
    ((struct sched_task_object *)task)->fork_cow_reclaim = (struct fork_cow_reclaim_state){0};
#endif
    storage_drain_task_io(task->pid);
#ifdef CONFIG_SWAP
    if(memory_headroom_cancel)memory_headroom_cancel(task);
    userland_exec_cancel(task);
    int futex_result = futex_task_exit_step(task);
    if (futex_result != 1) return;
#endif
    syscall_record_lock_cancel(task);
    if (task->waiting_queue) kernel_wait_queue_remove(task->waiting_queue, task);
    task_socket_cancel_receive(task);
    task_release_syscall_file(task);
    task_sysv_sem_exit(task);
#ifndef CONFIG_SWAP
    futex_task_exit(task);
#endif
    /* Linux mm_release clears child_tid and releases robust futexes before
     * vfork completion. This task can no longer touch the parent's memory. */
    if (task->vfork_parent || task->vfork_child) {
        uint64_t vfork_flags;
        kernel_spin_lock_irqsave(&scheduler_lock, &vfork_flags);
        vfork_parent_exit_locked(task);
        vfork_child_done_locked(task);
        kernel_spin_unlock_irqrestore(&scheduler_lock, vfork_flags);
    }
    sched_release_task_signals(task);
    if (!thread_group_pending(sched_task_tgid(task), task))
        pty_process_session_exit(sched_task_tgid(task));
    bool last_files = !task->shared_files ||
        __atomic_sub_fetch(&task->shared_files->references, 1, __ATOMIC_SEQ_CST) == 0;
    if (last_files) {
        syscall_release_task_files(task);
        if (task->shared_files) kernel_free(task->shared_files);
    }
    task->shared_files = NULL;
    task_groups_release(task);
    if (!task->shared_mm || __atomic_sub_fetch(&task->shared_mm->references, 1, __ATOMIC_SEQ_CST) == 0) {
        address_space_destroy(sched_task_as(task));
        sched_task_vma_release(task);
        if (task->shared_mm) kernel_free(task->shared_mm);
    }
    task->shared_mm = NULL;
    task->address_space = (struct task_address_space_state){0};
    task->fd_table = (struct task_fd_table_state){0};
    sched_task_fs_release(task);
    if (task->shared_sighand && __atomic_sub_fetch(&task->shared_sighand->references, 1, __ATOMIC_SEQ_CST) == 0)
        kernel_free(task->shared_sighand);
    task->shared_sighand = NULL;
    kernel_free(task->exec_argv); kernel_free(task->exec_envp); kernel_free(task->exec_data);
    task->exec_argv = task->exec_envp = NULL;
    task->exec_data = NULL;
    task->exec_argc = task->exec_envc = task->exec_data_len = 0;
    svga_gpu_release_owner(task->pid);
    task->flags |= TASK_FLAG_RESOURCES_RELEASED;
    if (sched_oom_membership_changed) sched_oom_membership_changed();
    void (*notify)(void) = __atomic_load_n(&memory_release_notify, __ATOMIC_ACQUIRE);
    if (notify) notify();
    /* Signal/fault exits also retire private PTY owners. Publish cleanup first:
     * hangup can recursively terminate other members of the foreground group. */
    if (last_files) pty_process_exit(task->pid);
    sched_notify_parent_exit(task);
}

/** @brief Advance a bounded number of quiescent deferred task cleanups.
 * @param budget Maximum task slots examined, zero a no-op. The unreleased
 * task owns its MM until completion; callers provide outer writer ownership.
 */
void sched_cleanup_poll(uint32_t budget)
{
#ifdef CONFIG_SWAP
    static uint32_t cursor;
    if (!kernel_execution_is_outermost() || !task_count) return;
    if (budget > task_count) budget = task_count;
    while (budget--) {
        if (cursor >= task_count) cursor = 0;
        struct task *task = tasks[cursor++];
        if (task && task->kind == TASK_KIND_USER && task->state == TASK_EXITED &&
            task->running_cpu == SCHED_CPU_NONE &&
            !(task->flags & (TASK_FLAG_RESOURCES_RELEASED | TASK_FLAG_REAP_IN_PROGRESS))) {
            sched_release_task_resources(task);
            break;
        }
    }
#else
    (void)budget;
#endif
}

void sched_exit_group(uint32_t tgid, uint64_t code)
{
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (task && sched_task_tgid(task) == tgid && task->state != TASK_EXITED)
            sched_exit(task->pid, code);
        else if (task && task->pid == tgid) task->exit_code = code;
    }
}
