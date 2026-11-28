/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_CGROUP_INTERNAL_H
#define RELIEFNT_CGROUP_INTERNAL_H
#include <reliefnt/cgroup.h>
#include <reliefnt/lock.h>
#include <reliefnt/storage.h>

#define CGROUP_PIDS_MAX 4194304u
#define CGROUP_PIDS_UNLIMITED UINT64_MAX
#define CGROUP_PIDS_EPOCH_MAX (UINT32_MAX >> 4)
#define CGROUP_DEVICE 203u
#define CGROUP_MAGIC 0x63677270u

enum cgroup_file {
    CG_DIR, CG_PROCS, CG_THREADS, CG_CONTROLLERS, CG_SUBTREE, CG_TYPE, CG_EVENTS, CG_STAT,
    CG_PIDS_MAX, CG_PIDS_CURRENT, CG_PIDS_PEAK, CG_PIDS_EVENTS, CG_PIDS_LOCAL, CG_FILE_COUNT,
};

/* Topology/index and VFS operations require the execution writer. All task
 * membership, counters, controller state, refs and pending bits are also
 * serialized by scheduler_lock, including updates from remote task exit. */
struct cgroup {
    struct cgroup *parent, *children, *sibling, *index_next, *work_next;
    uint64_t refs, live, charged, limit, peak, events, local_events;
    uint32_t id, pids_epoch, descendants, dying;
    uint16_t path_length, depth, pending;
    bool linked, subtree_pids, queued;
    struct reliefos_permissions mode;
    char name[];
};

extern struct kernel_spinlock scheduler_lock;
extern struct cgroup cgroup_root;

/** @brief Borrow an ID-indexed object under VFS execution ownership. */
struct cgroup *cgroup_find(uint32_t id);
/** @brief Allocate and publish an empty child and its globally unique identity. */
int cgroup_mkdir(struct cgroup *parent, const char *name, const struct reliefos_permissions *mode,
                 struct cgroup **out);
/** @brief Unlink an empty group without invalidating retained task or inode pins. */
int cgroup_rmdir(struct cgroup *group);
/** @brief Acquire one more reference to a live group; caller holds scheduler_lock. */
void cgroup_get_locked(struct cgroup *group);
/** @brief Drop one reference, queuing final release; caller holds scheduler_lock. */
void cgroup_put_locked(struct cgroup *group);
/** @brief Test controller availability while holding scheduler_lock. */
bool cgroup_pids_enabled(const struct cgroup *group);
/** @brief Switch child controller visibility atomically with fork admission. */
int cgroup_set_subtree_locked(struct cgroup *group, bool enabled);
/** @brief Move the live thread group identified by PID or TID under scheduler_lock. */
int cgroup_move_locked(struct cgroup *destination, uint32_t pid);
/** @brief Identify a live, published user thread in the locked task table. */
bool cgroup_live_task(const struct task *task);
/** @brief Export an inode's globally unique key for a borrowed group and epoch. */
struct storage_node cgroup_node(const struct cgroup *group, enum cgroup_file file, uint32_t epoch);
/** @brief Deliver an event-key snapshot outside scheduler_lock under execution ownership. */
void cgroup_notify(const struct storage_node *node, bool removed);
#endif
