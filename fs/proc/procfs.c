#include <reliefnt/sound_service.h>
/* Generated procfs with a narrowly scoped optional diagnostic control. */
#include <reliefnt/mm.h>
#include <reliefnt/module.h>
#include <reliefnt/driver_manager.h>
#include <reliefnt/sched.h>
#include <reliefnt/smp.h>
#include <reliefnt/storage.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/time.h>
#include <reliefnt/userland.h>
#include <reliefnt/version.h>
#include <reliefnt/uts.h>
#include <reliefnt/inventory.h>
#include <reliefnt/text_stream.h>
#include <reliefnt/lock_profile.h>
#include <reliefnt/permissions.h>
#include <reliefnt/pty.h>
#include <reliefos/fs_abi.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include "internal.h"
#include <reliefnt/proc_stats.h>

#define PROCFS_PATH_MAX RELIEFOS_FS_PATH_LEN
#define PROCFS_CONTENT_MAX 1024u

static int proc_text_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == 0 && *b == 0;
}

static void proc_copy(char *dst, uint32_t capacity, const char *src)
{
    uint32_t i = 0;
    if (!dst || !capacity) return;
    while (src && src[i] && i + 1u < capacity) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static void proc_append_u64(char *dst, uint32_t *pos, uint32_t cap, uint64_t value)
{
    char tmp[24];
    uint32_t n = 0;
    if (!value) {
        if (*pos + 1u < cap) { dst[*pos] = '0'; ++(*pos); dst[*pos] = 0; }
        return;
    }
    while (value && n + 1u < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (n) {
        if (*pos + 1u >= cap) return;
        dst[*pos] = tmp[--n];
        ++(*pos);
        dst[*pos] = 0;
    }
}

static void proc_append_text(char *dst, uint32_t *pos, uint32_t cap, const char *src)
{
    while (src && *src) {
        if (*pos + 1u >= cap) return;
        dst[*pos] = *src++;
        ++(*pos);
        dst[*pos] = 0;
    }
}

static int proc_asound_card_path(const char *path, uint32_t *ordinal,
                                 const char **leaf)
{
    const char *p;
    uint32_t value = 0;
    if (!path || __builtin_strncmp(path, "/proc/asound/card", 17) != 0)
        return 0;
    p = path + 17;
    if (*p < '0' || *p > '9') return 0;
    do {
        uint32_t digit = (uint32_t)(*p++ - '0');
        if (value > (UINT32_MAX - digit) / 10u) return 0;
        value = value * 10u + digit;
    } while (*p >= '0' && *p <= '9');
    if (ordinal) *ordinal = value;
    if (leaf) *leaf = *p == '/' ? p + 1 : p;
    return *p == 0 || *p == '/';
}

static int proc_asound_card_exists(uint32_t ordinal)
{
    uint32_t token;
    return sound_service_card_snapshot(ordinal, &token, NULL) == 0;
}

/** @brief Identify an explicit per-thread directory without libc substring calls.
 * @param path Required intrinsic key. @return True for task/TID or thread-self paths.
 */
static bool proc_thread_path(const char *path)
{
    if (!__builtin_strncmp(path, "/proc/thread-self", 17))
        return true;
    for (const char *p = path; *p; ++p)
        if (!__builtin_strncmp(p, "/task/", 6))
            return true;
    return false;
}

/** @brief Parse process/thread keys and validate thread-group membership.
 * @param path Nullable intrinsic proc pathname. @param file_name Optional leaf output.
 * @param pid Optional resolved PID/TID output, cleared on failure.
 * @return Two for a valid task-shaped key, zero otherwise; no lifetime transfer.
 */
static int proc_path_kind(const char *path, const char **file_name, uint32_t *pid)
{
    if (pid)
        *pid = 0;
    if (file_name)
        *file_name = NULL;
    if (!path || __builtin_strncmp(path, "/proc/", 6))
        return 0;
    const char *p = path + 6;
    struct task *task = NULL;
    uint32_t value = 0;
    bool thread_view = false;
    if (!__builtin_strncmp(p, "self", 4) && (!p[4] || p[4] == '/')) {
        task = sched_current_task();
        if (task)
            value = sched_task_tgid(task);
        p += 4;
    } else if (!__builtin_strncmp(p, "thread-self", 11) && (!p[11] || p[11] == '/')) {
        task = sched_current_task();
        if (task)
            value = task->pid;
        p += 11;
        thread_view = true;
    } else {
        if (*p < '1' || *p > '9')
            return 0;
        while (*p >= '0' && *p <= '9') {
            uint32_t digit = (uint32_t)(*p++ - '0');
            if (value > (UINT32_MAX - digit) / 10)
                return 0;
            value = value * 10 + digit;
        }
        if (*p && *p != '/')
            return 0;
    }
    if (!value)
        return 0;
    task = sched_find(value);
    if (task && task->pid != sched_task_tgid(task))
        thread_view = true;
    if (thread_view &&
        (*p && (!__builtin_strcmp(p, "/task") || !__builtin_strncmp(p, "/task/", 6))))
        return 0;
    if (*p == '/' && !__builtin_strncmp(p + 1, "task/", 5)) {
        struct task *leader = sched_find(value);
        p += 6;
        uint32_t tid = 0;
        if (!leader || *p < '1' || *p > '9')
            return 0;
        while (*p >= '0' && *p <= '9') {
            uint32_t d = *p++ - '0';
            if (tid > (UINT32_MAX - d) / 10)
                return 0;
            tid = tid * 10 + d;
        }
        if (*p && *p != '/')
            return 0;
        task = sched_find(tid);
        if (!task || sched_task_tgid(task) != sched_task_tgid(leader))
            return 0;
        value = tid;
        if (*p && (!__builtin_strcmp(p, "/task") || !__builtin_strncmp(p, "/task/", 6)))
            return 0;
    }
    if (pid)
        *pid = value;
    if (file_name && *p == '/')
        *file_name = p + 1;
    return 2;
}

/** @brief Borrow a validated task and leaf while execution prevents reaping.
 * @param path Proc key. @param leaf Required output. @return Task or NULL.
 */
struct task *proc_view_task(const char *path, const char **leaf)
{
    uint32_t pid = 0;
    return proc_path_kind(path, leaf, &pid) ? sched_find(pid) : NULL;
}

/** @brief Preserve the established private status fields alongside Linux fields.
 * @param task Borrowed live task. @param stream Required output stream.
 * Execution-owned metadata only; no allocation or retained references.
 */
void proc_task_identity_fields(const struct task *task, struct text_stream *stream)
{
    text_string(stream, "LeonOSRole:\t");
    text_unsigned(stream, task->role);
    text_string(stream, "\nLeonOSFlags:\t");
    text_unsigned(stream, task->flags);
    text_string(stream, "\n");
}

/**
 * @brief Fill buffer with the complete content of a fixed proc file.
 * @param path Absolute kernel pathname of the fixed proc file.
 * @param buffer Writable kernel buffer, always NUL-terminated on return.
 * @param capacity Nonzero byte capacity of buffer.
 * @return Zero with the file content in buffer, or negative ENOENT for paths
 * this generator does not own.
 */
static int proc_fill_content(const char *path, char *buffer, uint32_t capacity)
{
    uint32_t pos = 0;
    buffer[0] = 0;
    if (proc_text_eq(path, "/proc/cmdline")) {
        /* Linux cmdline_proc_show prints saved_command_line as one line. */
        const char *cmdline = userland_boot_cmdline();
        if (cmdline && cmdline[0]) {
            proc_append_text(buffer, &pos, capacity, cmdline);
            proc_append_text(buffer, &pos, capacity, "\n");
        }
        return 0;
    }
    if (proc_text_eq(path, "/proc/uptime")) {
        uint64_t busy, idle;
        sched_cpu_ticks(&busy, &idle);
        uint64_t ms = time_uptime_ms();
        proc_append_u64(buffer, &pos, capacity, ms / 1000u);
        proc_append_text(buffer, &pos, capacity, ".");
        proc_append_u64(buffer, &pos, capacity, (ms % 1000u) / 100u);
        proc_append_text(buffer, &pos, capacity, " ");
        proc_append_u64(buffer, &pos, capacity, idle / RELIEFNT_TICK_HZ);
        proc_append_text(buffer, &pos, capacity, ".");
        uint64_t fraction = (idle % RELIEFNT_TICK_HZ) * 100 / RELIEFNT_TICK_HZ;
        if (fraction < 10) proc_append_text(buffer, &pos, capacity, "0");
        proc_append_u64(buffer, &pos, capacity, fraction);
        proc_append_text(buffer, &pos, capacity, "\n");
        return 0;
    }
    if (proc_text_eq(path, "/proc/version")) {
        const struct reliefos_system_info *info = reliefnt_system_info();
        if (info) proc_append_text(buffer, &pos, capacity, info->kernel_name);
        proc_append_text(buffer, &pos, capacity, " version ");
        if (info) proc_append_text(buffer, &pos, capacity, info->kernel_version);
        proc_append_text(buffer, &pos, capacity, " (");
        if (info) proc_append_text(buffer, &pos, capacity, info->build_time);
        proc_append_text(buffer, &pos, capacity, ")\n");
        return 0;
    }
    if (proc_text_eq(path, "/proc/filesystems")) {
        proc_append_text(buffer, &pos, capacity,
                         "\text2\n\text4\n\tvfat\n\texfat\n\tiso9660\nnodev\tproc\nnodev\tdevfs\nnodev\tsysfs\nnodev\ttmpfs\n");
        return 0;
    }
    if (proc_text_eq(path, "/proc/asound/cards")) {
        uint32_t ordinal = 0;
        struct audio_card_identity identity;
        while (sound_service_card_snapshot(ordinal, NULL, &identity) == 0) {
            proc_append_u64(buffer, &pos, capacity, ordinal);
            proc_append_text(buffer, &pos, capacity, " [");
            proc_append_text(buffer, &pos, capacity, identity.id);
            proc_append_text(buffer, &pos, capacity, "]: ");
            proc_append_text(buffer, &pos, capacity, identity.name);
            proc_append_text(buffer, &pos, capacity, "\n");
            ++ordinal;
        }
        return 0;
    }
    if (proc_text_eq(path, "/proc/sys/kernel/hostname") ||
        proc_text_eq(path, "/proc/sys/kernel/domainname")) {
        char hostname[65], domainname[65];
        linux_uts_names(hostname, domainname);
        proc_append_text(buffer, &pos, capacity,
                         proc_text_eq(path, "/proc/sys/kernel/hostname") ? hostname : domainname);
        proc_append_text(buffer, &pos, capacity, "\n");
        return 0;
    }
    if (proc_text_eq(path, "/proc/sys/kernel/ostype") ||
        proc_text_eq(path, "/proc/sys/kernel/osrelease") ||
        proc_text_eq(path, "/proc/sys/kernel/version")) {
        const struct reliefos_system_info *info = reliefnt_system_info();
        const char *value = proc_text_eq(path, "/proc/sys/kernel/ostype") ? LINUX_UTS_SYSNAME :
            !info ? "" :
            proc_text_eq(path, "/proc/sys/kernel/version") ? info->build_time :
            info->kernel_version;
        proc_append_text(buffer, &pos, capacity, value);
        proc_append_text(buffer, &pos, capacity, "\n");
        return 0;
    }
    /* Linux kernel.overflowuid/overflowgid: the IDs substituted for unmapped
     * 16-bit uids/gids. The upstream default is 65534. */
    if (proc_text_eq(path, "/proc/sys/kernel/overflowuid") ||
        proc_text_eq(path, "/proc/sys/kernel/overflowgid")) {
        proc_append_text(buffer, &pos, capacity, "65534\n");
        return 0;
    }
    return -2;
}

/**
 * @brief Parse a proc fd link without accepting overflow or trailing text.
 * @param file Task-relative pathname, nullable.
 * @return Descriptor number or negative ENOENT.
 */
static int proc_fd_number(const char *file)
{
    uint32_t fd = 0;
    if (!file || __builtin_strncmp(file, "fd/", 3) || !file[3]) return -2;
    for (const char *p = file + 3; *p; ++p) {
        if (*p < '0' || *p > '9' || fd >= SCHED_NR_OPEN) return -2;
        fd = fd * 10 + (uint32_t)(*p - '0');
    }
    return fd < SCHED_NR_OPEN ? (int)fd : -2;
}

/** @brief Render a retained inode relative to the caller's actual root.
 * @param object Borrowed held object; anonymous provider labels are allowed.
 * @param target Writable diagnostic output, including a terminating NUL.
 * @param capacity Available bytes including NUL and any deleted suffix.
 * @return Zero or negative errno, without namespace lookup or I/O. Execution
 * ownership protects the caller root; temporary path references are balanced.
 */
int proc_object_target(const struct vfs_path *object, char *target, uint32_t capacity)
{
    struct vfs_namespace *ns=vfs_path_namespace(object);
    if(!ns)return vfs_path_format_link(NULL,NULL,object,target,capacity);
    struct task *caller=sched_current_task();struct vfs_path root={0};int rc=0;
    const struct vfs_path *caller_root=caller ? &sched_task_paths(caller)->root : NULL;
    if(caller_root && vfs_path_node(caller_root))vfs_path_get(caller_root,&root);
    else rc=vfs_namespace_root(ns,&root);
    if(!rc)rc=vfs_path_format_link(ns,&root,object,target,capacity);
    vfs_path_put(&root);return rc;
}

/**
 * @brief Resolve named file and terminal descriptors for procfs readlink.
 * @param task Existing task whose descriptor table is protected by the execution lock.
 * @param fd Descriptor number.
 * @param target Writable diagnostic buffer.
 * @param capacity Bytes available in target, including NUL.
 * @return Zero on success, negative ENOENT for closed or unnamed descriptors.
 */
static int proc_fd_target(struct task *task, int fd, char *target,uint32_t capacity)
{
    struct task_pty_fd *pty = task_pty_fd_for_fd(task, fd);
    if (pty && pty->pty_id && pty->endpoint) {
        if(pty->description && vfs_path_node(&pty->description->path_ref)) {
            int rc=proc_object_target(&pty->description->path_ref,target,capacity);
            if(rc!=-EOPNOTSUPP)return rc;
        }
        const char *prefix = pty->endpoint == TASK_PTY_ENDPOINT_MASTER ? "/dev/ptmx" :
            pty_vt_number(pty->pty_id) ? "/dev/tty" : "/dev/pts/";
        proc_copy(target, capacity, prefix);
        if (pty->endpoint == TASK_PTY_ENDPOINT_SLAVE) {
            uint32_t pos = 0;
            while (target[pos]) ++pos;
            proc_append_u64(target, &pos, capacity, pty->pty_id);
        }
        return 0;
    }
    struct task_file *file = task_file_for_fd(task, fd);
    if (file && file->socket_ino) {
        uint32_t pos = 0;
        target[0] = 0;
        proc_append_text(target, &pos, capacity, "socket:[");
        proc_append_u64(target, &pos, capacity, file->socket_ino);
        proc_append_text(target, &pos, capacity, "]");
        return 0;
    }
    if(file && vfs_path_node(&file->path_ref)) {
        int rc=proc_object_target(&file->path_ref,target,capacity);
        if(rc!=-EOPNOTSUPP)return rc;
    }
    if (!file || !file->path[0]) return -2;
    proc_copy(target, capacity, file->path);
    return 0;
}

/** @brief Recognize a live process mount view and optionally return its task.
 * @param path Intrinsic proc inode key, independent of visible attachment.
 * @param target Optional borrowed target task output, cleared on failure.
 * @return One for mounts, two for mountinfo, zero otherwise. The caller holds
 * execution ownership; task identity is separately pinned by the proc inode.
 */
static int proc_mount_view(const char *path,struct task **target)
{
    if(target)*target=NULL;
    const char *file = NULL;
    uint32_t pid = 0;
    int kind = proc_path_kind(path, &file, &pid);
    if (kind == 3) pid = sched_current_pid();
    struct task *task=pid ? sched_find(pid) : NULL;
    if ((kind != 2 && kind != 3) || !file || !task) return 0;
    int view=proc_text_eq(file, "mounts") ? 1 : proc_text_eq(file, "mountinfo") ? 2 : 0;
    if(view && target)*target=task;
    return view;
}

struct proc_open_view {
    struct vfs_namespace *ns; struct vfs_path root; bool mountinfo;
    char *text;
    uint64_t text_size;
#ifdef CONFIG_LOCK_STAT
    void *lock_stats;
#endif
};

/** @brief Capture a mount view, generated leaf or diagnostic snapshot at open.
 * @param name Intrinsic authorized proc inode key, independent of attachment.
 * @param flags Sanitized status flags, borrowed and unchanged.
 * @param out Required owned output; legacy stateless files return NULL.
 * @return Zero or negative errno with empty output on failure. Mount views
 * retain a root path; generated leaves and diagnostic readers capture bytes.
 * Namespace ownership outlives its retained paths; final release drops this
 * path even after task exit. Execution-owned, without graph locks across
 * renderers; partition metadata may perform I/O and return its native errno.
 */
int proc_file_open(const char *name,uint32_t flags,void **out)
{
    (void)flags;if(!out)return -RELIEFOS_EINVAL;*out=NULL;
#ifdef CONFIG_LOCK_STAT
    if (proc_text_eq(name, "/proc/reliefnt/lock_stats")) {
        /* Controls need no view and must remain usable under memory pressure. */
        if ((flags & LINUX_O_ACCMODE) == LINUX_O_WRONLY) return 0;
        struct proc_open_view *state=kernel_malloc(sizeof(*state));
        if (!state) return -RELIEFOS_ENOMEM;
        *state=(struct proc_open_view){0};
        int rc=lock_profile_view_new(&state->lock_stats);
        if (rc) { kernel_free(state); return rc; }
        *out=state; return 0;
    }
    if ((flags & LINUX_O_ACCMODE) != LINUX_O_RDONLY || (flags & LINUX_O_TRUNC))
        return -RELIEFOS_EROFS;
#endif
    if (proc_extra_kind(name) == 1) {
        struct proc_open_view *state = kernel_malloc(sizeof(*state));
        if (!state)
            return -RELIEFOS_ENOMEM;
        *state = (struct proc_open_view){0};
        uint32_t capacity = 4096;
        for (;;) {
            state->text = kernel_malloc(capacity);
            if (!state->text) {
                kernel_free(state);
                return -RELIEFOS_ENOMEM;
            }
            struct text_stream stream = {.buffer = state->text, .capacity = capacity};
            int rc = proc_extra_emit(name, &stream);
            if (rc) {
                kernel_free(state->text);
                kernel_free(state);
                return rc;
            }
            if (stream.position <= capacity) {
                state->text_size = stream.written;
                break;
            }
            kernel_free(state->text);
            state->text = NULL;
            if (stream.position > UINT32_MAX / 2) {
                kernel_free(state);
                return -RELIEFOS_ENOMEM;
            }
            capacity = (uint32_t)stream.position * 2;
        }
        *out = state;
        return 0;
    }
    struct task *task=NULL;int view=proc_mount_view(name,&task);if(!view)return 0;
    const struct vfs_path *root=&sched_task_paths(task)->root;
    struct vfs_namespace *ns=vfs_path_namespace(root);if(!ns)return -RELIEFOS_ENOENT;
    struct proc_open_view *state=kernel_malloc(sizeof(*state));if(!state)return -RELIEFOS_ENOMEM;
    *state=(struct proc_open_view){.ns=ns,.mountinfo=view==2};vfs_path_get(root,&state->root);*out=state;return 0;
}

/** @brief Read a captured mount view or diagnostic snapshot.
 * @param value Required borrowed capture, independent of target PID liveness.
 * @param offset Byte position in the mount graph or fixed diagnostic view.
 * @param buffer Kernel-accessible destination, nullable at zero capacity.
 * @param capacity Available output bytes.
 * @param read Required output count, cleared before validation.
 * @return Renderer status or negative EINVAL. Does not look up a PID, change
 * the captured root or diagnostic data, or transfer ownership. Execution-owned,
 * no graph locks across renderer allocation or filesystem policy callbacks.
 */
int proc_file_read(void *value,uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read)
{
    if(!read)return -RELIEFOS_EINVAL;*read=0;
    struct proc_open_view *state=value;if(!state)return -RELIEFOS_EINVAL;
#ifdef CONFIG_LOCK_STAT
    if (state->lock_stats) return lock_profile_view_read(state->lock_stats,offset,buffer,capacity,read);
#endif
    if (state->text) {
        if (!buffer && capacity)
            return -RELIEFOS_EINVAL;
        uint64_t n = offset < state->text_size ? state->text_size - offset : 0;
        if (n > capacity)
            n = capacity;
        if (n)
            __builtin_memcpy(buffer, state->text + offset, n);
        *read = (uint32_t)n;
        return 0;
    }
    return vfs_read_mount_table(state->ns,&state->root,state->mountinfo,offset,buffer,capacity,read);
}

/** @brief Release an open view after the final shared OFD reference.
 * @param value Owned optional captured state; NULL is ignored.
 * @return None. Root/provider release can perform I/O; caller holds execution
 * ownership without a graph, scheduler or module-table spinlock.
 */
void proc_file_close(void *value)
{
    struct proc_open_view *state=value;if(!state)return;
#ifdef CONFIG_LOCK_STAT
    if (state->lock_stats) { lock_profile_view_free(state->lock_stats); kernel_free(state); return; }
#endif
    kernel_free(state->text);
    vfs_path_put(&state->root);
    kernel_free(state);
}

/** @brief Map scheduler states to Linux proc characters, never expose private enums. */
static char proc_state(const struct task *task)
{
    return task->state == TASK_EXITED    ? 'Z'
           : task->state == TASK_STOPPED ? 'T'
           : task->state == TASK_BLOCKED ? 'S'
                                         : 'R';
}
/**
 * @brief Read argv or environment bytes from the original exec stack range.
 * @param task Target task pinned by the kernel execution lock.
 * @param s Output slice; offsets and lengths are bytes, including embedded NULs.
 * @param environment Select environment instead of argument bounds.
 * @return Zero or EIO when an unreadable page precedes all output.
 */
static int proc_mm_strings(struct task *task, struct text_stream *s, bool environment)
{
    struct task_address_space_state *mm = sched_task_mm(task);
    uint64_t start = environment ? mm->env_start : mm->arg_start;
    uint64_t end = environment ? mm->env_end : mm->arg_end;
    if (task->state == TASK_EXITED || !mm->as.cr3 || end <= start)
        return 0;
    uint64_t size = end - start;
    if (s->offset >= size)
        return 0;
    uint64_t position = start + s->offset;
    while (position < end && s->written < s->capacity) {
        if (!address_space_user_page_readable(&mm->as, position))
            return s->written ? 0 : -5;
        uint64_t phys = address_space_user_page_phys(&mm->as, position);
        const char *page = paging_kernel_direct_map(phys);
        if (!phys || !page)
            return s->written ? 0 : -5;
        uint32_t count = 4096 - (uint32_t)(position & 4095);
        if (count > end - position)
            count = (uint32_t)(end - position);
        if (count > s->capacity - s->written)
            count = s->capacity - s->written;
        __builtin_memcpy(s->buffer + s->written, page + (position & 4095), count);
        s->written += count;
        position += count;
    }
    return 0;
}
/** @brief Linux PTRACE_MODE_READ_FSCREDS gate for stat address fields. */
bool proc_stat_mm_visible(struct task *caller, struct task *target)
{
    if (!caller)
        return false;
    if (sched_task_tgid(caller) == sched_task_tgid(target))
        return true;
    bool privileged = (caller->cap_effective & (1ULL << CAP_SYS_PTRACE)) != 0;
    if (!privileged && (caller->fsuid != target->uid || caller->fsuid != target->euid ||
                        caller->fsuid != target->suid || caller->fsgid != target->gid ||
                        caller->fsgid != target->egid || caller->fsgid != target->sgid ||
                        sched_task_mm(target)->nondumpable))
        return false;
    return privileged || !(target->cap_permitted & ~caller->cap_effective);
}

/** @brief Pin the exact file path referenced by an authorized process FD.
 * @param target Borrowed task, stable under the execution transaction.
 * @param fd Descriptor in that task, with no borrowed OFD escaping the call.
 * @param out Empty output receiving one independently owned path reference.
 * @return Zero, EACCES before inspecting a foreign FD, ENOENT for missing task,
 * descriptor or object path, or EINVAL for a missing output. Display names do
 * not participate; detached shmem mounts and unlinked inodes remain valid.
 */
int proc_fd_get_path(struct task *target,int fd,struct vfs_path *out)
{
    if(!out)return -RELIEFOS_EINVAL;
    *out=(struct vfs_path){0};
    if(!target)return -RELIEFOS_ENOENT;
    if(!proc_stat_mm_visible(sched_current_task(),target))return -RELIEFOS_EACCES;
    struct task_file *file=task_file_for_fd(target,fd);
    if(!file || !file->path_ref.mount)return -RELIEFOS_ENOENT;
    vfs_path_get(&file->path_ref,out);return 0;
}

int proc_mount_node_identity(const char *name,struct task_identity **out,bool *magic)
{
    if(!out || !magic)return -RELIEFOS_EINVAL;
    *out=NULL;*magic=false;
    const char *file=NULL;uint32_t pid=0;
    int kind=proc_path_kind(name,&file,&pid);
    if(kind!=2 && !(kind==3 && file))return 0;
    struct task *task=kind==3 ? sched_current_task() : sched_find(pid);
    int rc=sched_task_identity_get(task,out);
    if(rc)return rc==-ESRCH ? -RELIEFOS_ENOENT : rc;
    *magic=file && (proc_fd_number(file)>=0 || proc_text_eq(file,"cwd") || proc_text_eq(file,"root") || proc_text_eq(file,"exe"));
    return 0;
}

int proc_mount_magic_target(const char *name,struct vfs_path *out)
{
    if(!out)return -RELIEFOS_EINVAL;
    *out=(struct vfs_path){0};
    const char *file=NULL;uint32_t pid=0;int kind=proc_path_kind(name,&file,&pid);
    if((kind!=2 && kind!=3) || !file)return -RELIEFOS_EINVAL;
    struct task *task=kind==3 ? sched_current_task() : sched_find(pid);
    if(!task)return -RELIEFOS_ENOENT;
    int fd=proc_fd_number(file);if(fd>=0)return proc_fd_get_path(task,fd,out);
    if(!proc_stat_mm_visible(sched_current_task(),task))return -RELIEFOS_EACCES;
    struct task_fs_paths *paths=sched_task_paths(task);
    if(proc_text_eq(file,"cwd"))vfs_path_get(&paths->cwd,out);
    else if(proc_text_eq(file,"root"))vfs_path_get(&paths->root,out);
    else if(proc_text_eq(file,"exe"))vfs_path_get(&sched_task_mm(task)->executable_path,out);
    else return -RELIEFOS_EINVAL;
    return out->mount ? 0 : -RELIEFOS_ENOENT;
}
/** @brief Emit Linux's ordered 52 stat fields using available task accounting.
 * Legacy private uid/role fields belong in status rather than Linux stat positions.
 * @param task Borrowed live target. @param s Required output stream.
 * @param process Sum CPU times across the group; false selects a thread.
 * Execution ownership protects task/VMA metadata; CPU counters use their lock.
 */
static void proc_task_stat(struct task *task, struct text_stream *s, bool process)
{
    uint64_t values[53] = {0};
    struct task_address_space_state *mm = sched_task_mm(task);
    uint32_t threads = 0;
    struct task *other;
    for (uint32_t i = 0; (other = sched_task_at(i)); ++i) {
        if (other->pid && sched_task_tgid(other) == sched_task_tgid(task)) {
            ++threads;
        }
    }
    uint32_t foreground = 0;
    if (task->controlling_pty_id && pty_get_foreground_pgid(task->controlling_pty_id, &foreground) == 0)
        values[7] = (136u << 8) | (task->controlling_pty_id - 1);
    values[4] = task->parent_pid;
    values[5] = task->process_group;
    values[6] = task->process_session;
    values[8] = values[7] ? foreground : (uint64_t)-1;
    sched_task_cpu_times(task, process, &values[14], &values[15]);
    values[18] = 20 + task->priority;
    values[19] = task->priority;
    values[20] = threads;
    values[22] = task->start_uptime_ms / 10;
    for (uint32_t i = 0; i < SCHED_TASK_VMA_MAX + mm->vma_extra_count; ++i) {
        const struct task_vma *vma = sched_task_vma_at(task, i);
        if (vma && vma->used)
            values[23] += vma->end - vma->start;
    }
    values[24] = address_space_user_resident_kib(&mm->as) / 4;
    values[25] = UINT64_MAX;
    values[28] = task->stack_top;
    values[31] = sched_task_pending(task) & 0x7fffffff;
    values[32] = task->blocked_signals & 0x7fffffff;
    values[38] = task->parent_exit_signal;
    values[39] = task->last_cpu;
    values[47] = mm->program_break_base;
    values[48] = mm->arg_start;
    values[49] = mm->arg_end;
    values[50] = mm->env_start;
    values[51] = mm->env_end;
    values[52] = task->state == TASK_EXITED
                     ? (task->exit_signal ? task->exit_signal & 0x7f : (task->exit_code & 0xff) << 8)
                     : 0;
    if (!proc_stat_mm_visible(sched_current_task(), task)) {
        values[28] = 0;
        for (uint32_t field = 45; field <= 52; ++field)
            values[field] = 0;
    }
    text_unsigned(s, task->pid);
    text_string(s, " (");
    const char *name = task->name ? task->name : "?";
    uint32_t length = 0;
    while (name[length] && length < 15)
        ++length;
    text_bytes(s, name, length);
    text_string(s, ") ");
    char state = proc_state(task);
    text_bytes(s, &state, 1);
    for (uint32_t field = 4; field <= 52; ++field) {
        text_string(s, " ");
        if (field == 8 || field == 18 || field == 19)
            text_signed(s, (int64_t)values[field]);
        else
            text_unsigned(s, values[field]);
    }
    text_string(s, "\n");
}

/** @brief Look up proc node types without following the last symbolic link.
 * @param path Borrowed absolute intrinsic pathname, NULL is rejected.
 * @param out Optional caller-owned writable node metadata.
 * @return Zero or negative ENOENT. Execution-owned; no reference transfers. */
int proc_lookup(const char *path, struct storage_node *out)
{
    int extra = proc_extra_kind(path);
    if (extra) {
        if (out)
            *out = (struct storage_node){.type = extra == 2   ? RELIEFOS_FS_TYPE_DIR
                                                 : extra == 3 ? RELIEFOS_FS_TYPE_SYMLINK
                                                              : RELIEFOS_FS_TYPE_FILE,
                                         .flags = STORAGE_NODE_FLAG_PROC};
        return 0;
    }
    char content[PROCFS_CONTENT_MAX];
    if (path && !__builtin_strncmp(path,"/sys",4) && (!path[4] || path[4]=='/')) return sysfs_lookup(path,out);
    if (!path || (path[0] != '/' || path[1] != 'p' || path[2] != 'r' ||
                  path[3] != 'o' || path[4] != 'c' || (path[5] && path[5] != '/'))) return -2;
    if (proc_text_eq(path, "/proc/mounts")) {
        if (out) *out = (struct storage_node){.type = RELIEFOS_FS_TYPE_SYMLINK,
            .flags = STORAGE_NODE_FLAG_PROC, .size = 11};
        return 0;
    }
#ifdef CONFIG_LOCK_STAT
    if (proc_text_eq(path, "/proc/reliefnt/lock_stats")) {
        if (out) *out = (struct storage_node){.type = RELIEFOS_FS_TYPE_FILE, .flags = STORAGE_NODE_FLAG_PROC};
        return 0;
    }
#endif
    if (proc_text_eq(path, "/proc") || proc_text_eq(path, "/proc/sys") ||
        proc_text_eq(path, "/proc/sys/kernel") ||
        proc_text_eq(path, "/proc/asound")
#ifdef CONFIG_LOCK_STAT
        || proc_text_eq(path, "/proc/reliefnt")
#endif
       ) {
        if (out) {
            *out = (struct storage_node){
                .type = RELIEFOS_FS_TYPE_DIR,
                .flags = STORAGE_NODE_FLAG_PROC,
                .first_cluster = 0x50524f43u, /* PROC */
                .volume_id = 0,
                .size = 0,
            };
        }
        return 0;
    }
    {
        uint32_t ordinal;
        const char *leaf;
        if (proc_asound_card_path(path, &ordinal, &leaf) &&
            *leaf == 0 && proc_asound_card_exists(ordinal)) {
            if (out) *out = (struct storage_node){
                .type = RELIEFOS_FS_TYPE_DIR,
                .flags = STORAGE_NODE_FLAG_PROC,
                .first_cluster = 0x41534443u,
            };
            return 0;
        }
    }
#ifdef CONFIG_MODULES
    if(proc_text_eq(path,"/proc/modules")) {
        if(out) *out=(struct storage_node){.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_PROC,
            .first_cluster=0x50524f43u,.size=0};
        return 0;
    }
#endif
    if (proc_mount_view(path,NULL) || proc_text_eq(path,"/proc/cpuinfo") || proc_text_eq(path,"/proc/leonos-drivers")) {
        if (out) *out = (struct storage_node){
            .type = RELIEFOS_FS_TYPE_FILE, .flags = STORAGE_NODE_FLAG_PROC,
            .first_cluster = 0x50524f43u, .size = 0,
        };
        return 0;
    }
    if (proc_text_eq(path, "/proc/cmdline") || proc_text_eq(path, "/proc/uptime") ||
        proc_text_eq(path, "/proc/version") || proc_text_eq(path, "/proc/filesystems") ||
        proc_text_eq(path, "/proc/asound/cards") ||
        proc_text_eq(path, "/proc/sys/kernel/hostname") ||
        proc_text_eq(path, "/proc/sys/kernel/domainname") ||
        proc_text_eq(path, "/proc/sys/kernel/ostype") ||
        proc_text_eq(path, "/proc/sys/kernel/osrelease") ||
        proc_text_eq(path, "/proc/sys/kernel/version") ||
        proc_text_eq(path, "/proc/sys/kernel/overflowuid") ||
        proc_text_eq(path, "/proc/sys/kernel/overflowgid")) {
        uint32_t len = 0;
        int ret = proc_fill_content(path, content, sizeof(content));
        if (ret < 0) return ret;
        while (content[len]) ++len;
        if (out) {
            *out = (struct storage_node){
                .type = RELIEFOS_FS_TYPE_FILE,
                .flags = STORAGE_NODE_FLAG_PROC,
                .first_cluster = 0x50524f43u,
                .volume_id = 0,
                .size = 0,
            };
        }
        return 0;
    }
    {
        const char *file = 0;
        uint32_t pid = 0;
        int kind = proc_path_kind(path, &file, &pid);
        if ((kind == 2 || kind == 3) && file && proc_text_eq(file, "fd")) {
            struct task *task = kind == 3 ? sched_current_task() : sched_find(pid);
            if (!task) return -RELIEFOS_ENOENT;
            if (out) *out = (struct storage_node){.type = RELIEFOS_FS_TYPE_DIR,
                .flags = STORAGE_NODE_FLAG_PROC};
            return 0;
        }
        if ((kind == 2 || kind == 3) && proc_fd_number(file) >= 0) {
            char target[RELIEFOS_FS_PATH_LEN];
            int ret = proc_readlink(path, target, sizeof(target));
            if (ret < 0) return ret;
            if (out) *out = (struct storage_node){.type = RELIEFOS_FS_TYPE_SYMLINK,
                .flags = STORAGE_NODE_FLAG_PROC};
            return 0;
        }
        if ((kind == 2 || kind == 3) && file &&
            (proc_text_eq(file, "exe") || proc_text_eq(file, "cwd") || proc_text_eq(file, "root"))) {
            if (kind == 3) pid = sched_current_pid();
            if (!pid || !sched_find(pid)) return -2;
            if (out) *out = (struct storage_node){.type = RELIEFOS_FS_TYPE_SYMLINK,
                .flags = STORAGE_NODE_FLAG_PROC};
            return 0;
        }
        if ((kind == 2 || kind == 3) && !file) {
            if (kind == 3) pid = sched_current_pid();
            if (!pid || !sched_find(pid)) return -2;
            if (out) *out = (struct storage_node){
                .type = kind == 3 ? RELIEFOS_FS_TYPE_SYMLINK : RELIEFOS_FS_TYPE_DIR,
                .flags = STORAGE_NODE_FLAG_PROC,
                .first_cluster = 0x50524f43u,
            };
            return 0;
        }
        if ((kind == 2 || kind == 3) && file &&
            (proc_text_eq(file, "stat") || proc_text_eq(file, "comm") || proc_text_eq(file, "cmdline") || proc_text_eq(file, "environ") || proc_text_eq(file, "status"))) {
            if (kind == 2 && !sched_find(pid)) return -2;
            if (out) {
                *out = (struct storage_node){
                    .type = RELIEFOS_FS_TYPE_FILE,
                    .flags = STORAGE_NODE_FLAG_PROC,
                    .first_cluster = 0x50524f43u,
                    .volume_id = 0,
                    .size = 0,
                };
            }
            return 0;
        }
    }
    return -2;
}

/** @brief Read a bounded range from a current proc view.
 * @param path Borrowed resolved intrinsic pathname.
 * @param offset Byte offset, including positions past EOF.
 * @param buffer Writable validated destination, nullable for zero length.
 * @param length Maximum bytes to copy.
 * @param out_read Optional caller-owned writable actual byte count.
 * @return Zero or negative errno, preserving backend failures. Execution-owned;
 * retains no caller memory and live generated views can change between reads. */
int proc_read(const char *path, uint64_t offset, void *buffer, uint32_t length,
              uint32_t *out_read)
{
    if (out_read)
        *out_read = 0;
    if (!path || (!buffer && length))
        return -RELIEFOS_EINVAL;
    if (proc_extra_kind(path) == 1) {
        struct text_stream stream = {.offset = offset, .buffer = buffer, .capacity = length};
        int rc = proc_extra_emit(path, &stream);
        if (!rc && out_read)
            *out_read = stream.written;
        return rc;
    }
    if (path && !__builtin_strncmp(path,"/sys",4) && (!path[4] || path[4]=='/')) return sysfs_read(path,offset,buffer,length,out_read);
#ifdef CONFIG_LOCK_STAT
    if (proc_text_eq(path, "/proc/reliefnt/lock_stats")) {
        uint32_t ignored;
        return lock_profile_read(offset, buffer, length, out_read ? out_read : &ignored);
    }
#endif
#ifdef CONFIG_MODULES
    if(proc_text_eq(path,"/proc/modules")) return module_proc_read(offset,buffer,length,out_read);
#endif
    if (proc_text_eq(path,"/proc/cpuinfo")) return cpu_inventory_read(offset,buffer,length,out_read);
    if (out_read) *out_read = 0;
    if (!buffer && length) return -22;
    /* Read-only, versioned driver records from the actual driver manager;
     * no user pointer reaches driver_manager_list. */
    if (proc_text_eq(path, "/proc/leonos-drivers")) {
        struct reliefos_driver_info drivers[RELIEFOS_DRIVER_MAX];
        struct reliefos_driver_list query = {.drivers = drivers, .capacity = RELIEFOS_DRIVER_MAX};
        int ret = driver_manager_list(&query);
        if (ret < 0) return ret;
        uint64_t bytes = (uint64_t)query.count * sizeof(drivers[0]);
        if (offset >= bytes) return 0;
        if (length > bytes - offset) length = (uint32_t)(bytes - offset);
        __builtin_memcpy(buffer, (const char *)drivers + offset, length);
        if (out_read) *out_read = length;
        return 0;
    }
    const char *file=0; uint32_t pid=0; int kind=proc_path_kind(path,&file,&pid);
    if(kind==3) pid=sched_current_pid();
    if((kind==2 || kind==3) && file && (proc_text_eq(file,"cmdline") || proc_text_eq(file,"environ") || proc_text_eq(file,"stat") || proc_text_eq(file,"comm"))) {
        struct task *task=sched_find(pid); if(!task) return -2;
        struct text_stream stream={.offset=offset,.buffer=buffer,.capacity=length};
        int ret=0;
        if(proc_text_eq(file,"environ")) {
            if (!proc_stat_mm_visible(sched_current_task(), task)) return -RELIEFOS_EACCES;
            ret=proc_mm_strings(task,&stream,true);
        }
        else if(proc_text_eq(file,"cmdline")) ret=proc_mm_strings(task,&stream,false);
        else if (proc_text_eq(file, "stat"))
            proc_task_stat(task, &stream,
                           sched_task_tgid(task) == task->pid && !proc_thread_path(path));
        else { const char *name=task->name?task->name:"?"; uint32_t n=0; while(name[n] && n<15) ++n;
            text_bytes(&stream,name,n); text_string(&stream,"\n"); }
        if (out_read) *out_read = stream.written;
        return ret;
    }
    struct task *mount_task=NULL;
    int view=proc_mount_view(path,&mount_task);
    if(view) {
        const struct vfs_path *root=&sched_task_paths(mount_task)->root;
        struct vfs_namespace *ns=vfs_path_namespace(root);
        if(!ns)return -RELIEFOS_ENOENT;
        return vfs_read_mount_table(ns,root,view==2,offset,buffer,length,out_read);
    }
    char content[PROCFS_CONTENT_MAX];
    uint32_t len = 0;
    int ret = proc_fill_content(path, content, sizeof(content));
    if (ret < 0) return ret;
    while (content[len]) ++len;
    if (offset >= len) {
        if (out_read) *out_read = 0;
        return 0;
    }
    if (length > len - offset) length = (uint32_t)(len - offset);
    for (uint32_t i = 0; i < length; ++i) {
        ((uint8_t *)buffer)[i] = (uint8_t)content[offset + i];
    }
    if (out_read) *out_read = length;
    return 0;
}

/** @brief Read a proc link with truncation and no terminating NUL.
 * @param path Absolute kernel pathname whose final component is not followed.
 * @param buffer Kernel destination.
 * @param capacity Nonzero destination capacity.
 * @return Copied byte count, EINVAL for non-links, ENOENT, or EACCES on cross-task denial.
 */
int proc_readlink(const char *path, char *buffer, uint32_t capacity)
{
    const char *file = NULL;
    uint32_t pid = 0;
    struct task *caller = sched_current_task();
    struct task *task;
    uint32_t length = 0;
    if (!path || !buffer || !capacity) return -RELIEFOS_EINVAL;
    if (!__builtin_strncmp(path,"/sys",4) && (!path[4] || path[4]=='/')) return sysfs_readlink(path,buffer,capacity);
    if (proc_text_eq(path, "/proc/mounts")) {
        const char *target = "self/mounts";
        length = capacity < 11 ? capacity : 11;
        for (uint32_t i = 0; i < length; ++i) buffer[i] = target[i];
        return (int)length;
    }
    if (proc_text_eq(path, "/proc/self") || proc_text_eq(path, "/proc/thread-self")) {
        if (!caller)
            return -RELIEFOS_ENOENT;
        struct text_stream stream = {.buffer = buffer, .capacity = capacity};
        text_unsigned(&stream, sched_task_tgid(caller));
        if (proc_text_eq(path, "/proc/thread-self")) {
            text_string(&stream, "/task/");
            text_unsigned(&stream, caller->pid);
        }
        return stream.written;
    }
    int kind = proc_path_kind(path, &file, &pid);
    if (kind != 2 && kind != 3) return -RELIEFOS_ENOENT;
    if (kind == 3 && !file) {
        char target[16] = {0};
        if (!caller) return -RELIEFOS_ENOENT;
        proc_append_u64(target, &length, sizeof(target), sched_task_tgid(caller));
        if (length > capacity) length = capacity;
        for (uint32_t i = 0; i < length; ++i) buffer[i] = target[i];
        return (int)length;
    }
    task = kind == 3 ? caller : sched_find(pid);
    if (!task) return -RELIEFOS_ENOENT;
    if (!file || (!proc_text_eq(file, "exe") && !proc_text_eq(file, "cwd") && !proc_text_eq(file, "root") && proc_fd_number(file) < 0))
        return proc_lookup(path, NULL) == 0 ? -RELIEFOS_EINVAL : -RELIEFOS_ENOENT;
    if (!proc_stat_mm_visible(caller,task)) return -RELIEFOS_EACCES;
    uint32_t text_capacity=VFS_PATH_MAX+sizeof(" (deleted)");
    char *text=kernel_malloc(text_capacity);if(!text)return -RELIEFOS_ENOMEM;
    int fd=proc_fd_number(file),rc;
    if(fd>=0)rc=proc_fd_target(task,fd,text,text_capacity);
    else {
        struct vfs_path object={0};rc=proc_mount_magic_target(path,&object);
        if(!rc)rc=proc_object_target(&object,text,text_capacity);
        vfs_path_put(&object);
    }
    if(!rc) {
        size_t size=__builtin_strlen(text);if(size>capacity)size=capacity;
        __builtin_memcpy(buffer,text,size);rc=(int)size;
    }
    kernel_free(text);return rc;
}

/** @brief Enumerate generated proc entries using stable numeric process cookies.
 * @param path Borrowed intrinsic proc directory name.
 * @param offset Required in/out cookie, zero to rewind; process cookies outlive slot compaction.
 * @param entry Required writable record, assigned on success.
 * @return One entry, zero EOF, or negative errno. Caller serializes execution
 * and retains the provider; no scheduler lock spans a generated-file callback.
 */
int proc_readdir(const char *path, uint64_t *offset, struct reliefos_dir_entry *entry)
{
    uint32_t index;
    static const char *files[] = {"uptime",
                                  "meminfo",
                                  "version",
                                  "filesystems",
                                  "stat",
                                  "mounts",
                                  "self",
                                  "sys",
                                  "cpuinfo",
                                  "leonos-drivers",
                                  "cmdline",
                                  "asound",
                                  "loadavg",
                                  "net",
                                  "partitions",
                                  "diskstats",
                                  "interrupts",
                                  "vmstat",
                                  "swaps",
                                  "thread-self"
#ifdef CONFIG_MODULES
                                  ,
                                  "modules"
#endif
#ifdef CONFIG_LOCK_STAT
                                  ,
                                  "reliefnt"
#endif
    };
    if (!path || !offset || !entry) return -22;
    int extra = proc_extra_readdir(path, offset, entry);
    if (extra != -20)
        return extra;
    if (!__builtin_strncmp(path,"/sys",4) && (!path[4] || path[4]=='/')) return sysfs_readdir(path,offset,entry);
#ifdef CONFIG_LOCK_STAT
    if (proc_text_eq(path, "/proc/reliefnt")) {
        if (*offset) return 0;
        entry->type = RELIEFOS_FS_TYPE_FILE;
        proc_copy(entry->name, sizeof(entry->name), "lock_stats");
        *offset = 1;
        return 1;
    }
#endif
    if (proc_text_eq(path, "/proc/sys") || proc_text_eq(path, "/proc/sys/kernel")) {
        static const char *values[] = {"hostname", "domainname",  "ostype",      "osrelease",
                                       "version",  "overflowuid", "overflowgid", "pid_max"};
        int parent = proc_text_eq(path, "/proc/sys");
        if (*offset >= (parent ? 2 : sizeof(values) / sizeof(values[0])))
            return 0;
        entry->type = parent ? RELIEFOS_FS_TYPE_DIR : RELIEFOS_FS_TYPE_FILE;
        proc_copy(entry->name, sizeof(entry->name),
                  parent ? (*offset ? "fs" : "kernel") : values[*offset]);
        ++*offset;
        return 1;
    }
    if (proc_text_eq(path, "/proc/asound")) {
        if (*offset == 0) {
            *offset = 1;
            entry->type = RELIEFOS_FS_TYPE_FILE;
            proc_copy(entry->name, sizeof(entry->name), "cards");
            return 1;
        }
        uint32_t ordinal = (uint32_t)(*offset - 1u);
        if (!proc_asound_card_exists(ordinal)) return 0;
        ++*offset;
        entry->type = RELIEFOS_FS_TYPE_DIR;
        proc_copy(entry->name, sizeof(entry->name), "card");
        uint32_t pos = 4;
        proc_append_u64(entry->name, &pos, sizeof(entry->name), ordinal);
        return 1;
    }
    {
        uint32_t ordinal;
        const char *leaf;
        if (proc_asound_card_path(path, &ordinal, &leaf) && *leaf == 0 &&
            proc_asound_card_exists(ordinal)) return 0;
    }
    if (!proc_text_eq(path, "/proc")) {
        const char *file = NULL;
        uint32_t pid;
        int kind = proc_path_kind(path, &file, &pid);
        if (kind == 3) pid = sched_current_pid();
        if ((kind == 2 || kind == 3) && file && proc_text_eq(file, "fd")) {
            struct task *task = sched_find(pid);
            struct task *caller = sched_current_task();
            if (!task) return -RELIEFOS_ENOENT;
            if (!proc_stat_mm_visible(caller,task)) return -RELIEFOS_EACCES;
            uint64_t limit = (uint64_t)sched_task_file_capacity(task) + 3u;
            for (uint32_t i = 0; i < sched_task_pty_capacity(task); ++i) {
                const struct task_pty_fd *pty = sched_task_pty_at(task, i);
                if (pty && pty->used && pty->fd >= 0 && (uint64_t)pty->fd + 1u > limit)
                    limit = (uint64_t)pty->fd + 1u;
            }
            while (*offset < limit) {
                uint32_t fd = (uint32_t)(*offset)++;
                const struct task_pty_fd *pty=task_pty_fd_for_fd(task,(int)fd);
                if(!task_file_for_fd(task,(int)fd) && !(pty && pty->pty_id && pty->endpoint))continue;
                *entry = (struct reliefos_dir_entry){.type = RELIEFOS_FS_TYPE_SYMLINK};
                uint32_t pos = 0;
                proc_append_u64(entry->name, &pos, sizeof(entry->name), fd);
                return 1;
            }
            return 0;
        }
        if ((kind != 2 && kind != 3) || file) return -20;
        if (!sched_find(pid)) return -2;
        static const char *task_files[] = {"stat", "cmdline", "status",       "mounts", "mountinfo",
                                           "exe",  "cwd",     "root",         "comm",   "environ",
                                           "fd",   "task",    "fdinfo",       "limits", "statm",
                                           "maps", "smaps",   "smaps_rollup", "auxv",   "io"};
        if (*offset == 11 && (proc_thread_path(path) || sched_task_tgid(sched_find(pid)) != pid ||
                              !__builtin_strncmp(path, "/proc/thread-self", 17)))
            ++*offset;
        if (*offset >= sizeof(task_files) / sizeof(task_files[0])) return 0;
        *entry = (struct reliefos_dir_entry){
            .type = (*offset == 10 || *offset == 11 || *offset == 12) ? RELIEFOS_FS_TYPE_DIR
                    : *offset >= 5 && *offset <= 7                    ? RELIEFOS_FS_TYPE_SYMLINK
                                                                      : RELIEFOS_FS_TYPE_FILE};
        proc_copy(entry->name, sizeof(entry->name), task_files[(*offset)++]);
        return 1;
    }
    if (*offset > UINT32_MAX) return 0;
    const uint64_t fixed_count=sizeof(files)/sizeof(files[0]);
    if (*offset < fixed_count) {
        index=(uint32_t)*offset;
        entry->type = proc_text_eq(files[index], "self") || proc_text_eq(files[index], "mounts") ||
                              proc_text_eq(files[index], "thread-self")
                          ? RELIEFOS_FS_TYPE_SYMLINK
                      : proc_text_eq(files[index], "sys") || proc_text_eq(files[index], "asound") ||
                              proc_text_eq(files[index], "net") ||
                              proc_text_eq(files[index], "reliefnt")
                          ? RELIEFOS_FS_TYPE_DIR
                          : RELIEFOS_FS_TYPE_FILE;
        proc_copy(entry->name, sizeof(entry->name), files[index]);
        *offset = index + 1u;
        return 1;
    }
    {
        uint64_t minimum=*offset-fixed_count;
        if(minimum>UINT32_MAX)return 0;
        struct task *task=NULL,*candidate;
        for(uint32_t slot=0;(candidate=sched_task_at(slot));++slot)
            if (candidate->pid && sched_task_tgid(candidate) == candidate->pid &&
                candidate->pid >= minimum && (!task || candidate->pid < task->pid))
                task = candidate;
        if(task) {
            *offset=fixed_count+(uint64_t)task->pid+1u;
            char name[16];
            uint32_t pos = 0;
            proc_copy(name, sizeof(name), "");
            proc_append_u64(name, &pos, sizeof(name), task->pid);
            entry->type = RELIEFOS_FS_TYPE_DIR;
            proc_copy(entry->name, sizeof(entry->name), name);
            return 1;
        }
    }
    return 0;
}

#ifdef CONFIG_LOCK_STAT
/** @brief Expose write mode only on the diagnostic control inode.
 * @param name Required borrowed intrinsic proc key.
 * @param node Required borrowed retained node.
 * @param value Required writable metadata or proposed value.
 * @param write True for a metadata mutation request.
 * @return Zero or negative errno; generated metadata cannot be changed.
 * Execution-owned context; no allocation or ownership transfer. */
int proc_lock_stats_permissions(const char *name, const struct storage_node *node,
                                struct reliefos_permissions *value, bool write)
{
    int rc = fs_virtual_permissions(name, node, value, write);
    if (!rc && !write && proc_text_eq(name, "/proc/reliefnt/lock_stats")) value->mode = 0644;
    return rc;
}

/** @brief Accept shell redirection's zero-size truncate without resetting data.
 * @param name Required borrowed intrinsic proc key.
 * @param length Requested file size, only zero is accepted.
 * @return Zero or negative errno; VFS enforces the live read-only mount.
 * Execution-owned task context; current CAP_SYS_ADMIN is required. */
int proc_lock_stats_truncate(const char *name, uint64_t length)
{
    if (!proc_text_eq(name, "/proc/reliefnt/lock_stats")) return -EROFS;
    struct task *task = sched_current_task();
    if (!task || !(task->cap_effective & (1ULL << CAP_SYS_ADMIN))) return -EPERM;
    return length ? -EINVAL : 0;
}

/** @brief Accept one privileged whole-value diagnostic command at offset zero.
 * @param name Required borrowed intrinsic proc key.
 * @param offset File position, which must be zero.
 * @param buffer Required validated readable command bytes.
 * @param length Exact byte length including optional newline, at most six.
 * @param written Required writable byte count, zero on failure.
 * @return Zero or negative errno. Execution-owned task context; VFS checks
 * the live read-only mount and this callback checks current CAP_SYS_ADMIN. */
int proc_lock_stats_write(const char *name, uint64_t offset, const void *buffer,
                          uint32_t length, uint32_t *written)
{
    if (!written) return -EINVAL;
    *written = 0;
    if (!proc_text_eq(name, "/proc/reliefnt/lock_stats")) return -EROFS;
    struct task *task = sched_current_task();
    if (!task || !(task->cap_effective & (1ULL << CAP_SYS_ADMIN))) return -EPERM;
    if (offset || !buffer || !length || length > 6) return -EINVAL;
    int rc = lock_profile_control(buffer, length);
    if (!rc) *written = length;
    return rc;
}
#endif
