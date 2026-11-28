/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/fsnotify.h>
#include <reliefnt/heap.h>
#include <reliefnt/mount.h>
#include <reliefnt/sched.h>
#include <reliefnt/text_stream.h>
#include <linux/errno.h>
#include <linux/statfs.h>

#define CGROUP_FILE_SHIFT 4u
#define CGROUP_COMMAND_MAX 4095u
_Static_assert(CG_FILE_COUNT <= (1u << CGROUP_FILE_SHIFT), "cgroup inode file bits");

static const struct vfs_backend_ops cgroup_ops;
static const struct cgroup_control {
    const char *name;
    bool writable;
} controls[CG_FILE_COUNT] = {
    [CG_PROCS] = {"cgroup.procs", true}, [CG_THREADS] = {"cgroup.threads", false},
    [CG_CONTROLLERS] = {"cgroup.controllers", false}, [CG_SUBTREE] = {"cgroup.subtree_control", true},
    [CG_TYPE] = {"cgroup.type", false}, [CG_EVENTS] = {"cgroup.events", false},
    [CG_STAT] = {"cgroup.stat", false}, [CG_PIDS_MAX] = {"pids.max", true},
    [CG_PIDS_CURRENT] = {"pids.current", false}, [CG_PIDS_PEAK] = {"pids.peak", false},
    [CG_PIDS_EVENTS] = {"pids.events", false}, [CG_PIDS_LOCAL] = {"pids.events.local", false},
};

/** @brief Encode group, file and controller incarnation into a stable inode key. */
struct storage_node cgroup_node(const struct cgroup *g, enum cgroup_file file, uint32_t epoch)
{
    return (struct storage_node){.type = file == CG_DIR ? RELIEFOS_FS_TYPE_DIR : RELIEFOS_FS_TYPE_FILE,
        .flags = STORAGE_NODE_FLAG_CGROUP, .first_cluster = g->id,
        .entry_id = ((uint64_t)g->id << 32) | (file >= CG_PIDS_MAX ? epoch << CGROUP_FILE_SHIFT : 0) | file};
}

/** @brief Publish an event on the shared hierarchy, or retire an unlinked inode's watch. */
void cgroup_notify(const struct storage_node *node, bool removed)
{
    struct fsnotify_key key = {.ops = &cgroup_ops, .context = &cgroup_root, .node = *node};
    if (removed) fsnotify_delete(&key);
    else fsnotify_emit(&key, IN_MODIFY, 0, NULL, false);
}

/** @brief Map a canonical inode number to its file selector. */
static enum cgroup_file node_file(const struct storage_node *node)
{ return node->entry_id & ((1u << CGROUP_FILE_SHIFT) - 1u); }

/** @brief Test visibility under execution ownership (remote exits do not change it). */
static bool file_visible(const struct cgroup *g, enum cgroup_file file)
{
    if (!g->linked || file >= CG_FILE_COUNT) return false;
    if (file >= CG_PIDS_MAX) return cgroup_pids_enabled(g);
    return g->parent || (file != CG_TYPE && file != CG_EVENTS);
}

/** @brief Match a visible inode, never a recreated directory or controller generation. */
static bool node_live(const struct cgroup *g, const struct storage_node *node)
{
    enum cgroup_file file = node_file(node);
    return g && file_visible(g, file) &&
        node->entry_id == cgroup_node(g, file, g->parent ? g->parent->pids_epoch : 0).entry_id;
}

/** @brief Look up one live directory or control file with no text-path replay. */
static int cgroup_lookup(void *context, const struct storage_node *parent, const char *name,
                         struct storage_node *out)
{
    (void)context;
    struct cgroup *g = cgroup_find(parent->first_cluster);
    if (!node_live(g, parent)) return -ENOENT;
    if (node_file(parent) != CG_DIR) return -ENOTDIR;
    for (enum cgroup_file file = CG_PROCS; file < CG_FILE_COUNT; ++file) {
        if (!file_visible(g, file) || __builtin_strcmp(controls[file].name, name)) continue;
        *out = cgroup_node(g, file, g->parent ? g->parent->pids_epoch : 0);
        return 0;
    }
    for (struct cgroup *child = g->children; child; child = child->sibling) {
        if (__builtin_strcmp(child->name, name)) continue;
        *out = cgroup_node(child, CG_DIR, 0);
        return 0;
    }
    return -ENOENT;
}

/** @brief Pin the actual group for one dentry or control file. */
static int cgroup_retain(void *context, const struct storage_node *node, void **pin)
{
    (void)context;
    *pin = NULL;
    struct cgroup *g = cgroup_find(node->first_cluster);
    if (!node_live(g, node)) return -ENOENT;
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    cgroup_get_locked(g);
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    *pin = g;
    return 0;
}

/** @brief Release the inode pin independently of its current name and visibility. */
static void cgroup_release(void *context, const struct storage_node *node, void *pin)
{
    (void)context; (void)node;
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    cgroup_put_locked(pin);
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    cgroup2_kick();
}

/** @brief Compare immutable keys; watches also use this after namespace removal. */
static bool cgroup_same(void *context, const struct storage_node *a, const struct storage_node *b)
{ (void)context; return a->entry_id == b->entry_id; }

/** @brief Restrict hierarchy changes to a system administrator (no delegation). */
static bool cgroup_admin(void)
{
    const struct task *task = sched_current_task();
    return !task || task->kind == TASK_KIND_KERNEL || (task->cap_effective & (1ULL << CAP_SYS_ADMIN));
}

/** @brief Return directory or control metadata for a retained inode key. */
static struct reliefos_permissions cgroup_mode(const struct cgroup *g, enum cgroup_file file)
{
    if (file == CG_DIR) return g->mode;
    return (struct reliefos_permissions){controls[file].writable ? 0644u : 0444u, g->mode.uid, g->mode.gid};
}

/** @brief Expose DAC metadata and allow only privileged directory attribute changes. */
static int cgroup_permissions(void *context, const struct vfs_path *path,
                              struct reliefos_permissions *value, bool write)
{
    (void)context;
    const struct storage_node *node = vfs_path_node(path);
    struct cgroup *g = cgroup_find(node->first_cluster);
    if (!node_live(g, node)) return -ENODEV;
    enum cgroup_file file = node_file(node);
    if (!write) { *value = cgroup_mode(g, file); return 0; }
    if (vfs_path_mount_flags(path) & MS_RDONLY) return -EROFS;
    if (file != CG_DIR || !cgroup_admin()) return -EPERM;
    g->mode = (struct reliefos_permissions){value->mode & 0777u, value->uid, value->gid};
    return 0;
}

/** @brief Report shared device identity and zero links for retired inode incarnations. */
static int cgroup_stat(void *context, const struct vfs_path *path, struct linux_stat_abi *out)
{
    (void)context;
    const struct storage_node *node = vfs_path_node(path);
    const struct cgroup *g = cgroup_find(node->first_cluster);
    enum cgroup_file file = node_file(node);
    if (!g || file >= CG_FILE_COUNT) return -ENOENT;
    struct reliefos_permissions mode = cgroup_mode(g, file);
    uint64_t links = 0;
    if (node_live(g, node)) {
        links = file == CG_DIR ? 2 : 1;
        if (file == CG_DIR) for (const struct cgroup *child = g->children; child; child = child->sibling) ++links;
    }
    *out = (struct linux_stat_abi){.st_dev = CGROUP_DEVICE, .st_ino = node->entry_id, .st_nlink = links,
        .st_mode = (file == CG_DIR ? LINUX_S_IFDIR : LINUX_S_IFREG) | mode.mode,
        .st_uid = mode.uid, .st_gid = mode.gid, .st_blksize = 4096};
    return 0;
}

/** @brief Return Linux cgroup2 magic and the shared device's filesystem ID. */
static int cgroup_statfs(void *context, const struct storage_node *node, struct linux_statfs_abi *out)
{
    (void)context; (void)node;
    *out = (struct linux_statfs_abi){.f_type = CGROUP_MAGIC, .f_bsize = 4096, .f_frsize = 4096,
        .f_namelen = VFS_NAME_MAX, .f_fsid = {CGROUP_DEVICE, 0}};
    return 0;
}

/** @brief Publish only directories, reserving the control names even when disabled. */
static int cgroup_create(void *context, const struct storage_node *parent, const char *name, uint32_t type,
                         const struct reliefos_permissions *mode, const char *target,
                         struct storage_node *out, void **pin)
{
    (void)context; (void)target;
    if (type != LINUX_S_IFDIR || !cgroup_admin()) return -EPERM;
    for (enum cgroup_file file = CG_PROCS; file < CG_FILE_COUNT; ++file)
        if (!__builtin_strcmp(controls[file].name, name)) return -EEXIST;
    struct cgroup *g = NULL;
    int rc = cgroup_mkdir(cgroup_find(parent->first_cluster), name, mode, &g);
    if (rc) return rc;
    *out = cgroup_node(g, CG_DIR, 0);
    return pin ? cgroup_retain(context, out, pin) : 0;
}

/** @brief Retire the weak watches of every file in a removed controller incarnation. */
static void cgroup_remove_files(const struct cgroup *g, enum cgroup_file first, uint32_t epoch)
{
    if (!fsnotify_has_watches()) return;
    for (enum cgroup_file file = first; file < CG_FILE_COUNT; ++file) {
        struct storage_node key = cgroup_node(g, file, epoch);
        cgroup_notify(&key, true);
    }
}

/** @brief Remove an empty group and retire its control and directory watch keys. */
static int cgroup_remove(void *context, const struct vfs_path *path, bool directory)
{
    (void)context;
    if (!directory || !cgroup_admin()) return -EPERM;
    struct cgroup *g = cgroup_find(vfs_path_node(path)->first_cluster);
    if (!g) return -ENOENT;
    int rc = cgroup_rmdir(g);
    if (!rc) cgroup_remove_files(g, CG_DIR, g->parent->pids_epoch);
    return rc;
}

/** @brief Enumerate dot entries, visible files and subgroups with monotonic directory cookies. */
static int cgroup_dir_next(const struct storage_node *node, uint64_t *cursor,
                           char *name, size_t capacity, uint32_t *type)
{
    struct cgroup *g = cgroup_find(node->first_cluster);
    if (!node_live(g, node)) return -ENOENT;
    uint64_t next = *cursor;
    const char *text = NULL;
    *type = RELIEFOS_FS_TYPE_DIR;
    if (next < 2) text = next++ ? ".." : ".";
    else {
        while (next < CG_FILE_COUNT + 1u) {
            enum cgroup_file file = (enum cgroup_file)(next++ - 1);
            if (!file_visible(g, file)) continue;
            text = controls[file].name;
            *type = RELIEFOS_FS_TYPE_FILE;
            break;
        }
        if (!text) {
            const struct cgroup *first = NULL;
            for (const struct cgroup *child = g->children; child; child = child->sibling)
                if ((uint64_t)child->id + CG_FILE_COUNT >= next && (!first || child->id < first->id)) first = child;
            if (!first) return 0;
            text = first->name;
            next = (uint64_t)first->id + CG_FILE_COUNT + 1u;
        }
    }
    size_t length = __builtin_strlen(text) + 1u;
    if (length > capacity) return -ENAMETOOLONG;
    __builtin_memcpy(name, text, length);
    *cursor = next;
    return 1;
}

/** @brief Export the legacy directory record without truncating its identity. */
static int cgroup_readdir(void *context, const struct storage_node *node, uint64_t *cursor,
                          struct reliefos_dir_entry *entry, uint64_t flags)
{
    (void)context; (void)flags;
    return cgroup_dir_next(node, cursor, entry->name, sizeof(entry->name), &entry->type);
}

/** @brief Export a Linux getdents name up to NAME_MAX. */
static int cgroup_readdir_name(void *context, const struct storage_node *node, uint64_t *cursor,
                               char *name, size_t capacity, uint64_t flags)
{
    (void)context; (void)flags;
    uint32_t type;
    return cgroup_dir_next(node, cursor, name, capacity, &type);
}

/** @brief Partition integer IDs in place by descending bit without allocating a second list. */
static void sort_ids(uint32_t *ids, size_t count, uint32_t bit)
{
    if (count < 2 || !bit) return;
    size_t first = 0, last = count;
    while (first < last) {
        if (!(ids[first] & bit)) ++first;
        else {
            uint32_t temp = ids[first];
            ids[first] = ids[--last];
            ids[last] = temp;
        }
    }
    sort_ids(ids, first, bit >> 1);
    sort_ids(ids + first, count - first, bit >> 1);
}

/** @brief Match one live thread, treating an uncharged membership as the root. */
static bool cgroup_matches(const struct cgroup *g, const struct task *task)
{
    return cgroup_live_task(task) && (task->cgroup ? task->cgroup : &cgroup_root) == g;
}

/** @brief Snapshot matching live IDs under the task lock; format and deduplicate outside it. */
static int cgroup_read_members(const struct cgroup *g, bool threads, struct text_stream *text)
{
    if (!text->capacity) return 0;
    uint64_t flags;
    size_t capacity = 0;
    const struct task *task;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    for (uint32_t i = 0; (task = sched_task_at(i)); ++i)
        if (cgroup_matches(g, task)) ++capacity;
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    if (!capacity) return 0;
    if (capacity > SIZE_MAX / sizeof(uint32_t)) return -ENOMEM;
    uint32_t *ids = kernel_malloc(capacity * sizeof(*ids));
    if (!ids) return -ENOMEM;
    size_t count = 0;
    /* The execution writer serializes admissions and moves; a remote exit can
     * only reduce the number of live members between the count and snapshot. */
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    for (uint32_t i = 0; count < capacity; ++i) {
        const struct task *task = sched_task_at(i);
        if (!task) break;
        if (cgroup_matches(g, task))
            ids[count++] = threads ? task->pid : sched_task_tgid(task);
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    if (!threads) sort_ids(ids, count, 1u << 31);
    for (size_t i = 0; i < count && text->written < text->capacity; ++i) {
        if (threads || !i || ids[i] != ids[i-1]) {
            text_unsigned(text, ids[i]);
            text_string(text, "\n");
        }
    }
    kernel_free(ids);
    return 0;
}

/** @brief Render the stable Linux control names and a locked counter snapshot. */
static int cgroup_read(void *context, const struct storage_node *node, uint64_t offset,
                       void *buffer, uint32_t capacity, uint32_t *read, uint64_t mount_flags)
{
    (void)context; (void)mount_flags;
    *read = 0;
    struct cgroup *g = cgroup_find(node->first_cluster);
    if (!node_live(g, node)) return -ENODEV;
    struct text_stream text = {.offset = offset, .buffer = buffer, .capacity = capacity};
    enum cgroup_file file = node_file(node);
    if (file == CG_PROCS || file == CG_THREADS) {
        int rc = cgroup_read_members(g, file == CG_THREADS, &text);
        *read = text.written;
        return rc;
    }
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    switch (file) {
    case CG_TYPE: text_string(&text, "domain\n"); break;
    case CG_CONTROLLERS:
        if (!g->parent || cgroup_pids_enabled(g)) text_string(&text, "pids\n");
        break;
    case CG_SUBTREE:
        if (g->subtree_pids) text_string(&text, "pids\n");
        break;
    case CG_EVENTS: text_string(&text, g->live ? "populated 1\nfrozen 0\n" : "populated 0\nfrozen 0\n"); break;
    case CG_STAT:
        text_string(&text, "nr_descendants "); text_unsigned(&text, g->descendants);
        text_string(&text, "\nnr_dying_descendants "); text_unsigned(&text, g->dying);
        text_string(&text, "\n"); break;
    case CG_PIDS_MAX:
        if (g->limit == CGROUP_PIDS_UNLIMITED) text_string(&text, "max");
        else text_unsigned(&text, g->limit);
        text_string(&text, "\n"); break;
    case CG_PIDS_CURRENT: case CG_PIDS_PEAK: case CG_PIDS_EVENTS: case CG_PIDS_LOCAL:
        if (file >= CG_PIDS_EVENTS) text_string(&text, "max ");
        text_unsigned(&text, file == CG_PIDS_CURRENT ? g->charged : file == CG_PIDS_PEAK ? g->peak :
            file == CG_PIDS_EVENTS ? g->events : g->local_events);
        text_string(&text, "\n"); break;
    default: kernel_spin_unlock_irqrestore(&scheduler_lock, flags); return -EINVAL;
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    *read = text.written;
    return 0;
}

/** @brief Recognize ASCII whitespace in a userspace control write. */
static bool command_space(char c)
{ return c == ' ' || (c >= '\t' && c <= '\r'); }

/** @brief Parse a complete unsigned base-0 integer with a bounded target range. */
static int command_number(const char *text, size_t length, uint32_t maximum, uint32_t *out)
{
    if (length && *text == '+') { ++text; --length; }
    if (!length) return -EINVAL;
    unsigned base = 10;
    if (length > 1 && text[0] == '0') {
        if (text[1] == 'x' || text[1] == 'X') { text += 2; length -= 2; base = 16; }
        else base = 8;
    }
    if (!length) return -EINVAL;
    uint32_t value = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = text[i];
        unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
            c >= 'A' && c <= 'F' ? c - 'A' + 10 : base;
        if (digit >= base || value > (maximum - digit) / base) return -EINVAL;
        value = value * base + digit;
    }
    *out = value;
    return 0;
}

/** @brief Validate every controller token before changing any child file or limit. */
static int command_subtree(const char *text, size_t length, int *enable)
{
    *enable = -1;
    for (size_t i = 0; i < length;) {
        if (command_space(text[i])) { ++i; continue; }
        if (length - i < 5 || (text[i] != '+' && text[i] != '-') ||
            __builtin_memcmp(text+i+1, "pids", 4) ||
            (length - i > 5 && !command_space(text[i+5]))) return -EINVAL;
        *enable = text[i] == '+';
        i += 5;
    }
    return 0;
}

/** @brief Apply one fully validated cgroup control write in a scheduler transaction. */
static int cgroup_write(void *context, struct storage_node *node, uint64_t offset,
                        const void *bytes, uint32_t length, uint32_t *written)
{
    (void)context; (void)offset;
    *written = 0;
    struct cgroup *g = cgroup_find(node->first_cluster);
    if (!node_live(g, node)) return -ENODEV;
    enum cgroup_file file = node_file(node);
    if (file == CG_DIR || !controls[file].writable || !cgroup_admin()) return -EPERM;
    if (!length) return 0;
    if (length > CGROUP_COMMAND_MAX) return -E2BIG;
    const char *text = bytes;
    size_t count = length;
    while (count && command_space(*text)) { ++text; --count; }
    while (count && command_space(text[count-1])) --count;
    int enable = -1, rc = 0;
    uint32_t value = 0;
    uint64_t limit = CGROUP_PIDS_UNLIMITED;
    if (file == CG_SUBTREE) rc = command_subtree(text, count, &enable);
    else if (file == CG_PROCS) rc = command_number(text, count, INT32_MAX, &value);
    else if (count != 3 || __builtin_memcmp(text, "max", 3)) {
        rc = command_number(text, count, CGROUP_PIDS_MAX, &value);
        limit = value;
    }
    if (rc) return rc;
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    uint32_t retired_epoch = !enable && g->subtree_pids ? g->pids_epoch : 0;
    if (file == CG_SUBTREE && enable >= 0) rc = cgroup_set_subtree_locked(g, enable);
    else if (file == CG_PROCS) rc = cgroup_move_locked(g, value);
    else if (file == CG_PIDS_MAX) g->limit = limit;
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    if (rc) return rc;
    if (retired_epoch)
        for (const struct cgroup *child = g->children; child; child = child->sibling)
            cgroup_remove_files(child, CG_PIDS_MAX, retired_epoch);
    cgroup2_kick();
    *written = length;
    return 0;
}

/** @brief Accept harmless O_TRUNC on writable live controls, without changing policy. */
static int cgroup_truncate(void *context, struct storage_node *node, uint64_t length)
{
    (void)context;
    if (!node_live(cgroup_find(node->first_cluster), node)) return -ENODEV;
    if (!controls[node_file(node)].writable || !cgroup_admin()) return -EPERM;
    return length ? -EINVAL : 0;
}

/** @brief Validate attachment options without claiming the unimplemented controllers or delegation. */
static int cgroup_remount(void *context, uint64_t flags, const char *options)
{
    (void)context;
    if (flags & ~MS_RDONLY) return -EOPNOTSUPP;
    for (const char *start = options; start && *start;) {
        const char *end = start;
        while (*end && *end != ',') ++end;
        size_t n = (size_t)(end - start);
        if (n && !(n == sizeof("nsdelegate") - 1 && !__builtin_memcmp(start, "nsdelegate", n)) &&
                 !(n == sizeof("memory_recursiveprot") - 1 &&
                   !__builtin_memcmp(start, "memory_recursiveprot", n))) return -EINVAL;
        start = *end ? end + 1 : end;
    }
    return 0;
}

/** @brief Generated controls apply synchronously; no backing data require a flush. */
static int cgroup_sync(void *context, const struct storage_node *node, bool filesystem, bool data_only)
{
    (void)context; (void)filesystem; (void)data_only;
    return node_live(cgroup_find(node->first_cluster), node) ? 0 : -ENODEV;
}

static const struct vfs_backend_ops cgroup_ops = {
    .lookup = cgroup_lookup, .retain = cgroup_retain, .release = cgroup_release, .same_node = cgroup_same,
    .remount_options = cgroup_remount, .read = cgroup_read, .readdir = cgroup_readdir,
    .readdir_name = cgroup_readdir_name, .permissions = cgroup_permissions, .create_node = cgroup_create,
    .getattr = cgroup_stat, .statfs = cgroup_statfs, .write = cgroup_write, .truncate = cgroup_truncate,
    .sync = cgroup_sync, .remove_node = cgroup_remove,
};

/** @brief Construct a new attachment of the permanent unified cgroup hierarchy. */
int cgroup2_create_vfs_mount(const char *source, uint64_t flags, const char *options, struct vfs_mount **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (flags & ~(VFS_MOUNT_POLICY_MASK | MS_STRICTATIME)) return -EOPNOTSUPP;
    int rc = cgroup_remount(&cgroup_root, flags & MS_RDONLY, options);
    if (rc) return rc;
    struct storage_node root = cgroup_node(&cgroup_root, CG_DIR, 0);
    rc = vfs_mount_new(&cgroup_ops, &cgroup_root, NULL, &root, out);
    if (rc) return rc;
    rc = vfs_mount_set_flags(*out, flags);
    if (!rc) rc = vfs_mount_describe(*out, source && *source ? source : "cgroup2", "cgroup2", CGROUP_DEVICE);
    if (rc) { vfs_mount_put(*out); *out = NULL; }
    return rc;
}
