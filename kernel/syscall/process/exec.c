#include "../internal.h"
#include <reliefnt/swap.h>

/** @brief Prepare one bounded argv/envp operand before any loader lock.
 * @param address User operand. @param length At most one string page or word.
 * @return Zero, prerequisite contention, or the existing user-access error.
 */
static int exec_prepare_user(uint64_t address, uint64_t length)
{
    int ret = swap_prepare_user_range(sched_current_task(), address, length, false);
    if (ret == SWAP_STORAGE_CONTENDED) return KERNEL_SYSCALL_SWAP_CONTENDED;
    return ret < 0 ? -RELIEFOS_EFAULT : 0;
}

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
    if (vector > UINT64_MAX - offset) return -RELIEFOS_EFAULT;
    int ret = exec_prepare_user(vector + offset, sizeof(*value));
    if (ret) return ret;
    if (!user_range_ok(vector + offset, sizeof(*value))) return -RELIEFOS_EFAULT;
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
        int ret = exec_prepare_user(address, chunk);
        if (ret) return ret;
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

#ifdef CONFIG_SWAP
/* One allocation owns both offset words and imported string bytes. Its cursor
 * records only numeric user addresses; no user mapping survives a wait. */
struct exec_import_snapshot {
    uint64_t argv_ptr,envp_ptr,string_ptr;
    uint32_t limit,phase,index,argc,envc,bytes,string_start,string_cap;
    uint32_t offsets_capacity,bytes_capacity;
    bool string_active;
    uint32_t payload[];
};

/** @brief Grow a single scope-owned exec snapshot without losing its prefix.
 * @param slot Owned scope slot. @param state Updated borrowed snapshot pointer.
 * @param offsets Required offset count. @param bytes Required packed capacity.
 * @return Zero or ENOMEM/E2BIG. The old snapshot remains owned on failure.
 * Capacity grows geometrically and is bounded by the admitted argument limit.
 */
static int exec_import_reserve(struct task_lookup_path *slot,struct exec_import_snapshot **state,
                               uint32_t offsets,uint32_t bytes)
{
    struct exec_import_snapshot *old=*state;
    if(offsets<=old->offsets_capacity && bytes<=old->bytes_capacity)return 0;
    if(offsets>old->limit/sizeof(char *) || bytes>old->limit)return -RELIEFOS_E2BIG;
    uint32_t oc=old->offsets_capacity ? old->offsets_capacity : 8;
    uint32_t bc=old->bytes_capacity ? old->bytes_capacity : 64;
    while(oc<offsets)oc=oc>old->limit/sizeof(char *)/2 ? old->limit/sizeof(char *) : oc*2;
    while(bc<bytes)bc=bc>old->limit/2 ? old->limit : bc*2;
    size_t size=sizeof(*old)+(size_t)oc*sizeof(uint32_t)+(size_t)bc;
    struct exec_import_snapshot *next=kernel_malloc(size);if(!next)return -RELIEFOS_ENOMEM;
    *next=*old;next->offsets_capacity=oc;next->bytes_capacity=bc;
    __builtin_memcpy(next->payload,old->payload,(size_t)(old->argc+old->envc)*sizeof(uint32_t));
    __builtin_memcpy((uint8_t *)next->payload+(size_t)oc*sizeof(uint32_t),
                     (uint8_t *)old->payload+(size_t)old->offsets_capacity*sizeof(uint32_t),old->bytes);
    slot->name=(char *)next;kernel_free(old);*state=next;return 0;
}

/** @brief Import argv and envp in order, keeping each observed word and byte.
 * @param task Current execution-owned caller with an armed exec scope.
 * @param argv_ptr Original nullable argument vector. @param envp_ptr Environment.
 * @param params Empty output owning final compact vectors on success.
 * @return Zero, errno or private contention; the scope owns progress until
 * common cancellation and no old user address is dereferenced on replay.
 */
static int exec_import_scoped(struct task *task,uint64_t argv_ptr,uint64_t envp_ptr,
                              struct exec_params_kernel *params)
{
    struct task_lookup_path *slot=NULL;int rc=task_lookup_scope_next(task,&slot);
    if(rc)return rc;
    if(!slot)return -RELIEFOS_EINVAL;
    struct exec_import_snapshot *s;
    if(slot->name) {
        if(slot->flags!=TASK_LOOKUP_EXEC_IMPORT_KIND)return -RELIEFOS_EINVAL;
        s=(struct exec_import_snapshot *)slot->name;
        if(s->argv_ptr!=argv_ptr || s->envp_ptr!=envp_ptr)return -RELIEFOS_EINVAL;
    } else {
        s=kernel_malloc(sizeof(*s));if(!s)return -RELIEFOS_ENOMEM;
        *s=(struct exec_import_snapshot){.argv_ptr=argv_ptr,.envp_ptr=envp_ptr,
            .limit=sched_exec_argument_limit(sched_task_limits(task)->stack.rlim_cur)};
        slot->name=(char *)s;slot->flags=TASK_LOOKUP_EXEC_IMPORT_KIND;
    }
    while(s->phase<2) {
        uint64_t vector=s->phase ? s->envp_ptr : s->argv_ptr;
        if(!vector){++s->phase;s->index=0;continue;}
        if(!s->string_active) {
            uint64_t pointer;rc=exec_user_pointer(vector,s->index,&pointer);if(rc)return rc;
            if(!pointer){++s->phase;s->index=0;continue;}
            if(s->index>=SCHED_EXEC_ARG_MAX || s->index>=s->limit/sizeof(char *))return -RELIEFOS_E2BIG;
            s->string_ptr=pointer;s->string_start=s->bytes;
            s->string_cap=s->limit-s->bytes;
            if(s->string_cap>SCHED_EXEC_STR_MAX)s->string_cap=SCHED_EXEC_STR_MAX;
            s->string_active=true;
        }
        uint32_t done=s->bytes-s->string_start;
        if(done>=s->string_cap)return -RELIEFOS_E2BIG;
        if(s->string_ptr>UINT64_MAX-done)return -RELIEFOS_EFAULT;
        uint64_t address=s->string_ptr+done;
        uint32_t chunk=4096u-(uint32_t)(address&4095u);
        if(chunk>s->string_cap-done)chunk=s->string_cap-done;
        rc=exec_prepare_user(address,chunk);if(rc)return rc;
        if(!user_range_ok(address,chunk))return -RELIEFOS_EFAULT;
        rc=exec_import_reserve(slot,&s,s->argc+s->envc+1,s->bytes+chunk);if(rc)return rc;
        char *data=(char *)(s->payload+s->offsets_capacity);
        for(uint32_t i=0;i<chunk;++i) {
            char ch=((const char *)(uintptr_t)address)[i];data[s->bytes++]=ch;
            if(!ch) {
                ((uint32_t *)s->payload)[s->argc+s->envc]=s->string_start;
                if(s->phase)++s->envc;else ++s->argc;
                ++s->index;s->string_active=false;break;
            }
        }
    }
    rc=allocate_exec_params(params,s->argc,s->envc,s->limit,s->bytes);if(rc)return rc;
    uint32_t *offsets=(uint32_t *)s->payload;
    char *data=(char *)(s->payload+s->offsets_capacity);
    __builtin_memcpy(params->data,data,s->bytes);
    for(uint32_t i=0;i<s->argc;++i)params->argv[i]=params->data+offsets[i];
    for(uint32_t i=0;i<s->envc;++i)params->envp[i]=params->data+offsets[s->argc+i];
    params->argv[s->argc]=NULL;params->envp[s->envc]=NULL;
    params->argc=s->argc;params->envc=s->envc;params->data_len=s->bytes;
    return 0;
}
#endif

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
#ifdef CONFIG_SWAP
    if(task && task->lookup_scope.armed)
        return exec_import_scoped(task,argv_ptr,envp_ptr,params);
#endif
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
