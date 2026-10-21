#include "../internal.h"



/**
 * Copy user path.
 * @param dst Value supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @param user_ptr Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int copy_user_path(char *dst, uint32_t cap, uint64_t user_ptr)
{
    size_t len;
    if (!dst || !cap || !user_range_ok(user_ptr, 1)) {
        return -RELIEFOS_EFAULT;
    }
    len = user_strlen((const char *)(uintptr_t)user_ptr, cap);
    if (len == cap) return user_range_ok(user_ptr, cap) ? -RELIEFOS_ENAMETOOLONG : -RELIEFOS_EFAULT;
    if (!user_range_ok(user_ptr, len + 1)) {
        return -RELIEFOS_EFAULT;
    }
    for (size_t i = 0; i <= len; ++i) {
        dst[i] = ((const char *)(uintptr_t)user_ptr)[i];
    }
    return 0;
}

/**
 * Resolve user path.
 * @param task Value supplied by the caller.
 * @param user_ptr Value supplied by the caller.
 * @param out Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
int resolve_user_path_ids(struct task *task, uint64_t user_ptr, char *out,
                                  uint32_t cap, bool real_ids)
{
    char raw[RELIEFOS_FS_PATH_LEN];
    int ret = copy_user_path(raw, sizeof(raw), user_ptr);
    if (ret < 0) {
        return ret;
    }
    if (!raw[0]) return -RELIEFOS_ENOENT;
    return fs_permissions_resolve(task, task ? sched_task_cwd(task) : "/", raw, out, cap, real_ids);
}

int resolve_user_path(struct task *task, uint64_t user_ptr, char *out, uint32_t cap)
{
    return resolve_user_path_ids(task, user_ptr, out, cap, false);
}

int resolve_kernel_path_at_flags(struct task *task, int32_t dirfd, const char *raw,
                                bool allow_empty, char *path, struct task_file **empty_file,
                                bool real_ids, uint32_t lookup_flags)
{
    if (empty_file) *empty_file = NULL;
    const char *base = task ? sched_task_cwd(task) : "/";
    if (!raw[0] && !allow_empty) return -RELIEFOS_ENOENT;
    if (raw[0] != '/' && dirfd != LINUX_AT_FDCWD) {
        struct task_file *file = task_file_for_fd(task, dirfd);
        if (!file) return -RELIEFOS_EBADF;
        /* AT_EMPTY_PATH addresses the open object, including anonymous pipes. */
        if (!raw[0] && empty_file) {
            *empty_file = file;
            copy_text(path, RELIEFOS_FS_PATH_LEN, file->path);
            return 0;
        }
        if (raw[0] && file->node.type != RELIEFOS_FS_TYPE_DIR) return -RELIEFOS_ENOTDIR;
        if (!file->path[0]) return -RELIEFOS_EBADF;
        base = file->path;
    }
    if (!raw[0]) { copy_text(path, RELIEFOS_FS_PATH_LEN, base); return 0; }
    return fs_permissions_resolve_flags(task, base, raw, path, RELIEFOS_FS_PATH_LEN,
                                         real_ids, lookup_flags);
}

/** @brief Resolves a raw *at pathname with an explicit final-symlink policy. */
int resolve_user_path_at_flags(struct task *task, int32_t dirfd, uint64_t user_ptr,
                                bool allow_empty, char *path, struct task_file **empty_file,
                                bool real_ids, uint32_t lookup_flags)
{
    char raw[RELIEFOS_FS_PATH_LEN];
    int ret = copy_user_path(raw, sizeof(raw), user_ptr);
    if (ret < 0) return ret;
    return resolve_kernel_path_at_flags(task, dirfd, raw, allow_empty, path, empty_file,
                                         real_ids, lookup_flags);
}

/**
 * @brief Validates and strips a trailing slash on a parent-resolved mutation path.
 * @param path Mutable resolved pathname.
 * @param creating Nonzero for non-directory creation, which rejects existing names.
 * @return Zero or Linux's trailing slash error without following the last symlink.
 */
int mutation_path_slash(char *path, bool creating)
{
    uint32_t length = 0;
    struct storage_node node;
    while (path[length]) ++length;
    if (length <= 1 || path[length - 1] != '/') return 0;
    path[length - 1] = 0;
    int ret = storage_lookup_path(path, &node);
    if (creating) return ret == 0 ? -RELIEFOS_EEXIST : ret;
    if (ret < 0) return ret;
    return node.type == RELIEFOS_FS_TYPE_DIR ? 0 : -RELIEFOS_ENOTDIR;
}

/**
 * @brief Classifies final dot components before any storage lexical normalization.
 * @param path Absolute parent-resolved path, possibly with a trailing slash.
 * @return 1 for dot, 2 for dot-dot, 3 for root, otherwise 0.
 */
int mutation_path_special(const char *path)
{
    uint32_t end = 0, start;
    while (path[end]) ++end;
    while (end && path[end - 1] == '/') --end;
    if (!end) return 3;
    start = end;
    while (start && path[start - 1] != '/') --start;
    if (path[start] == '.') {
        if (end - start == 1) return 1;
        if (end - start == 2 && path[start + 1] == '.') return 2;
    }
    return 0;
}

/**
 * Storage errno.
 * @param ret Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int storage_errno(int ret)
{
    if (ret == -2) {
        return -RELIEFOS_ENOENT;
    }
    if (ret == -17) {
        return -RELIEFOS_EEXIST;
    }
    if (ret == -20) {
        return -RELIEFOS_ENOTDIR;
    }
    if (ret == -21) {
        return -RELIEFOS_EISDIR;
    }
    if (ret == -39) {
        return -RELIEFOS_ENOTEMPTY;
    }
    if (ret == -18) {
        return -LINUX_EXDEV;
    }
    if (ret == -95) {
        return -RELIEFOS_EOPNOTSUPP;
    }
    return ret;
}
