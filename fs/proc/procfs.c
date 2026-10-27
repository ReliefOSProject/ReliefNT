#include <reliefnt/sound_service.h>
/* Read-only procfs implementation for the Unix migration. */
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
#include <reliefnt/pty.h>
#include <reliefos/fs_abi.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include "internal.h"

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

static int proc_path_kind(const char *path, const char **file_name,
                          uint32_t *pid)
{
    const char *prefix = "/proc/";
    if (!path) return 0;
    for (uint32_t i = 0; prefix[i]; ++i)
        if (path[i] != prefix[i]) return 0;
    const char *p = path + 6;
    const char *slash = p;
    uint32_t value = 0;
    if (pid) *pid = 0;
    if (file_name) *file_name = 0;
    while (*slash && *slash != '/') ++slash;
    if (file_name && *slash) *file_name = slash + 1;
    while (p < slash && *p >= '0' && *p <= '9') {
        uint32_t digit = (uint32_t)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10u) return 0;
        value = value * 10u + digit;
        ++p;
    }
    if (p == slash && value && path[6] != '0') {
        if (pid) *pid = value;
        return 2;
    }
    if (p == path + 6 && p < slash && (uint32_t)(slash - p) == 4u &&
        p[0] == 's' && p[1] == 'e' && p[2] == 'l' && p[3] == 'f') {
        return 3;
    }
    return 0;
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
    if (proc_text_eq(path, "/proc/meminfo")) {
        proc_append_text(buffer, &pos, capacity, "MemTotal: ");
        proc_append_u64(buffer, &pos, capacity, mm_total_memory_kib());
        proc_append_text(buffer, &pos, capacity, " kB\nMemFree: ");
        proc_append_u64(buffer, &pos, capacity, mm_free_memory_kib());
        /* There is no swap or reclaimable page-cache pool yet. All memory
         * currently available for new allocations is in the free-page pool. */
        proc_append_text(buffer, &pos, capacity, " kB\nMemAvailable: ");
        proc_append_u64(buffer, &pos, capacity, mm_free_memory_kib());
        proc_append_text(buffer, &pos, capacity, " kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n");
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
    if (proc_text_eq(path, "/proc/stat")) {
        uint64_t busy = 0;
        uint64_t idle = 0;
        uint64_t per_cpu_busy[SMP_MAX_CPUS] = {0};
        uint64_t per_cpu_idle[SMP_MAX_CPUS] = {0};
        uint32_t cpu_count = smp_cpu_count();
        if (cpu_count > SMP_MAX_CPUS) cpu_count = SMP_MAX_CPUS;
        sched_cpu_runtime_snapshot(per_cpu_busy, per_cpu_idle, NULL, NULL, cpu_count);
        for (uint32_t cpu = 0; cpu < cpu_count; ++cpu) {
            busy += per_cpu_busy[cpu];
            idle += per_cpu_idle[cpu];
        }
        proc_append_text(buffer, &pos, capacity, "cpu ");
        proc_append_u64(buffer, &pos, capacity, busy);
        proc_append_text(buffer, &pos, capacity, " 0 0 ");
        proc_append_u64(buffer, &pos, capacity, idle);
        proc_append_text(buffer, &pos, capacity, " 0 0 0 0 0 0\n");
        for (uint32_t cpu = 0; cpu < cpu_count; ++cpu) {
            proc_append_text(buffer, &pos, capacity, "cpu");
            proc_append_u64(buffer, &pos, capacity, cpu);
            proc_append_text(buffer, &pos, capacity, " ");
            proc_append_u64(buffer, &pos, capacity, per_cpu_busy[cpu]);
            proc_append_text(buffer, &pos, capacity, " 0 0 ");
            proc_append_u64(buffer, &pos, capacity, per_cpu_idle[cpu]);
            proc_append_text(buffer, &pos, capacity, " 0 0 0 0 0 0\n");
        }
        return 0;
    }
    {
        const char *file = 0;
        uint32_t pid = 0;
        int kind = proc_path_kind(path, &file, &pid);
        if (kind == 3) {
            pid = sched_current_pid();
            kind = 2;
        }
        if (kind == 2 && file && pid) {
            struct task *task = sched_find(pid);
            if (proc_text_eq(file, "status")) {
                if (!task) return -2;
                proc_append_text(buffer, &pos, capacity, "Name:\t");
                const char *name = task->name ? task->name : "?";
                for (uint32_t i = 0; name[i] && i < 15; ++i) {
                    char letter[2] = {name[i], 0};
                    if (name[i] == '\n') proc_append_text(buffer, &pos, capacity, "\\n");
                    else if (name[i] == '\\') proc_append_text(buffer, &pos, capacity, "\\\\");
                    else proc_append_text(buffer, &pos, capacity, letter);
                }
                const char *state = task->state == TASK_EXITED ? "Z (zombie)" :
                    task->state == TASK_STOPPED ? "T (stopped)" :
                    task->state == TASK_BLOCKED ? "S (sleeping)" : "R (running)";
                proc_append_text(buffer, &pos, capacity, "\nState:\t");
                proc_append_text(buffer, &pos, capacity, state);
                proc_append_text(buffer, &pos, capacity, "\nTgid:\t");
                proc_append_u64(buffer, &pos, capacity, sched_task_tgid(task));
                proc_append_text(buffer, &pos, capacity, "\nPid:\t");
                proc_append_u64(buffer, &pos, capacity, task->pid);
                proc_append_text(buffer, &pos, capacity, "\nPPid:\t");
                proc_append_u64(buffer, &pos, capacity, task->parent_pid);
                const uint32_t ids[] = {task->uid, task->euid, task->suid, task->fsuid,
                                        task->gid, task->egid, task->sgid, task->fsgid};
                for (uint32_t i = 0; i < 8; ++i) {
                    proc_append_text(buffer, &pos, capacity, i == 0 ? "\nUid:\t" : i == 4 ? "\nGid:\t" : "\t");
                    proc_append_u64(buffer, &pos, capacity, ids[i]);
                }
                if (sched_task_as(task)->cr3) {
                    proc_append_text(buffer, &pos, capacity, "\nVmRSS:\t");
                    proc_append_u64(buffer, &pos, capacity, address_space_user_resident_kib(sched_task_as(task)));
                    proc_append_text(buffer, &pos, capacity, " kB");
                }
                proc_append_text(buffer, &pos, capacity, "\nLeonOSRole:\t");
                proc_append_u64(buffer, &pos, capacity, task->role);
                proc_append_text(buffer, &pos, capacity, "\nLeonOSFlags:\t");
                proc_append_u64(buffer, &pos, capacity, task->flags);
                proc_append_text(buffer, &pos, capacity, "\n");
                return 0;
            }

        }
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
static int proc_object_target(const struct vfs_path *object,char *target,uint32_t capacity)
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

struct proc_open_view {struct vfs_namespace *ns;struct vfs_path root;bool mountinfo;};

/** @brief Capture the selected task's mount namespace and root at open time.
 * @param name Intrinsic authorized proc inode key, independent of attachment.
 * @param flags Sanitized status flags, borrowed and unchanged.
 * @param out Required owned output; ordinary generated files return NULL.
 * @return Zero or negative errno with empty output on failure. Captures a root
 * path rather than generated text, so later mount changes remain visible.
 * Namespace ownership outlives its retained paths; final release drops this
 * path even after task exit. Nonblocking, execution-owned, without graph locks.
 */
int proc_file_open(const char *name,uint32_t flags,void **out)
{
    (void)flags;if(!out)return -RELIEFOS_EINVAL;*out=NULL;
    struct task *task=NULL;int view=proc_mount_view(name,&task);if(!view)return 0;
    const struct vfs_path *root=&sched_task_paths(task)->root;
    struct vfs_namespace *ns=vfs_path_namespace(root);if(!ns)return -RELIEFOS_ENOENT;
    struct proc_open_view *state=kernel_malloc(sizeof(*state));if(!state)return -RELIEFOS_ENOMEM;
    *state=(struct proc_open_view){.ns=ns,.mountinfo=view==2};vfs_path_get(root,&state->root);*out=state;return 0;
}

/** @brief Read a mount view using its open-time namespace/root references.
 * @param value Borrowed captured state, independent of target PID liveness.
 * @param offset Byte position in the currently rendered mount graph.
 * @param buffer Kernel-accessible destination, nullable at zero capacity.
 * @param capacity Available output bytes.
 * @param read Required output count, cleared before validation.
 * @return Renderer status or negative EINVAL. Does not look up a PID, change
 * captured root, or transfer ownership. Execution-owned, no graph locks
 * across renderer allocation or filesystem policy callbacks.
 */
int proc_file_read(void *value,uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read)
{
    if(!read)return -RELIEFOS_EINVAL;*read=0;
    struct proc_open_view *state=value;if(!state)return -RELIEFOS_EINVAL;
    return vfs_read_mount_table(state->ns,&state->root,state->mountinfo,offset,buffer,capacity,read);
}

/** @brief Release the open-time root after the final shared OFD reference.
 * @param value Owned optional captured state; NULL is ignored.
 * @return None. Root/provider release can perform I/O; caller holds execution
 * ownership without a graph, scheduler or module-table spinlock.
 */
void proc_file_close(void *value)
{
    struct proc_open_view *state=value;if(!state)return;
    vfs_path_put(&state->root);kernel_free(state);
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
static bool proc_stat_mm_visible(struct task *caller, struct task *target)
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
 */
static void proc_task_stat(struct task *task, struct text_stream *s)
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
    values[14] = sched_task_cpu_ticks(task, true);
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
 * @param path Absolute kernel pathname.
 * @param out Optional returned node.
 * @return Zero or negative ENOENT.
 */
int proc_lookup(const char *path, struct storage_node *out)
{
    char content[PROCFS_CONTENT_MAX];
    if (path && !__builtin_strncmp(path,"/sys",4) && (!path[4] || path[4]=='/')) return sysfs_lookup(path,out);
    if (!path || (path[0] != '/' || path[1] != 'p' || path[2] != 'r' ||
                  path[3] != 'o' || path[4] != 'c' || (path[5] && path[5] != '/'))) return -2;
    if (proc_text_eq(path, "/proc/mounts")) {
        if (out) *out = (struct storage_node){.type = RELIEFOS_FS_TYPE_SYMLINK,
            .flags = STORAGE_NODE_FLAG_PROC, .size = 11};
        return 0;
    }
    if (proc_text_eq(path, "/proc") || proc_text_eq(path, "/proc/sys") ||
        proc_text_eq(path, "/proc/sys/kernel") ||
        proc_text_eq(path, "/proc/asound")) {
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
    if (proc_text_eq(path, "/proc/cmdline") ||
        proc_text_eq(path, "/proc/uptime") ||
        proc_text_eq(path, "/proc/meminfo") ||
        proc_text_eq(path, "/proc/version") ||
        proc_text_eq(path, "/proc/filesystems") ||
        proc_text_eq(path, "/proc/asound/cards") ||
        proc_text_eq(path, "/proc/sys/kernel/hostname") ||
        proc_text_eq(path, "/proc/sys/kernel/domainname") ||
        proc_text_eq(path, "/proc/sys/kernel/ostype") ||
        proc_text_eq(path, "/proc/sys/kernel/osrelease") ||
        proc_text_eq(path, "/proc/sys/kernel/version") ||
        proc_text_eq(path, "/proc/sys/kernel/overflowuid") ||
        proc_text_eq(path, "/proc/sys/kernel/overflowgid") ||
        proc_text_eq(path, "/proc/stat")) {
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
 * @param path Resolved absolute kernel pathname.
 * @param offset Byte offset, including positions past EOF.
 * @param buffer Kernel destination, nullable for zero length.
 * @param length Maximum bytes to copy.
 * @param out_read Optional actual byte count.
 * @return Zero or negative errno, preserving mount backend failures.
 */
int proc_read(const char *path, uint64_t offset, void *buffer, uint32_t length,
              uint32_t *out_read)
{
    if (path && !__builtin_strncmp(path,"/sys",4) && (!path[4] || path[4]=='/')) return sysfs_read(path,offset,buffer,length,out_read);
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
        else if(proc_text_eq(file,"stat")) proc_task_stat(task,&stream);
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
    static const char *files[] = {"uptime", "meminfo", "version", "filesystems", "stat", "mounts", "self", "sys", "cpuinfo", "leonos-drivers", "cmdline", "asound"
#ifdef CONFIG_MODULES
        , "modules"
#endif
    };
    if (!path || !offset || !entry) return -22;
    if (!__builtin_strncmp(path,"/sys",4) && (!path[4] || path[4]=='/')) return sysfs_readdir(path,offset,entry);
    if (proc_text_eq(path, "/proc/sys") || proc_text_eq(path, "/proc/sys/kernel")) {
        static const char *values[] = {"hostname", "domainname", "ostype", "osrelease", "version"};
        int parent = proc_text_eq(path, "/proc/sys");
        if (*offset >= (parent ? 1 : sizeof(values) / sizeof(values[0]))) return 0;
        entry->type = parent ? RELIEFOS_FS_TYPE_DIR : RELIEFOS_FS_TYPE_FILE;
        proc_copy(entry->name, sizeof(entry->name), parent ? "kernel" : values[*offset]);
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
        static const char *task_files[] = {"stat", "cmdline", "status", "mounts", "mountinfo", "exe", "cwd", "root", "comm", "environ", "fd"};
        if (*offset >= sizeof(task_files) / sizeof(task_files[0])) return 0;
        *entry = (struct reliefos_dir_entry){.type = *offset == 10 ? RELIEFOS_FS_TYPE_DIR : *offset >= 5 && *offset <= 7 ? RELIEFOS_FS_TYPE_SYMLINK : RELIEFOS_FS_TYPE_FILE};
        proc_copy(entry->name, sizeof(entry->name), task_files[(*offset)++]);
        return 1;
    }
    if (*offset > UINT32_MAX) return 0;
    const uint64_t fixed_count=sizeof(files)/sizeof(files[0]);
    if (*offset < fixed_count) {
        index=(uint32_t)*offset;
        entry->type = index == 5 || index == 6 ? RELIEFOS_FS_TYPE_SYMLINK :
            (index == 7 || index == 11) ? RELIEFOS_FS_TYPE_DIR : RELIEFOS_FS_TYPE_FILE;
        proc_copy(entry->name, sizeof(entry->name), files[index]);
        *offset = index + 1u;
        return 1;
    }
    {
        uint64_t minimum=*offset-fixed_count;
        if(minimum>UINT32_MAX)return 0;
        struct task *task=NULL,*candidate;
        for(uint32_t slot=0;(candidate=sched_task_at(slot));++slot)
            if(candidate->pid && candidate->pid>=minimum &&
               (!task || candidate->pid<task->pid))task=candidate;
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
