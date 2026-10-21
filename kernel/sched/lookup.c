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
 * @brief True when path resolves to the given volume id (used to detect in-use mounts).
 */
static bool sched_path_uses_volume(const char *path, uint32_t volume_id);


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

/**
 * @brief True when path resolves to the given volume id (used to detect in-use mounts).
 */
static bool sched_path_uses_volume(const char *path, uint32_t volume_id)
{
    uint32_t path_volume_id;
    return path && storage_path_volume_id(path, &path_volume_id) == 0 &&
           path_volume_id == volume_id;
}

/**
 * @brief True when any live task's cwd, path, image node, or open files/VMA reference this volume.
 */
bool sched_volume_has_writers(uint32_t volume_id)
{
    for (uint32_t i = 0; i < task_count; ++i) {
        struct task *task = tasks[i];
        if (!task || !task->pid || (task->flags & TASK_FLAG_RESOURCES_RELEASED)) continue;
        for (uint32_t j = 0; j < sched_task_file_capacity(task) + SCHED_TASK_STDIO_MAX; ++j) {
            struct task_file *slot = j < SCHED_TASK_STDIO_MAX ?
                &sched_task_fds(task)->stdio_files[j] : sched_task_file_at(task, j - SCHED_TASK_STDIO_MAX);
            struct task_file *file = task_file_description(slot);
            if (file && file->used && (file->node.flags & STORAGE_NODE_FLAG_TMPFS) &&
                file->node.volume_id == volume_id &&
                ((file->flags & LINUX_O_ACCMODE) == LINUX_O_WRONLY ||
                 (file->flags & LINUX_O_ACCMODE) == LINUX_O_RDWR)) return true;
        }
        for (uint32_t j = 0; j < sched_task_vma_capacity(task); ++j) {
            struct task_vma *vma = sched_task_vma_at(task, j);
            if (vma && vma->used && (vma->flags & TASK_VMA_FLAG_SHARED_FILE) &&
                vma->file_node.volume_id == volume_id &&
                (vma->max_prot & TASK_VMA_PROT_WRITE)) return true;
        }
    }
    return false;
}

bool sched_volume_in_use(uint32_t volume_id)
{
    for (uint32_t i = 0; i < task_count; ++i) {
        const struct task *task = tasks[i];
        if (!task->pid || task->state == TASK_EXITED) {
            continue;
        }
        if (sched_path_uses_volume(sched_task_cwd(task), volume_id) ||
            sched_path_uses_volume(task->path, volume_id) ||
            task->image_node.volume_id == volume_id) {
            return true;
        }
        for (uint32_t fd = 0; fd < sched_task_file_capacity(task); ++fd) {
            const struct task_file *file = task_file_description(sched_task_file_at((struct task *)task, fd));
            if (file && file->used && file->node.volume_id == volume_id) {
                return true;
            }
        }
        for (uint32_t fd = 0; fd < SCHED_TASK_STDIO_MAX; ++fd) {
            struct task_file *file = task_file_description(&sched_task_fds(task)->stdio_files[fd]);
            if (file->used && file->node.volume_id == volume_id) {
                return true;
            }
        }
        for (uint32_t vma = 0; vma < SCHED_TASK_VMA_MAX; ++vma) {
            if (sched_task_mm(task)->vmas[vma].used &&
                (sched_task_mm(task)->vmas[vma].flags & TASK_VMA_FLAG_FILE) &&
                sched_task_mm(task)->vmas[vma].file_node.volume_id == volume_id) {
                return true;
            }
        }
    }
    return false;
}
