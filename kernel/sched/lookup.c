#include "internal.h"

/**
 * @brief True when both strings are non-NULL and byte-for-byte identical.
 */
static int str_eq(const char *a, const char *b);

/**
 * @brief Return the substring after the final '/', or "" for a NULL or slash-free path.
 */
static const char *task_path_basename(const char *path);




/**
 * @brief True when both strings are non-NULL and byte-for-byte identical.
 */
static int str_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

/**
 * @brief Copy name into the task's fixed buffer (bounded, NUL-terminated) and point task->name at it.
 */
void task_copy_name(struct task *task, const char *name)
{
    size_t i = 0;
    if (!task) {
        return;
    }
    if (name) {
        while (i + 1 < sizeof(task->name_storage) && name[i]) {
            task->name_storage[i] = name[i];
            ++i;
        }
    }
    task->name_storage[i] = 0;
    task->name = task->name_storage;
}

/**
 * @brief Copy cwd into the task's fixed buffer, defaulting an empty or NULL value to "/".
 */
void task_copy_cwd(struct task *task, const char *cwd)
{
    size_t i = 0;
    if (!task) {
        return;
    }
    if (!cwd || !cwd[0]) {
        cwd = "/";
    }
    while (i + 1 < RELIEFOS_FS_PATH_LEN && cwd[i]) {
        sched_task_cwd(task)[i] = cwd[i];
        ++i;
    }
    sched_task_cwd(task)[i] = 0;
}

/**
 * @brief Copy path into the task's fixed buffer, NUL-terminating even for a NULL source.
 */
void task_copy_path(struct task *task, const char *path)
{
    size_t i = 0;
    if (!task) {
        return;
    }
    while (path && path[i] && i + 1 < sizeof(task->path)) {
        task->path[i] = path[i];
        ++i;
    }
    task->path[i] = 0;
}

/**
 * @brief Return the substring after the final '/', or "" for a NULL or slash-free path.
 */
static const char *task_path_basename(const char *path)
{
    const char *base = path;
    if (!path) {
        return "";
    }
    for (const char *p = path; *p; ++p) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    return base ? base : "";
}

/**
 * @brief Linear scan for the task with the given pid, or NULL when absent.
 */
struct task *sched_find(uint32_t pid)
{
    for (uint32_t i = 0; i < task_count; ++i) {
        if (tasks[i]->pid == pid) {
            return tasks[i];
        }
    }
    return NULL;
}

/**
 * @brief Borrow a scheduler slot with no allocation or initial-capacity cutoff.
 * @param index Zero-based slot; caller holds execution ownership or scheduler lock.
 * @return Stable task pointer until the caller releases ownership, or NULL
 * after the last slot. Does not acquire a second scheduler lock.
 */
struct task *sched_task_at(uint32_t index)
{
    return index < task_count ? tasks[index] : NULL;
}

/**
 * @brief Return the first task whose name matches, or NULL when none does.
 */
struct task *sched_find_by_name(const char *name)
{
    for (uint32_t i = 0; i < task_count; ++i) {
        if (str_eq(tasks[i]->name, name)) {
            return tasks[i];
        }
    }
    return NULL;
}

/**
 * @brief Return the live user task whose path matches exactly, or NULL.
 */
struct task *sched_find_by_path(const char *path)
{
    for (uint32_t i = 0; i < task_count; ++i) {
        if (tasks[i]->pid && tasks[i]->kind == TASK_KIND_USER &&
            tasks[i]->state != TASK_EXITED && str_eq(tasks[i]->path, path)) {
            return tasks[i];
        }
    }
    return NULL;
}

/**
 * @brief Return the live user task whose path's basename matches, or NULL.
 */
struct task *sched_find_by_path_basename(const char *basename)
{
    for (uint32_t i = 0; i < task_count; ++i) {
        if (tasks[i]->pid && tasks[i]->kind == TASK_KIND_USER &&
            tasks[i]->state != TASK_EXITED &&
            str_eq(task_path_basename(tasks[i]->path), basename)) {
            return tasks[i];
        }
    }
    return NULL;
}
