#include "internal.h"



void sched_truncate_file_mappings(const struct storage_node *node, uint64_t size)
{
    /* The execution lock pins task/VMA lifetimes and excludes concurrent
     * faults, fork and exec. Duplicate CLONE_VM entries are harmless. */
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (task && task->kind == TASK_KIND_USER &&
            !(task->flags & TASK_FLAG_RESOURCES_RELEASED))
            syscall_mm_truncate_file(task, node, size);
    }
}

/**
 * @brief Attach an in-memory image and its length to the task with the given pid.
 */
void sched_set_task_image(uint32_t pid, const void *image, size_t image_len)
{
    struct task *task = sched_find(pid);
    if (!task) {
        return;
    }
    task->image = image;
    task->image_len = image_len;
}

/**
 * @brief Record the storage node backing a task's image and flag it for a lazy load.
 */
void sched_set_task_image_node(uint32_t pid, const struct storage_node *node)
{
    struct task *task = sched_find(pid);
    if (!task) {
        return;
    }
    task->image_node = node ? *node : (struct storage_node){0};
    task->flags |= TASK_FLAG_PENDING_LOAD;
}

/**
 * @brief Copy path into the task's fixed path field, resolving the task by pid.
 */
void sched_set_task_path(uint32_t pid, const char *path)
{
    task_copy_path(sched_find(pid), path);
}

/** @brief Copy exec metadata into exactly sized task-owned allocations.
 * @param task Destination task; existing metadata survives allocation failure.
 * @param argc Argument count. @param argv Pointers into data.
 * @param envc Environment count. @param envp Pointers into data.
 * @param data Packed NUL-terminated strings. @param data_len Valid packed bytes.
 * @return Zero, -EINVAL for invalid metadata, -E2BIG, or -ENOMEM.
 */
int sched_copy_task_exec_params(struct task *task,
                                uint32_t argc, char *const argv[],
                                uint32_t envc, char *const envp[],
                                const char *data, uint32_t data_len)
{
    if (!task || (data_len && !data)) return -22;
    uint64_t pointers = ((uint64_t)(argc ? argc : 1) + envc) * sizeof(char *);
    uint32_t limit = SCHED_EXEC_DATA_MAX;
    if (argc > SCHED_EXEC_ARG_MAX || envc > SCHED_EXEC_ENV_MAX || pointers >= limit ||
        data_len > limit - pointers) return -7;
    char **new_argv = kernel_malloc(((size_t)argc + 1) * sizeof(char *));
    char **new_envp = kernel_malloc(((size_t)envc + 1) * sizeof(char *));
    char *new_data = kernel_malloc(data_len ? data_len : 1);
    if (!new_argv || !new_envp || !new_data) {
        kernel_free(new_argv); kernel_free(new_envp); kernel_free(new_data);
        return -12;
    }
    if (data_len) __builtin_memcpy(new_data, data, data_len);
    for (unsigned v = 0; v < 2; ++v) {
        uint32_t count = v ? envc : argc;
        char *const *src = v ? envp : argv;
        char **dst = v ? new_envp : new_argv;
        for (uint32_t i = 0; i < count; ++i) {
            uintptr_t offset = src && src[i] ? (uintptr_t)src[i] - (uintptr_t)data : UINTPTR_MAX;
            if (offset >= data_len) {
                kernel_free(new_argv); kernel_free(new_envp); kernel_free(new_data);
                return -22;
            }
            dst[i] = new_data + offset;
        }
        dst[count] = NULL;
    }
    kernel_free(task->exec_argv); kernel_free(task->exec_envp); kernel_free(task->exec_data);
    task->exec_argv = new_argv; task->exec_envp = new_envp; task->exec_data = new_data;
    task->exec_argc = argc; task->exec_envc = envc; task->exec_data_len = data_len;
    return 0;
}

/** @brief Assign a task's next launch arguments using owned heap storage.
 * @param pid Destination task ID.
 * @param argc Argument count. @param argv Pointers into data.
 * @param envc Environment count. @param envp Pointers into data.
 * @param data Packed strings. @param data_len Number of valid bytes.
 * @return Zero or a negative validation/allocation error.
 */
int sched_set_task_exec_params(uint32_t pid, uint32_t argc, char *const argv[],
                                uint32_t envc, char *const envp[],
                                const char *data, uint32_t data_len)
{
    return sched_copy_task_exec_params(sched_find(pid), argc, argv, envc, envp, data, data_len);
}

int sched_prepare_exec_current(struct task *task)
{
    int result = syscall_unshare_task_files(task);
    if (result < 0) return result;
    if (task) {
        task->rseq_area = 0;
        task->rseq_len = 0;
        task->rseq_sig = 0;
    }
    uint32_t group = sched_task_tgid(task);
    struct task *leader = sched_find(group);
    if (leader) {
        /* Linux discards POSIX timers across execve. */
        for (uint32_t i = 0; i < SCHED_TASK_TIMER_MAX; ++i)
            leader->timers[i] = (struct task_posix_timer){0};
    }
    bool pending = false;
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *other = tasks[i];
        if (other == task || sched_task_tgid(other) != group) continue;
        sched_exit(other->pid, 0);
        if (other->running_cpu != SCHED_CPU_NONE) pending = true;
    }
    /* Remote members retire at their next kernel boundary before mm changes. */
    if (pending) return -11;
    storage_drain_task_io(task->pid);
    futex_task_exit(task);
    task->robust_list = 0;
    uint64_t signal_flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &signal_flags);
    kernel_signal_detach_process(task);
    kernel_spin_unlock_irqrestore(&scheduler_lock, signal_flags);
    if (leader) {
        task->alarm_deadline = leader->alarm_deadline;
        task->alarm_interval_ns = leader->alarm_interval_ns;
        task->alarm_last_expiry = leader->alarm_last_expiry;
    }
    if (leader && leader != task) {
        uint64_t flags;
        kernel_spin_lock_irqsave(&scheduler_lock, &flags);
        uint32_t old_tid = task->pid;
        task->parent_pid = leader->parent_pid;
        task->parent_exit_signal = leader->parent_exit_signal;
        task->parent_exit_notified = 0;
        task->flags |= leader->flags & TASK_FLAG_WAITABLE_CHILD;
        leader->flags &= ~TASK_FLAG_WAITABLE_CHILD;
        leader->process_pending_signals = 0;
        leader->alarm_deadline = 0;
        leader->alarm_interval_ns = leader->alarm_last_expiry = 0;
        leader->pid = old_tid;
        task->pid = group;
        for (uint32_t i = 0; i < task_count; ++i)
            if (tasks[i]->pdeath_parent_pid == old_tid) tasks[i]->pdeath_parent_pid = group;
        current_pid[scheduler_cpu_index()] = group;
        current_task[scheduler_cpu_index()] = task;
        kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    }
    if (task->shared_sighand) {
        struct task_sighand_state *old = task->shared_sighand;
        for (unsigned i = 0; i < KERNEL_SIGNAL_ACTION_MAX; ++i) task->signal_actions[i] = old->actions[i];
        task->shared_sighand = NULL;
        if (!--old->references) kernel_free(old);
    }
    if (task->shared_fs) {
        struct task_fs_state *old = task->shared_fs;
        task->shared_fs = NULL;
        task_copy_cwd(task, old->cwd);
        task_copy_identity_text(task->root_dir, sizeof(task->root_dir), old->root_dir);
        *sched_task_umask(task) = old->umask;
        if (!--old->references) kernel_free(old);
    }
    task->fs_base = 0;
    return 0;
}

void sched_exec_replace_mm(struct task *task, const struct address_space *replacement)
{
    /* A committed exec has replaced the user's address-space contract. Drop
     * any saved ioctl user pointers before releasing the previous mm. */
    drm_vmwgfx_cancel_task_continuation(task);
    struct task_address_space_state *old = task->shared_mm;
    /* exec_mmap() calls exec_mm_release() before the new mm is activated.
     * At this point userland_exec_current_node() cannot fail any more, so a
     * vfork parent must be released even though the child is still loading
     * the new image lazily.  A failing execve returns before this function
     * and therefore never wakes the parent early. */
    sched_vfork_child_done(task);
    paging_load_cr3(paging_kernel_cr3());
    if (!old || !--old->references) {
        address_space_destroy(sched_task_as(task));
        sched_task_vma_release(task);
        if (old) kernel_free(old);
    }
    task->shared_mm = NULL;
    task->address_space = (struct task_address_space_state){.as = *replacement};
}
