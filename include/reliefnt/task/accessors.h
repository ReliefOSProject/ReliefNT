/* Resolve inline versus shared task ownership domains. */
#ifndef RELIEFNT_TASK_ACCESSORS_H
#define RELIEFNT_TASK_ACCESSORS_H
#include <reliefnt/task/types.h>

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

static inline char *sched_task_cwd(const struct task *task)
{
    return task->shared_fs ? task->shared_fs->cwd : (char *)task->cwd;
}

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
