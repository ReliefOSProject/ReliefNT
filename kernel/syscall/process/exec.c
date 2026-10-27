#include "../internal.h"

/** @brief Release all staging allocations, including partially built vectors.
 * @param params Initialized staging object, reset to zero on return.
 */
void free_exec_params(struct exec_params_kernel *params)
{
    if (!params) return;
    kernel_free(params->argv);
    kernel_free(params->envp);
    kernel_free(params->data);
    *params = (struct exec_params_kernel){0};
}

/** @brief Allocate bounded staging space after counting the user vectors.
 * @param params Zeroed object receiving owned storage.
 * @param argc Argument capacity, excluding its null terminator.
 * @param envc Environment capacity, excluding its null terminator.
 * @param limit Combined Linux argument budget, including pointer words.
 * @param bytes Actual packed string capacity, not the entire remaining budget.
 * @return Zero, -E2BIG for an exhausted budget, or -ENOMEM.
 */
static int allocate_exec_params(struct exec_params_kernel *params, uint32_t argc,
                               uint32_t envc, uint32_t limit, uint32_t bytes)
{
    uint64_t pointers = ((uint64_t)(argc ? argc : 1) + envc) * sizeof(char *);
    if (argc > SCHED_EXEC_ARG_MAX || envc > SCHED_EXEC_ENV_MAX || pointers >= limit ||
        bytes > limit - pointers)
        return -RELIEFOS_E2BIG;
    params->limit = limit;
    params->argv_cap = argc;
    params->envp_cap = envc;
    params->data_cap = bytes;
    params->argv = kernel_malloc(((size_t)argc + 1) * sizeof(char *));
    params->envp = kernel_malloc(((size_t)envc + 1) * sizeof(char *));
    params->data = kernel_malloc(params->data_cap ? params->data_cap : 1);
    if (!params->argv || !params->envp || !params->data) {
        free_exec_params(params);
        return -RELIEFOS_ENOMEM;
    }
    params->argv[0] = params->envp[0] = NULL;
    return 0;
}

/** @brief Read one pointer word without user-address arithmetic wrapping.
 * @param vector User vector base, nonzero.
 * @param index Word offset within the vector.
 * @param value Output pointer word.
 * @return Zero or -EFAULT for inaccessible/overflowing user memory.
 */
static int exec_user_pointer(uint64_t vector, uint32_t index, uint64_t *value)
{
    uint64_t offset = (uint64_t)index * sizeof(uint64_t);
    if (vector > UINT64_MAX - offset || !user_range_ok(vector + offset, sizeof(*value)))
        return -RELIEFOS_EFAULT;
    __builtin_memcpy(value, (const void *)(uintptr_t)(vector + offset), sizeof(*value));
    return 0;
}

/** @brief Measure or copy a user string within the Linux per-string bound.
 * @param pointer User address. @param data Optional copy destination.
 * @param cap String limit including NUL. @param length Output size including NUL.
 * @return Zero, -EFAULT, or -E2BIG.
 */
static int copy_exec_string(uint64_t pointer, char *data, uint32_t cap, uint32_t *length);

/** @brief Count user strings, bounded by the bytes available for pointer words.
 * @param vector Nullable user vector base.
 * @param limit Combined string/pointer budget.
 * @param count Output number of non-null entries.
 * @param bytes Accumulated packed string bytes from both vectors.
 * @return Zero, -EFAULT, or -E2BIG.
 */
static int count_exec_vector(uint64_t vector, uint32_t limit, uint32_t *count, uint32_t *bytes)
{
    *count = 0;
    if (!vector) return 0;
    for (;;) {
        uint64_t pointer;
        int ret = exec_user_pointer(vector, *count, &pointer);
        if (ret < 0) return ret;
        if (!pointer) return 0;
        if (*count >= SCHED_EXEC_ARG_MAX || *count >= limit / sizeof(char *))
            return -RELIEFOS_E2BIG;
        uint32_t cap = limit - *bytes, length;
        if (cap > SCHED_EXEC_STR_MAX) cap = SCHED_EXEC_STR_MAX;
        ret = copy_exec_string(pointer, NULL, cap, &length);
        if (ret < 0) return ret;
        *bytes += length;
        ++*count;
    }
}

/** @brief Copy a user string page by page with Linux MAX_ARG_STRLEN semantics.
 * @param pointer Non-null user string address.
 * @param data Optional kernel destination; NULL measures without copying.
 * @param cap Capacity including the terminating NUL, at most MAX_ARG_STRLEN.
 * @param length Output byte count including the terminating NUL.
 * @return Zero, -EFAULT on unreadable memory, or -E2BIG on a missing NUL.
 */
static int copy_exec_string(uint64_t pointer, char *data, uint32_t cap, uint32_t *length)
{
    uint32_t done = 0;
    while (done < cap) {
        if (pointer > UINT64_MAX - done) return -RELIEFOS_EFAULT;
        uint64_t address = pointer + done;
        uint32_t chunk = 4096u - (uint32_t)(address & 4095u);
        if (chunk > cap - done) chunk = cap - done;
        if (!user_range_ok(address, chunk)) return -RELIEFOS_EFAULT;
        for (uint32_t i = 0; i < chunk; ++i) {
            char ch = ((const char *)(uintptr_t)address)[i];
            if (data) data[done] = ch;
            ++done;
            if (!ch) { *length = done; return 0; }
        }
    }
    return -RELIEFOS_E2BIG;
}

/** @brief Copy a counted user vector; a concurrent size change fails safely.
 * @param vector Nullable user vector base.
 * @param count Count observed before allocation.
 * @param params Owned staging object.
 * @param environment Select envp rather than argv.
 * @return Zero or a negative copy/budget error.
 */
static int copy_exec_vector_user(uint64_t vector, uint32_t count,
                                 struct exec_params_kernel *params, bool environment)
{
    char **dest = environment ? params->envp : params->argv;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t pointer;
        int ret = exec_user_pointer(vector, i, &pointer);
        if (ret < 0) return ret;
        if (!pointer) return -RELIEFOS_EFAULT;
        uint32_t cap = params->data_cap - params->data_len, length = 0;
        if (cap > SCHED_EXEC_STR_MAX) cap = SCHED_EXEC_STR_MAX;
        dest[i] = params->data + params->data_len;
        ret = copy_exec_string(pointer, dest[i], cap, &length);
        if (ret < 0) return ret;
        params->data_len += length;
    }
    if (vector) {
        uint64_t end;
        int ret = exec_user_pointer(vector, count, &end);
        if (ret < 0) return ret;
        if (end) return -RELIEFOS_E2BIG;
    }
    dest[count] = NULL;
    if (environment) params->envc = count;
    else params->argc = count;
    return 0;
}

/** @brief Snapshot argv/envp in heap storage using the current Linux stack budget.
 * @param argv_ptr Nullable user argument vector.
 * @param envp_ptr Nullable user environment vector.
 * @param params Output object; released on failure or with free_exec_params.
 * @return Zero, -E2BIG, -EFAULT, or -ENOMEM; no process image is changed.
 */
int copy_exec_params_from_user(uint64_t argv_ptr, uint64_t envp_ptr, struct exec_params_kernel *params)
{
    if (!params) return -RELIEFOS_EINVAL;
    *params = (struct exec_params_kernel){0};
    struct task *task = sched_current_task();
    uint32_t limit = sched_exec_argument_limit(task ? sched_task_limits(task)->stack.rlim_cur : 8u*1024u*1024u);
    uint32_t argc, envc, bytes = 0;
    int ret = count_exec_vector(argv_ptr, limit, &argc, &bytes);
    if (!ret) ret = count_exec_vector(envp_ptr, limit, &envc, &bytes);
    if (!ret) ret = allocate_exec_params(params, argc, envc, limit, bytes);
    if (!ret) ret = copy_exec_vector_user(argv_ptr, argc, params, false);
    if (!ret) ret = copy_exec_vector_user(envp_ptr, envc, params, true);
    if (ret < 0) free_exec_params(params);
    return ret;
}

/** @brief Identify the two separators accepted by Linux shebang parsing.
 * @param c Header byte. @return True for a space or tab.
 */
static bool script_space(char c) { return c == ' ' || c == '\t'; }

/** @brief Parse the bounded interpreter line without reading beyond 256 bytes.
 * @param header Mutable file prefix. @param name Output interpreter pointer.
 * @param argument Output optional argument pointer, or NULL.
 * @return One for a script, zero for other files, or -ENOEXEC for truncation.
 */
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

/** @brief Append one kernel string without exceeding the reserved Linux budget.
 * @param params Allocated staging object.
 * @param text Kernel string, bounded by MAX_ARG_STRLEN.
 * @param environment Select the environment vector.
 * @return Zero or -E2BIG, without modifying state on error.
 */
static int exec_append_string(struct exec_params_kernel *params, const char *text, bool environment)
{
    size_t size = kernel_string_len_cap(text, SCHED_EXEC_STR_MAX);
    uint32_t *count = environment ? &params->envc : &params->argc;
    uint32_t cap = environment ? params->envp_cap : params->argv_cap;
    if (size >= SCHED_EXEC_STR_MAX || *count >= cap ||
        size + 1 > params->data_cap - params->data_len) return -RELIEFOS_E2BIG;
    char **vector = environment ? params->envp : params->argv;
    vector[(*count)++] = params->data + params->data_len;
    __builtin_memcpy(params->data + params->data_len, text, size + 1);
    params->data_len += (uint32_t)size + 1;
    vector[*count] = NULL;
    return 0;
}

/** @brief Rewrite shebang arguments transactionally, preserving the environment.
 * @param params Owned staging object, replaced only on success.
 * @param interpreter Interpreter pathname.
 * @param argument Optional shebang argument.
 * @param script Script filename.
 * @return Zero or a negative budget/allocation error.
 */
static int exec_script_arguments(struct exec_params_kernel *params, const char *interpreter,
                                  const char *argument, const char *script)
{
    struct exec_params_kernel next = {0};
    uint64_t argc = (params->argc ? params->argc - 1 : 0) + 2ULL + !!argument;
    if (argc > SCHED_EXEC_ARG_MAX) return -RELIEFOS_E2BIG;
    uint64_t bytes = params->data_len;
    if (params->argc) bytes -= __builtin_strlen(params->argv[0]) + 1;
    bytes += __builtin_strlen(interpreter) + 1 + __builtin_strlen(script) + 1;
    if (argument) bytes += __builtin_strlen(argument) + 1;
    if (bytes > params->limit) return -RELIEFOS_E2BIG;
    int ret = allocate_exec_params(&next, (uint32_t)argc, params->envc, params->limit, (uint32_t)bytes);
    if (!ret) ret = exec_append_string(&next, interpreter, false);
    if (!ret && argument) ret = exec_append_string(&next, argument, false);
    if (!ret) ret = exec_append_string(&next, script, false);
    for (uint32_t i = 1; !ret && i < params->argc; ++i)
        ret = exec_append_string(&next, params->argv[i], false);
    for (uint32_t i = 0; !ret && i < params->envc; ++i)
        ret = exec_append_string(&next, params->envp[i], true);
    if (!ret) {
        free_exec_params(params);
        *params = next;
    } else free_exec_params(&next);
    return ret;
}

/** @brief Resolve shebang interpreters through actual held inode paths.
 * @param task Caller under execution ownership with an optional retry scope.
 * @param image Owned path updated only after the next interpreter is resolved.
 * @param execfn Original execution name, preserved in the first script argv.
 * @param inaccessible Original execveat script FD would close on exec.
 * @param params Owned argument staging, rebuilt within the existing stack budget.
 * @return Zero for an ELF candidate or negative lookup/format/copy errno.
 * Reads and authorizes actual held objects; diagnostics never reopen images.
 * Temporary names/paths retire on every outcome, without locks across I/O.
 */
int exec_resolve_scripts(struct task *task,struct vfs_path *image,const char *execfn,
                            bool inaccessible,struct exec_params_kernel *params)
{
    char *script=kernel_malloc(VFS_PATH_MAX+32);if(!script)return -RELIEFOS_ENOMEM;
    copy_text(script,VFS_PATH_MAX+32,execfn);int rc=0;
    struct storage_inode_ref *temporary_guard=NULL;
    for(unsigned depth=0;;++depth) {
        const struct storage_node *node=vfs_path_node(image);
        if(depth>5){rc=-LINUX_ELOOP;break;}
        if(!node || node->type!=RELIEFOS_FS_TYPE_FILE){rc=-RELIEFOS_EACCES;break;}
        rc=fs_permissions_check_path(task,image,FS_ACCESS_EXEC,false);if(rc)break;
        rc=task_lookup_exec_guard(task,image,&temporary_guard);if(rc)break;
        char header[256]={0},*interpreter,*argument;uint32_t got=0;
        rc=vfs_path_read(image,0,header,sizeof(header),&got);if(rc)break;
        rc=script_header(header,&interpreter,&argument);if(rc<=0)break;
        if(inaccessible){rc=-RELIEFOS_ENOENT;break;}
        rc=exec_script_arguments(params,interpreter,argument,script);if(rc)break;
        copy_text(script,VFS_PATH_MAX+32,interpreter);
        struct vfs_lookup_result next={0};
        rc=resolve_kernel_object_at_flags(vfs_system_namespace(),task,LINUX_AT_FDCWD,interpreter,
            false,false,VFS_LOOKUP_FOLLOW,&next);
        if(rc)break;
        if(temporary_guard) {
            storage_inode_allow_write(temporary_guard);(void)storage_inode_put(temporary_guard);temporary_guard=NULL;
        }
        vfs_path_put(image);*image=next.path;
    }
    if(temporary_guard){storage_inode_allow_write(temporary_guard);(void)storage_inode_put(temporary_guard);}
    kernel_free(script);return rc;
}
