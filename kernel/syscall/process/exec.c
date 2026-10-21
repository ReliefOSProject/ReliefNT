#include "../internal.h"

/**
 * Copy user vector.
 * @param user_ptr Value supplied by the caller.
 * @param max_count Value supplied by the caller.
 * @param out_ptrs Value supplied by the caller.
 * @param data Value supplied by the caller.
 * @param data_cap Value supplied by the caller.
 * @param out_count Value supplied by the caller.
 * @param data_len Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int copy_user_vector(uint64_t user_ptr, uint32_t max_count,
                            char *out_ptrs[], char *data,
                            uint32_t data_cap, uint32_t *out_count, uint32_t *data_len);

static bool script_space(char c);

static int script_header(char header[256], char **name, char **argument);

static int exec_append_string(struct exec_params_kernel *params, const char *text, bool environment);

static int exec_script_arguments(struct exec_params_kernel *params, const char *interpreter,
                                  const char *argument, const char *script);


/**
 * Copy user vector.
 * @param user_ptr Value supplied by the caller.
 * @param max_count Value supplied by the caller.
 * @param out_ptrs Value supplied by the caller.
 * @param data Value supplied by the caller.
 * @param data_cap Value supplied by the caller.
 * @param out_count Value supplied by the caller.
 * @param data_len Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int copy_user_vector(uint64_t user_ptr, uint32_t max_count,
                            char *out_ptrs[], char *data,
                            uint32_t data_cap, uint32_t *out_count, uint32_t *data_len)
{
    uint64_t *user_vec = (uint64_t *)(uintptr_t)user_ptr;
    uint32_t count = 0;
    if (!out_ptrs || !data || !out_count || !data_len) {
        return -RELIEFOS_EINVAL;
    }
    if (!user_ptr) {
        *out_count = 0;
        return 0;
    }
    for (;;) {
        uint64_t entry_ptr;
        if (!user_range_ok((uint64_t)(uintptr_t)&user_vec[count], sizeof(uint64_t))) {
            return -RELIEFOS_EFAULT;
        }
        entry_ptr = user_vec[count];
        if (!entry_ptr) {
            break;
        }
        if (count >= max_count) {
            return -RELIEFOS_E2BIG;
        }
        uint32_t len = 0;
        uint32_t start = *data_len;
        int ret = copy_user_string_fixed(data + start, data_cap - start, entry_ptr, &len);
        if (ret < 0) {
            return ret;
        }
        out_ptrs[count] = data + start;
        *data_len += len + 1;
        ++count;
    }
    out_ptrs[count] = 0;
    *out_count = count;
    return 0;
}

/**
 * Copy exec params from user.
 * @param argv_ptr Value supplied by the caller.
 * @param envp_ptr Value supplied by the caller.
 * @param params Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int copy_exec_params_from_user(uint64_t argv_ptr, uint64_t envp_ptr, struct exec_params_kernel *params)
{
    int ret;
    uint32_t data_len = 0;
    if (!params) {
        return -RELIEFOS_EINVAL;
    }
    for (uint32_t i = 0; i < SCHED_EXEC_ARG_MAX + 1; ++i) {
        params->argv[i] = 0;
    }
    for (uint32_t i = 0; i < SCHED_EXEC_ENV_MAX + 1; ++i) {
        params->envp[i] = 0;
    }
    params->argc = 0;
    params->envc = 0;
    params->data_len = 0;

    if (argv_ptr) {
        ret = copy_user_vector(argv_ptr, SCHED_EXEC_ARG_MAX, params->argv, params->data,
                               sizeof(params->data), &params->argc, &data_len);
        if (ret < 0) {
            return ret;
        }
    }
    ret = copy_user_vector(envp_ptr, SCHED_EXEC_ENV_MAX, params->envp, params->data,
                           sizeof(params->data), &params->envc, &data_len);
    if (ret < 0) {
        return ret;
    }
    params->data_len = data_len;
    return 0;
}

static bool script_space(char c) { return c == ' ' || c == '\t'; }

static int script_header(char header[256], char **name, char **argument)
{
    if (header[0] != '#' || header[1] != '!') return 0;
    char *last = header + 255, *end = header;
    while (end <= last && *end && *end != '\n') ++end;
    if (end > last || *end != '\n') {
        char *start = header + 2;
        while (start <= last && script_space(*start)) ++start;
        if (start > last) return -LINUX_ENOEXEC;
        while (start <= last && *start && !script_space(*start)) ++start;
        if (start > last) return -LINUX_ENOEXEC;
        end = last;
    }
    while (end > header + 2 && script_space(end[-1])) --end;
    char *start = header + 2;
    while (start < end && script_space(*start)) ++start;
    if (start == end) return -LINUX_ENOEXEC;
    char *separator = start;
    while (separator < end && *separator && !script_space(*separator)) ++separator;
    *argument = NULL;
    if (separator < end && *separator) {
        char *arg = separator;
        while (arg < end && script_space(*arg)) ++arg;
        if (arg < end) *argument = arg;
    }
    *end = 0;
    *separator = 0;
    *name = start;
    return 1;
}

static int exec_append_string(struct exec_params_kernel *params, const char *text, bool environment)
{
    uint32_t length = (uint32_t)__builtin_strlen(text) + 1;
    uint32_t *count = environment ? &params->envc : &params->argc;
    uint32_t limit = environment ? SCHED_EXEC_ENV_MAX : SCHED_EXEC_ARG_MAX;
    if (*count >= limit || length > sizeof(params->data) - params->data_len) return -RELIEFOS_E2BIG;
    char **vector = environment ? params->envp : params->argv;
    vector[(*count)++] = params->data + params->data_len;
    __builtin_memcpy(params->data + params->data_len, text, length);
    params->data_len += length;
    vector[*count] = NULL;
    return 0;
}

static int exec_script_arguments(struct exec_params_kernel *params, const char *interpreter,
                                  const char *argument, const char *script)
{
    struct exec_params_kernel *next = kernel_malloc(sizeof(*next));
    if (!next) return -RELIEFOS_ENOMEM;
    __builtin_memset(next, 0, sizeof(*next));
    int ret = exec_append_string(next, interpreter, false);
    if (!ret && argument) ret = exec_append_string(next, argument, false);
    if (!ret) ret = exec_append_string(next, script, false);
    for (uint32_t i = 1; !ret && i < params->argc; ++i)
        ret = exec_append_string(next, params->argv[i], false);
    for (uint32_t i = 0; !ret && i < params->envc; ++i)
        ret = exec_append_string(next, params->envp[i], true);
    if (!ret) {
        __builtin_memcpy(params->data, next->data, next->data_len);
        params->data_len = next->data_len;
        params->argc = next->argc;
        params->envc = next->envc;
        for (uint32_t i = 0; i < next->argc; ++i)
            params->argv[i] = params->data + (next->argv[i] - next->data);
        for (uint32_t i = 0; i < next->envc; ++i)
            params->envp[i] = params->data + (next->envp[i] - next->data);
        params->argv[params->argc] = params->envp[params->envc] = NULL;
    }
    kernel_free(next);
    return ret;
}

int exec_resolve_scripts(struct task *task, char *path, struct storage_node *node,
                                 const char *execfn, bool inaccessible,
                                 struct exec_params_kernel *params)
{
    char script[RELIEFOS_FS_PATH_LEN + 32];
    copy_text(script, sizeof(script), execfn);
    for (unsigned depth = 0; ; ++depth) {
        if (depth > 5) return -LINUX_ELOOP;
        if (node->type != RELIEFOS_FS_TYPE_FILE) return -RELIEFOS_EACCES;
        int ret = fs_permissions_check_node(task, path, node, FS_ACCESS_EXEC, false);
        if (ret < 0) return ret;
        char header[256] = {0}, *interpreter, *argument;
        uint32_t got = 0;
        ret = storage_read_node(node, 0, header, sizeof(header), &got);
        if (ret < 0) return ret;
        ret = script_header(header, &interpreter, &argument);
        if (ret <= 0) return ret;
        if (inaccessible) return -RELIEFOS_ENOENT;
        ret = exec_script_arguments(params, interpreter, argument, script);
        if (ret < 0) return ret;
        copy_text(script, sizeof(script), interpreter);
        ret = fs_permissions_resolve(task, sched_task_cwd(task), interpreter,
                                      path, RELIEFOS_FS_PATH_LEN, false);
        if (ret < 0) return ret;
        ret = storage_lookup_path(path, node);
        if (ret < 0) return ret;
    }
}
