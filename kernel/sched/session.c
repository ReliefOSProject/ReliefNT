#include "internal.h"



/**
 * @brief Bounded, always-NUL-terminated copy used for username and home identity strings.
 */
void task_copy_identity_text(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

/**
 * @brief Strip admin flag, uid, role, session, username, and home, leaving an anonymous task.
 */
void task_clear_identity(struct task *task)
{
    if (!task) {
        return;
    }
    task->flags &= ~TASK_FLAG_ELEVATED_ADMIN;
    task_groups_release(task);
    task->uid = 0;
    task->gid = 0;
    task->euid = 0;
    task->egid = 0;
    task->suid = 0;
    task->sgid = 0;
    task->fsuid = 0;
    task->fsgid = 0;
    task->cap_effective = 0;
    task->cap_permitted = 0;
    task->cap_inheritable = 0;
    task->cap_bset = (UINT64_C(1) << (CAP_LAST_CAP + 1)) - 1;
    task->cap_ambient = 0;
    task->securebits = 0;
    task->role = RELIEFOS_AUTH_ROLE_NONE;
    task->session_id = 0;
    task->username[0] = 0;
    task->home[0] = 0;
}

/**
 * @brief Inherit uid, role, session, username, and home from the parent task.
 */
void task_copy_identity_from_parent(struct task *task, const struct task *parent)
{
    if (!task || !parent) {
        return;
    }
    task->uid = parent->uid;
    task->gid = parent->gid;
    task->euid = parent->euid;
    task->egid = parent->egid;
    task->suid = parent->suid;
    task->sgid = parent->sgid;
    task->fsuid = parent->fsuid;
    task->fsgid = parent->fsgid;
    task->cap_effective = parent->cap_effective;
    task->cap_permitted = parent->cap_permitted;
    task->cap_inheritable = parent->cap_inheritable;
    task->cap_bset = parent->cap_bset;
    task->cap_ambient = parent->cap_ambient;
    task->securebits = parent->securebits;
    task->no_new_privs = parent->no_new_privs;
    task->groups = parent->groups;
    task_groups_retain(task->groups);
    task->role = parent->role;
    task->session_id = parent->session_id;
    task_copy_identity_text(task->username, sizeof(task->username), parent->username);
    task_copy_identity_text(task->home, sizeof(task->home), parent->home);
    task_copy_identity_text(sched_task_root_dir(task), RELIEFOS_FS_PATH_LEN, sched_task_root(parent));
}

int64_t sched_process_group_session(uint32_t process_group)
{
    if (!process_group) return -LINUX_ESRCH;
    for (uint32_t i = 0; i < task_count; ++i) {
        const struct task *task = tasks[i];
        if (task && task->kind == TASK_KIND_USER && task->process_group == process_group &&
            (task->state != TASK_EXITED || (task->flags & TASK_FLAG_WAITABLE_CHILD)))
            return task->process_session;
    }
    /* Linux tty_jobctrl.c also accepts a PID whose group has no members. */
    return sched_get_process_session(process_group);
}

int sched_process_group_orphaned(uint32_t process_group)
{
    for (uint32_t i = 0; i < task_count; ++i) {
        const struct task *task = tasks[i];
        if (!task || task->kind != TASK_KIND_USER || task->state == TASK_EXITED ||
            task->process_group != process_group) continue;
        const struct task *parent = sched_find(task->parent_pid);
        if (parent && parent->pid > 1 &&
            (parent->state != TASK_EXITED || thread_group_pending(sched_task_tgid(parent), parent)) &&
            parent->process_group != process_group && parent->process_session == task->process_session)
            return 0;
    }
    return 1;
}

/**
 * @brief Move a caller-controlled task into an existing or newly created process group.
 */
int sched_set_process_group(uint32_t caller_pid, uint32_t pid,
                            uint32_t process_group)
{
    struct task *caller = sched_find(caller_pid);
    if (!caller || caller->kind != TASK_KIND_USER) return -LINUX_ESRCH;
    uint32_t caller_group = sched_task_tgid(caller);
    if (!pid) pid = caller_group;
    if (!process_group) process_group = pid;
    if ((int32_t)process_group < 0) return -LINUX_EINVAL;
    uint64_t flags;
    int result = -LINUX_ESRCH;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    struct task *target = sched_find(pid);
    if (!target || target->kind != TASK_KIND_USER ||
        (target->state == TASK_EXITED && !(target->flags & TASK_FLAG_WAITABLE_CHILD))) goto done;
    result = -LINUX_EINVAL;
    if (sched_task_tgid(target) != pid) goto done;
    struct task *parent = sched_find(target->parent_pid);
    if (parent && sched_task_tgid(parent) == caller_group) {
        result = -LINUX_EPERM;
        if (target->process_session != caller->process_session) goto done;
        result = -LINUX_EACCES;
        if (!(target->flags & TASK_FLAG_FORK_NOEXEC)) goto done;
    } else {
        result = -LINUX_ESRCH;
        if (pid != caller_group) goto done;
    }
    result = -LINUX_EPERM;
    if (target->process_session == pid) goto done;
    if (process_group != pid) {
        int group_exists = 0;
        for (uint32_t i = 0; i < task_count; ++i) {
            const struct task *member = tasks[i];
            if (member && member->pid != 0 && member->kind == TASK_KIND_USER &&
                (member->state != TASK_EXITED || (member->flags & TASK_FLAG_WAITABLE_CHILD)) &&
                member->process_group == process_group &&
                member->process_session == target->process_session) {
                group_exists = 1;
                break;
            }
        }
        if (!group_exists) goto done;
    }
    for (uint32_t i = 0; i < task_count; ++i)
        if (tasks[i] && sched_task_tgid(tasks[i]) == pid)
            tasks[i]->process_group = process_group;
    result = 0;
done:
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return result;
}

int64_t sched_get_process_group(uint32_t pid)
{
    return sched_process_identity(pid, false);
}

/**
 * @brief Make the calling task the leader of a new session and process group.
 */
int64_t sched_create_process_session(uint32_t pid)
{
    struct task *task = sched_find(pid);
    if (!task || task->kind != TASK_KIND_USER || task->state == TASK_EXITED) return -LINUX_ESRCH;
    uint32_t tgid = sched_task_tgid(task);
    uint64_t flags;
    int64_t result = -LINUX_EPERM;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    if (task->process_session == tgid) goto done;
    for (uint32_t i = 0; i < task_count; ++i) {
        const struct task *member = tasks[i];
        if (member && member->kind == TASK_KIND_USER &&
            (member->state != TASK_EXITED || (member->flags & TASK_FLAG_WAITABLE_CHILD)) &&
            member->process_group == tgid) goto done;
    }
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *member = tasks[i];
        if (member && sched_task_tgid(member) == tgid) {
            member->process_session = tgid;
            member->process_group = tgid;
            member->controlling_pty_id = 0;
            member->tty_old_pgrp = 0;
        }
    }
    result = tgid;
done:
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return result;
}

/**
 * @brief Count open PTY endpoint descriptors and controlling attachments.
 * @param pty_id PTY identifier.
 * @return Number of live references.
 */
uint32_t sched_pty_reference_count(uint32_t pty_id)
{
    uint32_t count = pty_transfer_count(pty_id, 0);
    uint64_t flags;
    if (!pty_id) return 0;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (!task || task->kind != TASK_KIND_USER) continue;
        if (task->pty_id == pty_id) ++count;
        for (uint32_t fd = 0; fd < sched_task_pty_capacity(task); ++fd) {
            const struct task_pty_fd *entry = sched_task_pty_at(task, fd);
            if (entry->used && entry->pty_id == pty_id) {
                ++count;
            }
        }
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return count;
}

/**
 * @brief Count live master-endpoint references that prevent PTY session teardown.
 * @param pty_id Stable PTY session identifier; zero has no references.
 * @return Number of transfer references and open master aliases.
 * @note The scheduler task-table scan runs with the scheduler lock held; the
 * transfer count is sampled before acquiring that lock.
 */
uint32_t sched_pty_master_reference_count(uint32_t pty_id)
{
    uint32_t count = pty_transfer_count(pty_id, 1);
    uint64_t flags;
    if (!pty_id) return 0;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (!task || task->kind != TASK_KIND_USER) continue;
        for (uint32_t fd = 0; fd < sched_task_pty_capacity(task); ++fd) {
            const struct task_pty_fd *entry = sched_task_pty_at(task, fd);
            if (entry->used && entry->pty_id == pty_id &&
                entry->endpoint == TASK_PTY_ENDPOINT_MASTER) ++count;
        }
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    return count;
}

/**
 * @brief True when some live task in this process group is attached to the given PTY.
 */
int sched_process_group_has_pty(uint32_t process_group, uint32_t pty_id)
{
    if (!process_group || !pty_id) {
        return 0;
    }
    for (uint32_t i = 0; i < task_count; ++i) {
        const struct task *task = tasks[i];
        if (task->pid != 0 && task->kind == TASK_KIND_USER &&
            task->state != TASK_EXITED && task->process_group == process_group &&
            task->pty_id == pty_id) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Return the process session of pid, or a negative error.
 */
int64_t sched_get_process_session(uint32_t pid)
{
    return sched_process_identity(pid, true);
}

/**
 * @brief Set a task's uid/role/session/username/home from user, or clear it when user is empty.
 */
void sched_set_task_identity(uint32_t pid, const struct reliefos_user_info *user,
                             uint32_t session_id)
{
    struct task *task = sched_find(pid);
    if (!task) {
        return;
    }
    task->flags &= ~TASK_FLAG_ELEVATED_ADMIN;
    if (!user || !user->uid) {
        task_credentials_prepare(task, 0, 0, 0, 0, 0);
        task_clear_identity(task);
        task_copy_cwd(task, "/");
        return;
    }
    task_credentials_prepare(task, user->uid, user->uid, user->uid, user->uid, 0);
    task->uid = user->uid;
    task->gid = user->uid;
    task->euid = user->uid;
    task->egid = user->uid;
    task->suid = user->uid;
    task->sgid = user->uid;
    task->fsuid = user->uid;
    task->fsgid = user->uid;
    if (user->uid == 0) {
        const uint64_t all = task->cap_bset;
        task->cap_effective = all;
        task->cap_permitted = all;
        task->cap_inheritable = 0;
    } else {
        task->cap_effective = 0;
        task->cap_permitted = 0;
        task->cap_inheritable = 0;
    }
    task->role = user->role;
    task->cap_ambient = 0;
    task->session_id = session_id;
    task_copy_identity_text(task->username, sizeof(task->username), user->username);
    task_copy_identity_text(task->home, sizeof(task->home), user->home);
    if (user->home[0]) {
        task_copy_cwd(task, user->home);
    }
}

/**
 * @brief Apply identity to the given task and its non-service children in a session.
 */
void sched_set_session_identity(uint32_t parent_pid, const struct reliefos_user_info *user,
                                uint32_t session_id)
{
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if ((task->flags & TASK_FLAG_SERVICE) == 0 &&
            (task->pid == parent_pid || task->parent_pid == parent_pid)) {
            sched_set_task_identity(task->pid, user, session_id);
        }
    }
}

/**
 * @brief Clear identity and reset cwd for every non-service task in this session.
 */
void sched_clear_session_identity(uint32_t session_id)
{
    if (!session_id) {
        return;
    }
    for (uint32_t i = 0; i < task_count; ++i) {
        if (tasks[i]->session_id == session_id &&
            (tasks[i]->flags & TASK_FLAG_SERVICE) == 0) {
            task_clear_identity(tasks[i]);
            task_copy_cwd(tasks[i], "/");
        }
    }
}

/**
 * @brief Return the next session id, wrapping past zero back to 1.
 */
uint32_t sched_next_session_id(void)
{
    if (next_session_id == 0) {
        next_session_id = 1;
    }
    return next_session_id++;
}

/**
 * @brief Publish a controlling tty change to the process thread group.
 * @param pid Target thread ID.
 * @param pty_id New tty ID, or zero.
 */
void sched_set_controlling_pty(uint32_t pid, uint32_t pty_id)
{
    struct task *target = sched_find(pid);
    if (!target) return;
    for (uint32_t i = 0; i < task_count; ++i)
        if (tasks[i] && tasks[i]->tgid == target->tgid)
            tasks[i]->controlling_pty_id = pty_id;
}

/**
 * @brief Detach all processes when a privileged session steals a tty.
 * @param pty_id Controlling tty ID to clear.
 */
void sched_clear_controlling_pty(uint32_t pty_id)
{
    for (uint32_t i = 0; i < task_count; ++i)
        if (tasks[i] && tasks[i]->controlling_pty_id == pty_id)
            tasks[i]->controlling_pty_id = 0;
}
