/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/deferred.h>
#include <reliefnt/heap.h>
#include <reliefnt/mount.h>
#include <reliefnt/sched.h>
#include <reliefnt/text_stream.h>
#include <linux/errno.h>

#define CGROUP_INDEX_BUCKETS 256u
#define CGROUP_FILE_BIT(file) (1u << (file))
#define CGROUP_PIDS_EVENT_BITS (CGROUP_FILE_BIT(CG_PIDS_EVENTS) | CGROUP_FILE_BIT(CG_PIDS_LOCAL))

struct cgroup cgroup_root = {.id=1,.refs=1,.linked=true,.mode={0755,0,0}};
static struct cgroup *index[CGROUP_INDEX_BUCKETS];
static uint32_t last_id = 1;
static struct cgroup *work_head, **work_tail = &work_head;
static bool work_pending;

/** @brief Find one group, including unlinked but retained identities. */
struct cgroup *cgroup_find(uint32_t id)
{
    if (id == cgroup_root.id) return &cgroup_root;
    for (struct cgroup *g = index[id % CGROUP_INDEX_BUCKETS]; g; g = g->index_next)
        if (g->id == id) return g;
    return NULL;
}

/** @brief Coalesce a group's work without allocation or a scheduler wakeup. */
static void queue_locked(struct cgroup *g, uint16_t bits)
{
    g->pending |= bits;
    if (g->queued) return;
    g->queued = true;
    *work_tail = g;
    work_tail = &g->work_next;
    __atomic_store_n(&work_pending, true, __ATOMIC_RELEASE);
}

/** @brief Schedule delivery only after dropping the scheduler lock. */
void cgroup2_kick(void)
{
    if (__atomic_load_n(&work_pending, __ATOMIC_ACQUIRE))
        kernel_deferred_schedule(DEFERRED_VFS);
}

/** @brief Retain inode, child or task ownership of a group. */
void cgroup_get_locked(struct cgroup *g) { ++g->refs; }

/** @brief Defer reclamation even if the final pin is dropped from task exit. */
void cgroup_put_locked(struct cgroup *g)
{
    if (!--g->refs) queue_locked(g, 0);
}

/** @brief Construct a fully initialized empty child before publishing it. */
int cgroup_mkdir(struct cgroup *parent, const char *name, const struct reliefos_permissions *mode,
                 struct cgroup **out)
{
    if (!parent->linked) return -ENOENT;
    if (!__builtin_strncmp(name, "cgroup.", 7)) return -EINVAL;
    for (struct cgroup *child = parent->children; child; child = child->sibling)
        if (!__builtin_strcmp(child->name, name)) return -EEXIST;
    size_t n = __builtin_strlen(name);
    if (parent->path_length + 1 + n >= VFS_PATH_MAX) return -ENAMETOOLONG;
    if (last_id == UINT32_MAX) return -ENOSPC;
    struct cgroup *g = kernel_malloc(sizeof(*g) + n + 1);
    if (!g) return -ENOMEM;
    *g = (struct cgroup){.parent=parent,.refs=1,.linked=true,.limit=CGROUP_PIDS_UNLIMITED,
        .mode={mode->mode & 0777u,mode->uid,mode->gid},.id=++last_id,
        .path_length=(uint16_t)(parent->path_length+1+n),.depth=(uint16_t)(parent->depth+1)};
    __builtin_memcpy(g->name, name, n+1);
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    cgroup_get_locked(parent);
    g->sibling = parent->children;
    parent->children = g;
    for (struct cgroup *p = parent; p; p = p->parent) ++p->descendants;
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    struct cgroup **bucket = &index[g->id % CGROUP_INDEX_BUCKETS];
    g->index_next = *bucket;
    *bucket = g;
    *out = g;
    return 0;
}

/** @brief Unpublish a child only when no live task or linked child remains. */
int cgroup_rmdir(struct cgroup *g)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    int rc = !g->linked ? -ENOENT : !g->parent || g->children || g->live ? -EBUSY : 0;
    if (!rc) {
        struct cgroup **link = &g->parent->children;
        while (*link != g) link = &(*link)->sibling;
        *link = g->sibling;
        g->linked = false;
        g->pending = 0;
        for (struct cgroup *p = g->parent; p; p = p->parent) { --p->descendants; ++p->dying; }
        cgroup_put_locked(g);
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    if (!rc) cgroup2_kick();
    return rc;
}

/** @brief Return whether pids files and enforcement are enabled at this group. */
bool cgroup_pids_enabled(const struct cgroup *g)
{ return g->parent && g->parent->subtree_pids; }

/** @brief Change the state of all direct pids children in one locked transaction. */
int cgroup_set_subtree_locked(struct cgroup *g, bool enabled)
{
    if (g->subtree_pids == enabled) return 0;
    if (enabled && g->parent && !cgroup_pids_enabled(g)) return -ENOENT;
    if (enabled && g->pids_epoch == CGROUP_PIDS_EPOCH_MAX) return -ENOSPC;
    if (!enabled)
        for (struct cgroup *child = g->children; child; child = child->sibling)
            if (child->subtree_pids) return -EBUSY;
    for (struct cgroup *child = g->children; child; child = child->sibling) {
        child->pending &= ~CGROUP_PIDS_EVENT_BITS;
        child->limit = CGROUP_PIDS_UNLIMITED;
        child->events = child->local_events = 0;
        child->peak = child->charged;
    }
    if (enabled) ++g->pids_epoch;
    g->subtree_pids = enabled;
    return 0;
}

/** @brief Apply signed task/population deltas below (but not including) stop.
 * Root needs no population counter or PID charge. Caller holds scheduler_lock. */
static void adjust_locked(struct cgroup *g, const struct cgroup *stop, int64_t charged, int64_t live)
{
    for (; g && g->parent && g != stop; g = g->parent) {
        bool populated = g->live != 0;
        g->live += live;
        g->charged += charged;
        if (g->charged > g->peak) g->peak = g->charged;
        if (populated != (g->live != 0) && g->linked) queue_locked(g, CGROUP_FILE_BIT(CG_EVENTS));
    }
}

/** @brief Check whether one more thread fits the closest applicable limits. */
bool cgroup2_fork_allowed(const struct task *parent)
{
    for (struct cgroup *g = parent ? parent->cgroup : NULL; g && g->parent; g = g->parent) {
        if (!g->linked || !cgroup_pids_enabled(g) || g->charged < g->limit) continue;
        ++g->local_events;
        queue_locked(g, CGROUP_FILE_BIT(CG_PIDS_LOCAL));
        for (struct cgroup *p = g; p->parent; p = p->parent) {
            ++p->events;
            queue_locked(p, CGROUP_FILE_BIT(CG_PIDS_EVENTS));
        }
        return false;
    }
    return true;
}

/** @brief Charge a new thread only once all fallible construction has succeeded. */
void cgroup2_fork_commit(struct task *child, const struct task *parent)
{
    struct cgroup *g = parent ? parent->cgroup : NULL;
    if (!g) return;
    child->cgroup = g;
    cgroup_get_locked(g);
    adjust_locked(g, NULL, 1, 1);
}

/** @brief Separate live population from the PID charge of a waitable zombie. */
void cgroup2_task_exit(struct task *task)
{
    if (!task->cgroup) return;
    adjust_locked(task->cgroup, NULL, 0, -1);
    if (!(task->flags & TASK_FLAG_WAITABLE_CHILD) || !task->parent_pid) cgroup2_task_reap(task);
}

/** @brief Release an exiting thread's or reaped zombie's charge without VFS I/O. */
void cgroup2_task_reap(struct task *task)
{
    struct cgroup *g = task->cgroup;
    if (!g) return;
    task->cgroup = NULL;
    adjust_locked(g, NULL, -1, 0);
    cgroup_put_locked(g);
}

/** @brief Match a live user thread (including blocked or stopped tasks). */
bool cgroup_live_task(const struct task *task)
{ return task->pid && task->kind == TASK_KIND_USER && task->state != TASK_EXITED &&
    !(task->flags & TASK_FLAG_LAUNCH_PREPARING); }

/** @brief Find the common ancestor so a migration never changes its population. */
static struct cgroup *common_ancestor(struct cgroup *a, struct cgroup *b)
{
    while (a->depth > b->depth) a = a->parent;
    while (b->depth > a->depth) b = b->parent;
    while (a != b) { a = a->parent; b = b->parent; }
    return a;
}

/** @brief Migrate the live process, not the selected individual thread.
 * An exited leader with surviving threads still identifies that process.
 * Migration may exceed a limit; subsequent fork admission enforces it. */
int cgroup_move_locked(struct cgroup *destination, uint32_t pid)
{
    if (!destination->linked) return -ENODEV;
    struct task *target = pid ? sched_find(pid) : sched_current_task();
    if (!target || !target->pid || target->kind != TASK_KIND_USER) return -ESRCH;
    uint32_t tgid = sched_task_tgid(target);
    if (!cgroup_live_task(target) && target->pid != tgid) return -ESRCH;
    struct cgroup *old = NULL;
    int64_t moved = 0;
    struct task *task;
    for (uint32_t i = 0; (task = sched_task_at(i)); ++i) {
        if (!cgroup_live_task(task) || sched_task_tgid(task) != tgid) continue;
        struct cgroup *from = task->cgroup ? task->cgroup : &cgroup_root;
        if (destination == from) return 0;
        old = from;
        if (task->cgroup) cgroup_put_locked(task->cgroup);
        task->cgroup = destination->parent ? destination : NULL;
        if (task->cgroup) cgroup_get_locked(task->cgroup);
        ++moved;
    }
    if (!moved) return -ESRCH;
    struct cgroup *ancestor = common_ancestor(old, destination);
    adjust_locked(old, ancestor, -moved, -moved);
    adjust_locked(destination, ancestor, moved, moved);
    return 0;
}

/** @brief Produce a Linux unified membership record without resolving mounts. */
void cgroup2_proc_emit(const struct task *task, struct text_stream *stream)
{
    char path[VFS_PATH_MAX];
    unsigned start = sizeof(path);
    uint64_t flags;
    kernel_spin_lock_irqsave(&scheduler_lock, &flags);
    const struct cgroup *g = task->cgroup;
    bool deleted = g && !g->linked;
    while (g && g->parent) {
        unsigned n = g->path_length - g->parent->path_length - 1u;
        start -= n;
        __builtin_memcpy(path+start, g->name, n);
        path[--start] = '/';
        g = g->parent;
    }
    kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
    text_string(stream, "0::");
    if (start == sizeof(path)) text_string(stream, "/");
    else text_bytes(stream, path+start, sizeof(path)-start);
    if (deleted) text_string(stream, " (deleted)");
    text_string(stream, "\n");
}

/** @brief Remove a zero-ref group from the index and release its retained ancestry.
 * Must run under execution ownership with scheduler_lock held. Caller frees
 * the now unreachable record outside the lock. */
static void retire_locked(struct cgroup *g)
{
    struct cgroup **link = &index[g->id % CGROUP_INDEX_BUCKETS];
    while (*link != g) link = &(*link)->index_next;
    *link = g->index_next;
    for (struct cgroup *p = g->parent; p; p = p->parent) --p->dying;
    cgroup_put_locked(g->parent);
}

/** @brief Flush pending events by immutable key after leaving the task lock. */
void cgroup2_service(unsigned budget)
{
    while (budget--) {
        uint64_t flags;
        kernel_spin_lock_irqsave(&scheduler_lock, &flags);
        struct cgroup *g = work_head;
        if (!g) { kernel_spin_unlock_irqrestore(&scheduler_lock, flags); break; }
        work_head = g->work_next;
        if (!work_head) {
            work_tail = &work_head;
            __atomic_store_n(&work_pending, false, __ATOMIC_RELEASE);
        }
        g->work_next = NULL;
        g->queued = false;
        uint16_t pending = g->linked ? g->pending : 0;
        g->pending = 0;
        uint32_t epoch = g->parent ? g->parent->pids_epoch : 0;
        bool free_group = g->refs == 0;
        if (free_group) retire_locked(g);
        uint32_t id = g->id;
        kernel_spin_unlock_irqrestore(&scheduler_lock, flags);
        if (free_group) { kernel_free(g); continue; }
        /* Delivery can wake waiters; keep no borrowed group across callbacks. */
        const struct cgroup key = {.id=id};
        for (enum cgroup_file file = CG_EVENTS; pending && file < CG_FILE_COUNT; ++file) {
            if (!(pending & CGROUP_FILE_BIT(file))) continue;
            struct storage_node node = cgroup_node(&key, file, epoch);
            cgroup_notify(&node, false);
        }
    }
    cgroup2_kick();
}
