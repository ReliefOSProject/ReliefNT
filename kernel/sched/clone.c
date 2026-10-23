#include "internal.h"

/**
 * @brief Fork the calling user task: clone its address space COW-style, copy its register frame with rax zeroed in the child, and copy descriptors; returns the child pid or a negative errno.
 */
static int task_promote_shared(struct task *task, uint64_t flags);


/**
 * @brief Fork the calling user task: clone its address space COW-style, copy its register frame with rax zeroed in the child, and copy descriptors; returns the child pid or a negative errno.
 */
static int task_promote_shared(struct task *task, uint64_t flags)
{
    if ((flags & CLONE_THREAD) && !task->shared_limits) {
        struct task_rlimit_state *limits = kernel_malloc(sizeof(*limits));
        if (!limits) return -12;
        *limits = task->limits;
        limits->references = 1;
        task->shared_limits = limits;
    }
    if ((flags & CLONE_VM) && !task->shared_mm) {
        struct task_address_space_state *mm = kernel_malloc(sizeof(*mm));
        if (!mm) return -12;
        *mm = task->address_space;
        mm->references = 1;
        if (!mm->initial_stack_top) mm->initial_stack_top = task->stack_top;
        if (!mm->initial_stack_low) mm->initial_stack_low = task->stack_low;
        task->shared_mm = mm;
    }
    if ((flags & CLONE_FILES) && !task->shared_files) {
        struct task_fd_table_state *files = kernel_malloc(sizeof(*files));
        if (!files) return -12;
        *files = task->fd_table;
        files->references = 1;
        task->shared_files = files;
    }
    if ((flags & CLONE_FS) && !task->shared_fs) {
        struct task_fs_state *fs = kernel_malloc(sizeof(*fs));
        if (!fs) return -12;
        fs->umask = *sched_task_umask(task);
        for (unsigned i = 0; i < sizeof(fs->cwd); ++i) fs->cwd[i] = sched_task_cwd(task)[i];
        task_copy_identity_text(fs->root_dir, sizeof(fs->root_dir), sched_task_root(task));
        fs->references = 1;
        task->shared_fs = fs;
    }
    if ((flags & CLONE_SIGHAND) && !task->shared_sighand) {
        struct task_sighand_state *handlers = kernel_malloc(sizeof(*handlers));
        if (!handlers) return -12;
        for (unsigned i = 0; i < KERNEL_SIGNAL_ACTION_MAX; ++i)
            handlers->actions[i] = task->signal_actions[i];
        handlers->references = 1;
        task->shared_sighand = handlers;
    }
    return 0;
}

/**
 * @brief Finish a child-side CLONE_VFORK wait under scheduler_lock.
 *
 * Linux completes tsk->vfork_done from mm_release() on both the exec and the
 * final exit path.  The parent is woken only from TASK_BLOCKED; a parent that
 * was concurrently killed or stopped keeps its terminal/job-control state and
 * simply loses the stale child pointer.
 */
void vfork_child_done_locked(struct task *child)
{
    struct task *parent = child ? child->vfork_parent : NULL;
    if (!parent) return;
    child->vfork_parent = NULL;
    if (parent->vfork_child == child) parent->vfork_child = NULL;
    if (parent->state == TASK_BLOCKED) {
        parent->wake_tick = 0;
        parent->state = TASK_READY;
        sched_request_reschedule_all_online();
    }
}

/** @brief Drop a dying parent's reference to its vfork child under lock. */
void vfork_parent_exit_locked(struct task *parent)
{
    struct task *child = parent ? parent->vfork_child : NULL;
    if (!child) return;
    child->vfork_parent = NULL;
    parent->vfork_child = NULL;
}

/**
 * @brief Public non-locking wrapper used by exec/exit lifecycle code.
 *
 * Called with the scheduler unlocked after the child is committed to exec or
 * terminally exiting.  Keep the link cleared even when the parent already
 * died: a later task slot reuse must never observe a stale parent pointer.
 */
void sched_vfork_child_done(struct task *child)
{
    if (!child) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    vfork_child_done_locked(child);
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
}

/**
 * @brief Create a child task and clone or share the caller's kernel-managed state.
 * @param parent_frame Saved user register frame to copy into the child.
 * @param flags Supported Linux clone flags and optional exit signal.
 * @param stack New user stack pointer, or zero to inherit the caller's stack.
 * @param parent_tid Optional user address receiving the child ID in the parent.
 * @param child_tid Optional user address receiving the child ID in the child.
 * @param tls User TLS base when CLONE_SETTLS is requested.
 * @return Child process ID, or a negative errno if creation fails.
 */
int64_t sched_clone_current(const struct trap_frame *parent_frame, uint64_t flags,
                           uint64_t stack, uint64_t parent_tid,
                           uint64_t child_tid, uint64_t tls)
{
    const uint64_t supported = CSIGNAL | CLONE_VM | CLONE_VFORK | CLONE_FS |
        CLONE_FILES | CLONE_SIGHAND | CLONE_PARENT | CLONE_THREAD | CLONE_SYSVSEM |
        CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID |
        CLONE_DETACHED | CLONE_UNTRACED | CLONE_CHILD_SETTID | CLONE_CLEAR_SIGHAND;
    if ((flags & CLONE_CLEAR_SIGHAND) && (flags & CLONE_SIGHAND)) return -22;
    if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND)) return -22;
    if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM)) return -22;
    if (flags & ~supported) return -RELIEFOS_ENOSYS;
    if ((flags & CLONE_SETTLS) && tls >= RELIEFNT_USER_TLS_LIMIT) return -RELIEFOS_EPERM;
    if ((flags & CLONE_PARENT_SETTID) && !user_range_writable(parent_tid, 4)) return -14;
    if ((flags & CLONE_CHILD_SETTID) && !user_range_writable(child_tid, 4)) return -14;
    if (stack && (stack < RELIEFNT_USER_BASE || stack >= RELIEFNT_USER_TOP)) return -14;
    uint64_t lock_flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &lock_flags);
    struct task *parent = sched_current_task();
    struct task *child;
    uint32_t child_pid;
    bool files_retained = false;
    if (!parent || !parent_frame || parent->kind != TASK_KIND_USER ||
        parent->state == TASK_EXITED || !(*sched_task_as(parent)).cr3) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        return -22;
    }
    if (task_nproc_denied_locked(parent)) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        return -LINUX_EAGAIN;
    }
    parent->nproc_exceeded = false;
    if (task_promote_shared(parent, flags) < 0) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        return -12;
    }
    child = alloc_task_slot();
    if (!child) {
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        return -12;
    }
    child_pid = task_allocate_pid();
    if (!child_pid) {
        task_zero(child);
        child->state = TASK_EXITED;
        kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
        return -12;
    }
    task_zero(child);
    *child = *parent;
    child->exec_argv = NULL;
    child->exec_envp = NULL;
    child->exec_data = NULL;
    task_fair_reset(child);
    child->limits = *sched_task_limits(parent);
    child->limits.references = 1;
    child->shared_limits = NULL;
    child->shared_mm = NULL;
    child->vfork_parent = NULL;
    child->vfork_child = NULL;
    child->shared_files = NULL;
    child->shared_fs = NULL;
    child->shared_sighand = NULL;
    child->address_space = *sched_task_mm(parent);
    child->address_space.as = (struct address_space){0};
    child->address_space.vma_extra = NULL;
    child->address_space.vma_extra_count = 0;
    child->address_space.vma_extra_capacity = 0;
    child->address_space.executable_inode = NULL;
    for (uint32_t i = 0; i < SCHED_TASK_VMA_MAX; ++i) {
        child->address_space.vmas[i].inode = NULL;
        child->address_space.vmas[i].audio_pcm = NULL;
        child->address_space.vmas[i].sysv_shm_attachment = NULL;
    }
    child->fd_table = *sched_task_fds(parent);
    child->fd_table.lock_owner = 0;
    child->fd_table.file_extra = NULL;
    child->fd_table.file_extra_count = 0;
    child->fd_table.file_extra_capacity = 0;
    child->fd_table.pty_fds = NULL;
    child->fd_table.pty_fd_count = 0;
    child->fd_table.pty_fd_capacity = 0;
    task_copy_cwd(child, sched_task_cwd(parent));
    task_copy_identity_text(child->root_dir, sizeof(child->root_dir), sched_task_root(parent));
    *sched_task_umask(child) = *sched_task_umask(parent);
    for (unsigned i = 0; i < KERNEL_SIGNAL_ACTION_MAX; ++i)
        child->signal_actions[i] = sched_task_actions(parent)[i];
    if (flags & CLONE_CLEAR_SIGHAND) {
        for (unsigned i = 1; i < KERNEL_SIGNAL_ACTION_MAX; ++i) {
            if (child->signal_actions[i].handler != 1)
                child->signal_actions[i] = (struct kernel_signal_action){0};
        }
    }
    child->pid = child_pid;
    child->start_uptime_ms = time_uptime_ms();
    child->tgid = (flags & CLONE_THREAD) ? sched_task_tgid(parent) : child_pid;
    child->process_pending_signals = 0;
    child->signal_queue = (struct kernel_sigqueue){0};
    child->process_signal_queue = (struct kernel_sigqueue){0};
    child->shared_process_signal_queue = flags & CLONE_THREAD ?
        sched_task_process_signal_queue(parent) : NULL;
    child->alarm_deadline = 0;
    child->alarm_interval_ns = child->alarm_last_expiry = 0;
    /* The leader's task storage survives until the last group member exits. */
    child->shared_process_pending = flags & CLONE_THREAD ? sched_task_process_pending(parent) : NULL;
    child->parent_pid = flags & (CLONE_THREAD | CLONE_PARENT)
        ? parent->parent_pid : sched_task_tgid(parent);
    child->parent_exit_signal = flags & CLONE_THREAD ? 0 : flags & CLONE_PARENT
        ? sched_find(sched_task_tgid(parent))->parent_exit_signal : flags & CSIGNAL;
    child->parent_exit_notified = 0;
    /* Linux copy_process clears pdeath_signal and records real_parent: the
     * creating thread, or the parent's own real parent for CLONE_PARENT and
     * CLONE_THREAD children. */
    child->pdeath_signal = 0;
    child->pdeath_parent_pid = flags & (CLONE_THREAD | CLONE_PARENT)
        ? parent->pdeath_parent_pid : parent->pid;
    child->exit_signal = 0;
    child->clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? child_tid : 0;
    child->robust_list = 0;
    child->rseq_area = 0;
    child->rseq_len = 0;
    child->rseq_sig = 0;
    child->restart_syscall = 0;
    child->nanosleep_deadline = child->nanosleep_remaining = 0;
    child->sigwait_deadline = child->sigwait_mask = 0;
    child->signalfd_waiting = false;
    child->signalfd_wait_mask = 0;
    child->signalfd_vectors = NULL;
    child->signalfd_vector_bytes = 0;
    child->signalfd_vector_count = child->signalfd_read_flags = 0;
    child->sigwait_active = 0;
    /* POSIX timers are process objects; a forked child starts with none and
     * a CLONE_THREAD child resolves timer operations through its leader. */
    for (uint32_t i = 0; i < SCHED_TASK_TIMER_MAX; ++i)
        child->timers[i] = (struct task_posix_timer){0};
    child->waiting_queue = NULL;
    child->wait_next = NULL;
    child->wait_prev = NULL;
    child->wait_minimum = 0;
    child->socket_receive_file = NULL;
    child->socket_receive_done = 0;
    child->socket_receive_message = child->socket_receive_control_capacity = 0;
    child->socket_receive_name_capacity = 0;
    child->socket_receive_flags = child->socket_receive_path_length = 0;
    child->socket_receive_name = 0;
    child->mmsg = (struct task_mmsg_state){0};
    child->sysv_msg = (struct task_sysv_msg_state){0};
    child->sysv_sem = (struct task_sysv_sem_state){0};
    child->sysv_undo = NULL;
    child->syscall_file = NULL;
    child->fifo_open_file = NULL;
    __builtin_memset(&child->regular_io, 0, sizeof(child->regular_io));
    child->tty_old_pgrp = 0;
    child->syscall_pty = (struct task_pty_fd){0};
    child->socket_io_deadline = 0;
    child->socket_io_timed = false;
    if ((flags & (CLONE_VM | CLONE_VFORK)) == CLONE_VM) {
        child->signal_stack_base = 0;
        child->signal_stack_size = 0;
        child->signal_stack_flags = 0;
    }
    child->futex_next = NULL;
    child->futex_state = 0;
    child->futex_waitv_count = 0;
    child->futex_waitv_index = 0;
    child->futex_address = 0;
    if (flags & CLONE_SETTLS) child->fs_base = tls;
    child->image = NULL;
    child->image_len = 0;
    /**
 * @brief Kernel service authority is never inherited by an ordinary fork child.
 */
    child->flags &= ~(TASK_FLAG_RESOURCES_RELEASED | TASK_FLAG_PENDING_LOAD |
                      TASK_FLAG_SERVICE);
    child->flags |= TASK_FLAG_STARTED;
    if (flags & CLONE_THREAD) child->flags &= ~TASK_FLAG_WAITABLE_CHILD;
    else child->flags |= TASK_FLAG_WAITABLE_CHILD;
    child->flags |= TASK_FLAG_FORK_NOEXEC;
    child->state = TASK_BLOCKED;
    child->running_cpu = SCHED_CPU_NONE;
    child->wake_tick = 0;
    child->poll_deadline_ticks = 0;
    child->exit_code = 0;
    child->cpu_ticks = 0;
    child->pending_signals = 0;
    child->timer_pending_signals = 0;
    child->frame = *parent_frame;
    child->frame.rax = 0;
    if (stack) {
        child->frame.rsp = stack;
        child->stack_low = stack & ~4095ULL;
        child->stack_top = (stack + 4095) & ~4095ULL;
        for (uint32_t i = 0; i < sched_task_vma_capacity(parent); ++i) {
            struct task_vma *vma = sched_task_vma_at(parent, i);
            if (vma && vma->used && stack > vma->start && stack <= vma->end) {
                child->stack_low = vma->start;
                child->stack_top = vma->end;
                break;
            }
        }
    }
    arch_fpu_save(child->fpu_state);
    task_copy_name(child, parent->name);
    if (flags & CLONE_VM) {
        child->shared_mm = parent->shared_mm;
        ++child->shared_mm->references;
    } else {
        if (!address_space_clone_cow(sched_task_as(parent), sched_task_as(child))) goto fail;
        child->address_space.executable_inode = sched_task_mm(parent)->executable_inode;
        storage_inode_retain(child->address_space.executable_inode);
        for (uint32_t i = 0; i < SCHED_TASK_VMA_MAX; ++i) {
            const struct task_vma *src = &sched_task_mm(parent)->vmas[i];
            struct task_vma *dst = &child->address_space.vmas[i];
            *dst = *src;
            dst->inode = NULL;
            dst->audio_pcm = NULL;
            dst->drm_bo = NULL;
            dst->drm_mapping_file = NULL;
            dst->sysv_shm_attachment = NULL;
            if (src->inode) storage_inode_retain(dst->inode = src->inode);
            if (sched_task_vma_retain_audio(dst, src) < 0) goto fail;
            if (sched_task_vma_retain_drm(dst, src) < 0) goto fail;
            if (sysv_shm_vma_clone(child, dst, src) < 0) goto fail;
        }
        for (uint32_t i = 0; i < sched_task_mm(parent)->vma_extra_count; ++i) {
            struct task_vma *dst = sched_task_vma_at(child, SCHED_TASK_VMA_MAX + i);
            if (!dst) goto fail;
            const struct task_vma *src = &sched_task_mm(parent)->vma_extra[i];
            *dst = *src;
            dst->inode = NULL;
            dst->audio_pcm = NULL;
            dst->drm_bo = NULL;
            dst->drm_mapping_file = NULL;
            dst->sysv_shm_attachment = NULL;
            if (src->inode) storage_inode_retain(dst->inode = src->inode);
            if (sched_task_vma_retain_audio(dst, src) < 0) goto fail;
            if (sched_task_vma_retain_drm(dst, src) < 0) goto fail;
            if (sysv_shm_vma_clone(child, dst, src) < 0) goto fail;
        }
    }
    if (flags & CLONE_FILES) {
        child->shared_files = parent->shared_files;
        ++child->shared_files->references;
    } else {
        for (uint32_t i = 0; i < sched_task_fds(parent)->file_extra_count; ++i) {
            struct task_file *dst = sched_task_file_at(child, SCHED_TASK_FILE_MAX + i);
            if (!dst) goto fail;
            *dst = sched_task_fds(parent)->file_extra[i];
        }
        if (syscall_clone_task_files(parent, child) < 0) goto fail;
        files_retained = true;
    }
    /**
 * @brief exec_argv and exec_envp are interior pointers, so rebuild them to reference the child-owned packed string storage after the structure copy.
 */
    if (sched_set_task_exec_params(child_pid, parent->exec_argc, parent->exec_argv,
                                   parent->exec_envc, parent->exec_envp,
                                   parent->exec_data, parent->exec_data_len) < 0) goto fail;
    if (task_sysv_sem_clone(parent, child, flags) < 0) goto fail;
    if (flags & CLONE_FS) {
        child->shared_fs = parent->shared_fs;
        ++child->shared_fs->references;
    }
    if (flags & CLONE_SIGHAND) {
        child->shared_sighand = parent->shared_sighand;
        ++child->shared_sighand->references;
    }
    if (flags & CLONE_THREAD) {
        child->shared_limits = parent->shared_limits;
        ++child->shared_limits->references;
    }
    task_groups_retain(child->groups);
    if (flags & CLONE_PARENT_SETTID) *(uint32_t *)(uintptr_t)parent_tid = child_pid;
    if (flags & CLONE_CHILD_SETTID) {
        if (!address_space_user_page_writable(sched_task_as(child), child_tid))
            address_space_handle_cow_fault(sched_task_as(child), child_tid);
        uint64_t phys = address_space_user_page_phys(sched_task_as(child), child_tid);
        *(uint32_t *)(uintptr_t)(RELIEFNT_KERNEL_DIRECT_MAP_BASE + phys + (child_tid & 4095)) = child_pid;
    }
    if (flags & CLONE_VFORK) {
        /* Publish the parent/child relation while still holding
         * scheduler_lock.  The child cannot run and complete before the lock
         * is dropped, so the parent either observes a real completion or is
         * safely put to sleep by this transaction. */
        child->vfork_parent = parent;
        parent->vfork_child = child;
        parent->wake_tick = 0;
        parent->state = TASK_BLOCKED;
    }
    child->state = TASK_READY;
    sched_request_reschedule_all_online();
    kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
    return (int64_t)child_pid;

fail:
    if (child->shared_mm) --child->shared_mm->references;
    else {
        address_space_destroy(sched_task_as(child));
        sched_task_vma_release(child);
    }
    if (child->shared_files) --child->shared_files->references;
    else {
        if (files_retained) syscall_release_task_files(child);
        else {
            sched_task_file_release(child);
            sched_task_pty_release(child);
        }
    }
    kernel_free(child->exec_argv);
    kernel_free(child->exec_envp);
    kernel_free(child->exec_data);
    task_zero(child);
    child->state = TASK_EXITED;
    child->running_cpu = SCHED_CPU_NONE;
    child->flags = TASK_FLAG_RESOURCES_RELEASED;
    kernel_spin_unlock_irqrestore(&scheduler_lock, lock_flags);
    return -12;
}

int64_t sched_fork_current(const struct trap_frame *frame)
{
    return sched_clone_current(frame, 17, 0, 0, 0, 0);
}

int64_t sched_vfork_current(const struct trap_frame *frame)
{
    /* kernel/fork.c:SYSCALL_DEFINE0(vfork): exit_signal = SIGCHLD (17). */
    return sched_clone_current(frame, CLONE_VM | CLONE_VFORK | 17,
                               0, 0, 0, 0);
}
