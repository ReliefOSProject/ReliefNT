/* Resolve inline versus shared task ownership domains. */
#ifndef RELIEFNT_TASK_ACCESSORS_H
#define RELIEFNT_TASK_ACCESSORS_H
#include <reliefnt/task/types.h>

/** @brief Retain a stable PID identity that cannot retarget on task-slot reuse.
 * @param task Live task or waitable zombie, borrowed under execution ownership.
 * @param out Empty output receiving one identity reference, empty on failure.
 * @return Zero or negative EINVAL/ESRCH/ENOMEM, without a partial task binding.
 * Lazy allocation requires task context without scheduler_lock. The task keeps
 * its own reference until reap/recycle; proc inodes keep independent references.
 */
int sched_task_identity_get(struct task *task,struct task_identity **out);
/** @brief Borrow the task currently carrying a retained PID identity.
 * @param identity Held identity, nullable, under execution ownership.
 * @return Borrowed task, or NULL after reap/recycle. Never a replacement task.
 */
struct task *sched_task_identity_task(const struct task_identity *identity);
/** @brief Release one retained PID identity without touching task resources.
 * @param identity Owned identity, or NULL. Execution ownership required.
 * Only the last reference frees the identity; no backend I/O or allocation.
 */
void sched_task_identity_put(struct task_identity *identity);
/** @brief Invalidate a task's PID identity before reaping or reusing its record.
 * @param task Stable task under execution ownership or scheduler_lock, nullable.
 * Existing proc references become dead and cannot retarget a later task/PID.
 * Drops only the task's reference; no backend I/O or allocation.
 */
void sched_task_identity_detach(struct task *task);
/** @brief Move retained PID identities together with exec's native PID swap.
 * @param left Stable task exchanging its PID under scheduler_lock.
 * @param right Other stable task exchanging its PID under scheduler_lock.
 * Neither pointer may be NULL. No allocation or reference-count changes;
 * retained proc identities continue to follow their original PID objects.
 */
void sched_task_identity_swap(struct task *left,struct task *right);

/** @brief Prepare current-task state and owned root/cwd paths for image commit.
 * @param task Current task under exclusive execution ownership; required.
 * @return Zero, a file-table error, or EAGAIN until remote siblings retire.
 * Detaches native shared FS state without losing object pins; no caller-held
 * scheduler_lock. Signal state updates take that lock internally.
 */
int sched_prepare_exec_current(struct task *task);
void sched_signalfd_reconfigure(struct task *task);
void sched_exec_replace_mm(struct task *task, const struct address_space *replacement);

/* Inline storage is promoted on first sharing. All users of an ownership
 * domain must resolve through these accessors, including page-fault paths. */
/** @brief Resolve the process-wide resource limits shared by CLONE_THREAD. */
static inline struct task_rlimit_state *sched_task_limits(const struct task *task)
{
    return task->shared_limits ? task->shared_limits : (struct task_rlimit_state *)&task->limits;
}

uint64_t sched_user_task_count(uint32_t uid);

static inline struct task_address_space_state *sched_task_mm(const struct task *task)
{
    return task->shared_mm ? task->shared_mm : (struct task_address_space_state *)&task->address_space;
}

static inline struct address_space *sched_task_as(const struct task *task)
{
    return &sched_task_mm(task)->as;
}

/**
 * @brief Reset shared-MM dumpability before a credential change becomes visible.
 * @param task Task changing credentials under the execution lock.
 * @param euid Next effective user ID.
 * @param egid Next effective group ID.
 * @param fsuid Next filesystem user ID.
 * @param fsgid Next filesystem group ID.
 * @param permitted Next permitted capability set.
 * Linux commit_creds uses suid_dumpable, whose default is zero.
 */
static inline void task_credentials_prepare(struct task *task, uint32_t euid, uint32_t egid,
                                            uint32_t fsuid, uint32_t fsgid, uint64_t permitted)
{
    if (task->euid != euid || task->egid != egid || task->fsuid != fsuid ||
        task->fsgid != fsgid || (permitted & ~task->cap_permitted)) {
        sched_task_mm(task)->nondumpable = true;
        /* Linux commit_creds clears the parent-death signal on any effective
         * or filesystem ID change and on permitted capability gains. */
        task->pdeath_signal = 0;
        __atomic_thread_fence(__ATOMIC_RELEASE);
    }
}

static inline struct task_fd_table_state *sched_task_fds(const struct task *task)
{
    return task->shared_files ? task->shared_files : (struct task_fd_table_state *)&task->fd_table;
}

/**
 * @brief Return the number of live indexable slots in the task's PTY alias table.
 * @param task Descriptor-table owner; NULL returns zero.
 * @return Slot count, including holes that can be reused by a later alias.
 */
uint32_t sched_task_pty_capacity(const struct task *task);

/**
 * @brief Resolve an existing PTY alias slot without growing the descriptor table.
 * @param task Descriptor-table owner; NULL is accepted.
 * @param index Zero-based slot in the live alias table.
 * @return Borrowed slot, or NULL when index is outside the current slot count.
 * @note A later PTY table growth can invalidate the returned pointer.
 */
struct task_pty_fd *sched_task_pty_at(struct task *task, uint32_t index);

/**
 * @brief Reserve PTY alias storage in a detached descriptor-table copy.
 * @param table Descriptor table whose owned PTY vector may be replaced; required.
 * @param wanted Number of slots needed; this call does not change the live count.
 * @return Zero on success or negative EINVAL/ENOMEM without changing table storage.
 * @note Successful growth invalidates pointers borrowed from the previous vector.
 * The caller must serialize table mutation and must not retain entry pointers across growth.
 */
int sched_task_pty_table_reserve(struct task_fd_table_state *table, uint32_t wanted);

/**
 * @brief Extend the task's PTY alias table by one slot or reuse an empty slot.
 * @param task Descriptor-table owner; required.
 * @param out Receives the empty slot; required.
 * @return Zero on success or negative EINVAL/ENOMEM without publishing a slot.
 * @note The returned pointer is borrowed and may be invalidated by later growth.
 */
int sched_task_pty_alloc(struct task *task, struct task_pty_fd **out);

/**
 * @brief Free PTY alias storage after descriptor references and table sharing end.
 * @param task Descriptor-table owner whose entries are released and table is final; NULL is accepted.
 */
void sched_task_pty_release(struct task *task);

static inline char *sched_task_cwd(const struct task *task)
{
    return task->shared_fs ? task->shared_fs->cwd : (char *)task->cwd;
}

/** @brief Borrow the task's owned mount/root/cwd handles, honoring CLONE_FS.
 * @param task Stable task under execution ownership; required.
 * @return Mutable owned state; no references are transferred to the caller.
 */
static inline struct task_fs_paths *sched_task_paths(const struct task *task)
{
    return task->shared_fs ? &task->shared_fs->paths : (struct task_fs_paths *)&task->fs_paths;
}

/** @brief Initialize a new unpublished task's root/cwd from the shared namespace.
 * @param task Fresh filesystem state under execution ownership.
 * @param ns Published shared namespace; borrowed for this call.
 * @return Zero, ENODEV before namespace publication, or EINVAL for partial
 * state. Fully initialized inherited state is preserved. No allocation/I/O;
 * may run while the scheduler excludes publication of the new task.
 */
int sched_task_fs_start(struct task *task,struct vfs_namespace *ns);

/** @brief Copy independent root/cwd references when creating an unshared task.
 * @param task New task with private filesystem state under execution ownership.
 * @param parent Stable source task whose root/cwd references are borrowed.
 * No allocation; new pins are acquired before any old destination pins drop.
 */
void sched_task_fs_inherit(struct task *task,const struct task *parent);

/** @brief Detach shared FS state while retaining the same root/cwd objects.
 * @param task Task under exclusive execution ownership; NULL is accepted.
 * Preserves the existing native exec unshare policy and cached names/umask.
 * No allocation; the last shared reference releases its backend pins only
 * after the task has acquired independent pins. Final release may perform I/O.
 */
void sched_task_fs_unshare(struct task *task);

/** @brief Retire task root/cwd pins or its one shared CLONE_FS reference.
 * @param task Quiescent task under execution ownership; nullable.
 * Clears ownership before final backend release, which may perform I/O.
 * Only the final shared holder releases the underlying paths and state.
 */
void sched_task_fs_release(struct task *task);

static inline uint32_t *sched_task_umask(const struct task *task)
{
    return task->shared_fs ? &task->shared_fs->umask : (uint32_t *)&task->umask;
}

static inline char *sched_task_root_dir(const struct task *task)
{
    return task->shared_fs ? task->shared_fs->root_dir : (char *)task->root_dir;
}

static inline const char *sched_task_root(const struct task *task)
{
    const char *root = task ? sched_task_root_dir(task) : NULL;
    return root && root[0] ? root : "/";
}

static inline struct kernel_signal_action *sched_task_actions(const struct task *task)
{
    return task->shared_sighand ? task->shared_sighand->actions :
        (struct kernel_signal_action *)task->signal_actions;
}

static inline uint32_t sched_task_tgid(const struct task *task)
{
    return task->tgid ? task->tgid : task->pid;
}

static inline uint64_t *sched_task_process_pending(const struct task *task)
{
    return task->shared_process_pending ? task->shared_process_pending :
        (uint64_t *)&task->process_pending_signals;
}

static inline uint64_t sched_task_pending(const struct task *task)
{
    return task->pending_signals | *sched_task_process_pending(task);
}

static inline struct kernel_sigqueue *sched_task_process_signal_queue(struct task *task)
{
    return task->shared_process_signal_queue ? task->shared_process_signal_queue :
        &task->process_signal_queue;
}

/** @brief Clone or share a user's resources and owned filesystem paths.
 * @param frame Borrowed saved user frame, required.
 * @param flags Supported Linux clone flags and exit signal.
 * @param stack Optional new user stack, zero inherits.
 * @param parent_tid Optional writable parent-side TID address.
 * @param child_tid Optional writable child-side TID address.
 * @param tls User TLS base for CLONE_SETTLS.
 * @return Child PID or negative errno; rollback releases only child ownership.
 * Execution transaction required; takes scheduler_lock. CLONE_FS shares the
 * actual root/cwd owner; ordinary fork retains independent path references.
 */
int64_t sched_clone_current(const struct trap_frame *frame, uint64_t flags,
                           uint64_t stack, uint64_t parent_tid,
                           uint64_t child_tid, uint64_t tls);
/**
 * @brief Native vfork(2): CLONE_VM|CLONE_VFORK|SIGCHLD through the shared
 * clone path so the caller waits for the child's exec or final exit.
 */
int64_t sched_vfork_current(const struct trap_frame *frame);
/**
 * @brief True while task is the parent blocked in a CLONE_VFORK wait.
 *
 * The link is published and cleared under scheduler_lock. Scheduler selection
 * also checks it, so an unrelated wakeup cannot return the parent to userspace.
 */
static inline bool sched_vfork_waiting(const struct task *task)
{
    return task && task->vfork_child != NULL;
}
void sched_exit_group(uint32_t tgid, uint64_t code);

struct task_snapshot_info {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t state;
    uint32_t kind;
    uint32_t flags;
    uint32_t uid;
    uint32_t role;
    uint32_t session_id;
    uint32_t memory_kib;
    uint64_t cpu_ticks;
    int32_t priority;
    uint32_t pending_signals;
    uint64_t wake_tick;
    uint64_t entry;
    uint64_t cr3;
    uint64_t affinity_mask;
    char name[SCHED_TASK_NAME_LEN];
    char username[RELIEFOS_AUTH_USERNAME_LEN];
};


#endif
