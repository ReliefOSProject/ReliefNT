#include "../internal.h"

/**
 * Auth copy current user.
 * @param user Value supplied by the caller.
 * @param task Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int auth_copy_current_user(struct reliefos_user_info *user, const struct task *task);

/**
 * Auth apply session login.
 * @param caller Value supplied by the caller.
 * @param user Value supplied by the caller.
 */
static void auth_apply_session_login(struct task *caller,
                                     const struct reliefos_user_info *user);

/**
 * Auth cleanup logged out task.
 * @param pid Identifier or flags controlling the operation.
 */
static void auth_cleanup_logged_out_task(uint32_t pid);

/**
 * Auth kill session tasks for logout.
 * @param uid Identifier or flags controlling the operation.
 * @param session_id Identifier or flags controlling the operation.
 * @param keep_pid Value supplied by the caller.
 */
static void auth_kill_session_tasks_for_logout(uint32_t uid, uint32_t session_id,
                                               uint32_t keep_pid);


/**
 * Auth copy current user.
 * @param user Value supplied by the caller.
 * @param task Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int auth_copy_current_user(struct reliefos_user_info *user, const struct task *task)
{
    if (!user) {
        return -RELIEFOS_EINVAL;
    }
    *user = (struct reliefos_user_info){0};
    if (!task || !task->uid) {
        user->role = RELIEFOS_AUTH_ROLE_NONE;
        return 0;
    }
    user->uid = task->uid;
    user->role = task_effective_role(task);
    copy_text(user->username, sizeof(user->username), task->username);
    copy_text(user->home, sizeof(user->home), task->home);
    return 0;
}

/**
 * Auth apply session login.
 * @param caller Value supplied by the caller.
 * @param user Value supplied by the caller.
 */
static void auth_apply_session_login(struct task *caller,
                                     const struct reliefos_user_info *user)
{
    uint32_t session_id;
    uint32_t root_pid;
    if (!caller || !user || !user->uid) {
        return;
    }
    session_id = sched_next_session_id();
    root_pid = caller->parent_pid ? caller->parent_pid : caller->pid;
    sched_set_session_identity(root_pid, user, session_id);
    sched_set_task_identity(caller->pid, user, session_id);
}

/**
 * Auth cleanup logged out task.
 * @param pid Identifier or flags controlling the operation.
 */
static void auth_cleanup_logged_out_task(uint32_t pid)
{
    net_close_owner_sockets(pid);
    pty_process_exit(pid);
}

/**
 * Auth kill session tasks for logout.
 * @param uid Identifier or flags controlling the operation.
 * @param session_id Identifier or flags controlling the operation.
 * @param keep_pid Value supplied by the caller.
 */
static void auth_kill_session_tasks_for_logout(uint32_t uid, uint32_t session_id,
                                               uint32_t keep_pid)
{
    struct task_snapshot_info *tasks;
    uint64_t tick;
    uint32_t count;
    if (!uid || !session_id) {
        return;
    }
    count = sched_snapshot(NULL, 0, &tick);
    if (!count || count > UINT32_MAX / sizeof(*tasks)) {
        return;
    }
    tasks = (struct task_snapshot_info *)kernel_malloc(
        (size_t)count * sizeof(*tasks));
    if (!tasks) {
        return;
    }
    count = sched_snapshot(tasks, count, &tick);
    for (uint32_t i = 0; i < count; ++i) {
        const struct task_snapshot_info *task = &tasks[i];
        if (task->pid == 0 || task->pid == keep_pid ||
            task->kind != TASK_KIND_USER || task->state == TASK_EXITED ||
            task->uid != uid || task->session_id != session_id ||
            (task->flags & TASK_FLAG_SERVICE)) {
            continue;
        }
        if (sched_kill_user_task(task->pid, 0) == 0) {
            auth_cleanup_logged_out_task(task->pid);
        }
    }
    kernel_free(tasks);
}
