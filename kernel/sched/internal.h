#ifndef RELIEFNT_KERNEL_SCHED_SCHED_C_INTERNAL_H
#define RELIEFNT_KERNEL_SCHED_SCHED_C_INTERNAL_H

/*
 * ReliefOS scheduler implementation: manages kernel and Ring-3 task state.
 * Selects runnable tasks, handles waits/exits, and switches address spaces.
 */
#include <reliefnt/console.h>
#include <reliefnt/audio.h>
#include <reliefnt/sysv_shm.h>
#include <reliefnt/bugcheck.h>
#include <reliefnt/arch.h>
#include <reliefnt/paging.h>
#include <reliefnt/pty.h>
#include <reliefnt/sched.h>
#include <reliefnt/drm.h>
#include <reliefnt/loadavg.h>
#include <reliefnt/permissions.h>
#include <reliefnt/storage.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>
#include <reliefnt/lock.h>
#include <reliefnt/smp.h>
#include <reliefnt/svga.h>
#include <reliefnt/usercopy.h>
#include <linux/sched.h>
#include <reliefnt/futex.h>
#include <reliefnt/wait.h>
#include <reliefnt/time.h>
#include <linux/time.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/fcntl.h>

/* The table grows by moving only pointers; task objects keep stable addresses
 * because wait queues and interrupt paths may retain struct task pointers. */


/* A returning task may skip the full scheduler round only when its own CPU
 * has no pending tick or runnable-task notification. */




/* Per-CPU O(1) mirror of the task owning current_pid[]. Updated at every
 * current_pid write so sched_current_task() and the frame capture avoid a
 * linear task-table scan on each syscall return. */




/* CPU time uses the BSP's USER_HZ reference on every CPU. LAPIC IRQs are
 * preemption requests, not a clock: their rate may differ during calibration
 * or fallback. The BSP samples CPU ownership at the existing 10 ms precision;
 * no unverified cross-CPU TSC interpolation enters CPU usage statistics. */

_Static_assert(RELIEFNT_TICK_HZ == 100, "CPU ticks use Linux USER_HZ=100");







/* A READY task can still be owned by the CPU that just saved its interrupt
 * frame.  Keep that reservation until the owner either reclaims or replaces
 * it; otherwise another CPU can select the same address space concurrently. */
#define SCHED_CPU_NONE UINT32_MAX







/* Waking a task can make it eligible on any CPU allowed by its affinity. Mark
 * every online CPU here so a remote wake cannot be hidden by another CPU's
 * scheduler round. Wake callers are serialized with scheduling either by
 * scheduler_lock or by the kernel execution transaction. */


/* This helper deliberately does not take scheduler_lock: callers must have
 * already serialized their runnable-state transition (or hold that lock). */
















/* EEVDF integration; included by sched.c and serialized by scheduler_lock.
 * Keep task lifecycle/state ownership in sched.c. The queues contain both
 * runnable entities and sleepers whose negative lag has not decayed yet. */


































































/* Keep the scheduler-private tree node next to its task without changing
 * struct task's size or any externally visible task field offset. */
struct sched_task_object {
    struct task task;
    struct eevdf_node fair_node;
};
_Static_assert(__builtin_offsetof(struct sched_task_object, task) == 0,
               "task must be the first member of its private allocation");









/* All helpers run under scheduler_lock. Reading statistics must not advance
 * EEVDF runtime or renew its deadlines. scheduler_ticks needs no clock lock. */






























































/* Publish one group-exit notification, after the last user frame retires.
 * Individual pthread exits and sibling destruction during exec are silent. */












































































































































/* Signal delivery is handled by pty_destroy; held endpoint FDs survive detach. */


struct cpu_runtime_accounting {
    uint64_t last_tick;
    uint64_t busy_ticks;
    uint64_t idle_ticks;
    bool started;
};

extern struct task **tasks;
extern uint32_t task_count;
extern uint32_t task_capacity;
extern uint32_t next_pid;
extern uint32_t current_pid[SMP_MAX_CPUS];
extern struct task *current_task[SMP_MAX_CPUS];
extern uint32_t next_session_id;
extern uint64_t scheduler_ticks;
extern uint64_t scheduler_loads[3];
extern struct cpu_runtime_accounting cpu_accounting[SMP_MAX_CPUS];
extern struct kernel_spinlock scheduler_lock;
extern struct eevdf_queue fair_queues[SMP_MAX_CPUS];
extern struct task *fair_running[SMP_MAX_CPUS];
extern uint64_t fair_started[SMP_MAX_CPUS];
extern uint64_t fair_clock;

void sched_truncate_file_mappings(const struct storage_node *node, uint64_t size);

bool thread_group_pending(uint32_t tgid, const struct task *except);

uint32_t scheduler_cpu_index(void);

void sched_request_reschedule_all_online(void);

void sched_notify_runnable(void);

bool sched_current_cpu_needs_resched(void);

uint64_t sched_user_task_count(uint32_t uid);

bool task_nproc_denied_locked(const struct task *task);

uint64_t sched_all_cpu_mask(void);

bool task_cpu_allowed(const struct task *task, uint32_t cpu);

uint32_t scheduler_current_pid(void);

uint64_t fair_now(void);

void fair_account_cpu(uint32_t cpu, uint64_t now);

void fair_stop_cpu(uint32_t cpu);

void fair_forget(struct task *task);

void fair_sync(void);

struct task *fair_select(uint32_t cpu);

struct task_vma *sched_task_vma_at(struct task *task, uint32_t index);

uint32_t sched_task_vma_capacity(const struct task *task);

void sched_task_vma_release(struct task *task);

/**
 * @brief Retain the PCM mapping reference represented by a copied VMA.
 * @param dst Child VMA receiving the copied audio mapping metadata.
 * @param src Parent VMA whose mapping reference is still live.
 * @return Zero when no mapping exists or the reference was retained, else a negative errno.
 */
int sched_task_vma_retain_audio(struct task_vma *dst,
                                       const struct task_vma *src);

/** @brief Retain the BO and OFD references represented by a copied DRM VMA. */
int sched_task_vma_retain_drm(struct task_vma *dst,
                                     const struct task_vma *src);

/** @brief Resolve a Linux descriptor number without acquiring a description.
 * @param task Owner under the execution lock. @param fd Linux fd, not array index.
 * @return Borrowed live slot or NULL. Includes stdio and the grown file table.
 */
struct task_file *task_descriptor_for_fd(struct task *task, int fd);

struct task_file *sched_task_file_at(struct task *task, uint32_t index);

uint32_t sched_task_file_capacity(const struct task *task);

void sched_task_file_release(struct task *task);

/**
 * @brief Copy name into the task's fixed buffer (bounded, NUL-terminated) and point task->name at it.
 */
void task_copy_name(struct task *task, const char *name);

/**
 * @brief Copy cwd into the task's fixed buffer, defaulting an empty or NULL value to "/".
 */
void task_copy_cwd(struct task *task, const char *cwd);

/**
 * @brief Copy path into the task's fixed buffer, NUL-terminating even for a NULL source.
 */
void task_copy_path(struct task *task, const char *path);

/**
 * @brief Bounded, always-NUL-terminated copy used for username and home identity strings.
 */
void task_copy_identity_text(char *dst, size_t cap, const char *src);

/**
 * @brief Strip admin flag, uid, role, session, username, and home, leaving an anonymous task.
 */
void task_clear_identity(struct task *task);

/**
 * @brief Inherit uid, role, session, username, and home from the parent task.
 */
void task_copy_identity_from_parent(struct task *task, const struct task *parent);

/**
 * @brief Reset the task table, pid/session counters, and tick statistics to their initial state.
 */
void sched_init(void);

void task_fair_bind(struct task *task);

void task_fair_reset(struct task *task);

/**
 * @brief Clear every byte of a task struct so it starts from a well-defined zero state.
 */
void task_zero(struct task *task);

void cpu_accounting_settle(uint32_t cpu);

void cpu_accounting_sync(void);

/** @brief Drop the resource-limit reference only when a task is reaped/recycled. */
void task_release_limits(struct task *task);

/**
 * @brief Scan forward from next_pid for an unused non-zero pid; 0 when the identifier space is exhausted.
 */
uint32_t task_allocate_pid(void);

/**
 * @brief Return a zeroed task slot, recycling a reaped EXITED entry or growing the table as needed.
 */
struct task *alloc_task_slot(void);

/**
 * @brief Allocate and initialize a kernel task with an entry point, no stack, and cleared identity.
 */
uint32_t sched_create_kernel_task(const char *name, uint64_t entry);

/**
 * @brief Allocate a user task, build its address space and stack, and inherit cwd/identity from parent.
 */
uint32_t sched_create_user_task(const char *name, uint64_t entry, uint64_t stack_top,
                                uint32_t parent_pid, uint32_t flags);

/**
 * @brief Finish a child-side CLONE_VFORK wait under scheduler_lock.
 *
 * Linux completes tsk->vfork_done from mm_release() on both the exec and the
 * final exit path.  The parent is woken only from TASK_BLOCKED; a parent that
 * was concurrently killed or stopped keeps its terminal/job-control state and
 * simply loses the stale child pointer.
 */
void vfork_child_done_locked(struct task *child);

/** @brief Drop a dying parent's reference to its vfork child under lock. */
void vfork_parent_exit_locked(struct task *parent);

/**
 * @brief Public non-locking wrapper used by exec/exit lifecycle code.
 *
 * Called with the scheduler unlocked after the child is committed to exec or
 * terminally exiting.  Keep the link cleared even when the parent already
 * died: a later task slot reuse must never observe a stale parent pointer.
 */
void sched_vfork_child_done(struct task *child);

int64_t sched_clone_current(const struct trap_frame *parent_frame, uint64_t flags,
                           uint64_t stack, uint64_t parent_tid,
                           uint64_t child_tid, uint64_t tls);

int64_t sched_fork_current(const struct trap_frame *frame);

int64_t sched_vfork_current(const struct trap_frame *frame);

/**
 * @brief Attach an in-memory image and its length to the task with the given pid.
 */
void sched_set_task_image(uint32_t pid, const void *image, size_t image_len);

/**
 * @brief Record the storage node backing a task's image and flag it for a lazy load.
 */
void sched_set_task_image_node(uint32_t pid, const struct storage_node *node);

/**
 * @brief Copy path into the task's fixed path field, resolving the task by pid.
 */
void sched_set_task_path(uint32_t pid, const char *path);

/**
 * @brief Store clamped argc/argv/envc/envp/data, rebinding interior pointers into the owned buffer.
 */
void sched_copy_task_exec_params(struct task *task,
                                uint32_t argc, char *const argv[],
                                uint32_t envc, char *const envp[],
                                const char *data, uint32_t data_len);

void sched_set_task_exec_params(uint32_t pid, uint32_t argc, char *const argv[],
                                uint32_t envc, char *const envp[],
                                const char *data, uint32_t data_len);

/**
 * @brief Create the pid-0 kernel idle task and mark it the running task.
 */
void sched_create_idle_task(void);

/**
 * @brief Make pid the running task and demote every other RUNNING task back to READY.
 */
void sched_set_running(uint32_t pid);

void sched_mark_orphaned_group(uint32_t group);

/**
 * @brief Publish terminal task state and log its first exit after releasing the scheduler lock.
 * @param pid Task identifier; PID 1 panics and an absent PID has no exit event.
 * @param code Exit status; repeated calls retain the existing status-update and cleanup semantics.
 * The caller must not hold scheduler_lock. Capture a bounded diagnostic under
 * that lock; defer resource release until the CPU reservation is quiescent.
 */
void sched_exit(uint32_t pid, uint64_t code);

/**
 * @brief Free a user task's storage, files, and address space once, guarded by a released flag.
 */
void sched_release_task_resources(struct task *task);

void sched_exit_group(uint32_t tgid, uint64_t code);

int sched_prepare_exec_current(struct task *task);

void sched_exec_replace_mm(struct task *task, const struct address_space *replacement);

/** @brief Wake signalfd readers attached to the mask updater's shared sighand. */
void sched_signalfd_reconfigure(struct task *updater);

void sched_wake_process_signal(uint32_t tgid, uint64_t bit);

void sched_notify_parent_exit(struct task *task);

void sched_itimer_task(struct task *task, const struct linux_itimerval *value, struct linux_itimerval *old);

uint32_t sched_alarm_task(struct task *task, uint32_t seconds);

void sched_alarm_rearm(struct task *task);

void sched_alarm_expire(void);

int sched_timer_create(struct task *task, int32_t clockid, int32_t notify,
                       int32_t signo, int32_t target_pid, uint64_t value,
                       int32_t *timerid);

int sched_timer_delete(struct task *task, int32_t timerid);

int sched_timer_settime(struct task *task, int32_t timerid, uint32_t flags,
                        const struct linux_itimerspec *value,
                        struct linux_itimerspec *old);

int sched_timer_gettime(struct task *task, int32_t timerid,
                        struct linux_itimerspec *value);

int sched_timer_getoverrun(struct task *task, int32_t timerid);

void sched_posix_timer_expire(void);

/** @brief Account a tick and queue expirations; signal actions run on return to user mode. */
void sched_on_tick(void);

/**
 * Request local-APIC preemption. The BSP PIT alone advances wall time,
 * sleepers and CPU usage; an AP tick must never manufacture CPU time.
 */
void sched_on_cpu_tick(void);

/**
 * @brief Return the total number of scheduler ticks since init.
 */
uint64_t sched_tick_count(void);

/**
 * @brief Read a consistent snapshot of the Linux Q16 load averages.
 * @param loads Output array of three 1/5/15-minute averages; must not be NULL.
 * @return None. The scheduler lock serializes readers with BSP tick sampling.
 */
void sched_load_averages(uint64_t loads[3]);

void sched_yield_current(void);

/**
 * @brief Request a normal scheduling decision on the current CPU.
 * @note The caller holds the IRQ-disabled syscall execution transaction and
 *       will soon reach the normal user-return scheduling boundary. This
 *       changes only the local reschedule flag; it does not yield the task or
 *       alter scheduler accounting, task state, or remote CPU flags.
 */
void sched_request_reschedule_current(void);

/**
 * @brief Write the accumulated busy and idle tick counters into the caller's outputs.
 */
void sched_cpu_ticks(uint64_t *busy_ticks, uint64_t *idle_ticks);

void sched_cpu_ticks_per_cpu(uint64_t *busy_ticks, uint64_t *idle_ticks,
                             uint32_t capacity);

void sched_cpu_runtime_snapshot(uint64_t *busy_ticks, uint64_t *idle_ticks,
                                uint32_t *current_pids, uint32_t *ready_counts,
                                uint32_t capacity);

uint64_t sched_task_cpu_ticks(const struct task *task, bool process);

/**
 * @brief Count tasks by RUNNING/READY/BLOCKED state and report each plus the total count.
 */
void sched_task_counts(uint32_t *out_task_count, uint32_t *running_tasks,
                       uint32_t *ready_tasks, uint32_t *sleeping_tasks);

/**
 * @brief Return the pid of the currently running task (0 before the first switch).
 */
uint32_t sched_current_pid(void);

int sched_get_task_affinity(uint32_t pid, uint64_t *mask);

int sched_set_task_affinity(uint32_t pid, uint64_t mask);

/**
 * @brief Linear scan for the task with the given pid, or NULL when absent.
 */
struct task *sched_find(uint32_t pid);

/**
 * @brief Return the first task whose name matches, or NULL when none does.
 */
struct task *sched_find_by_name(const char *name);

/**
 * @brief Return the live user task whose path matches exactly, or NULL.
 */
struct task *sched_find_by_path(const char *path);

/**
 * @brief Return the live user task whose path's basename matches, or NULL.
 */
struct task *sched_find_by_path_basename(const char *basename);

/**
 * @brief True when any live task's cwd, path, image node, or open files/VMA reference this volume.
 */
bool sched_volume_has_writers(uint32_t volume_id);

bool sched_volume_in_use(uint32_t volume_id);

/**
 * @brief Return the struct task for the currently running pid, or NULL.
 */
struct task *sched_current_task(void);

/**
 * Save the interrupted Ring-3 state before its CPU releases the task. The
 * frame copy and ownership change share one scheduler-lock transaction so no
 * other CPU can claim a partially published return context.
 */
bool sched_capture_current_user_frame(const struct trap_frame *frame);

/**
 * Save a syscall/timer return frame while preserving the current CPU's fair
 * accounting interval.  This fast path is deliberately narrower than full
 * capture: callers must schedule normally for signals, newly runnable work,
 * a blocked/exited task, an image replacement, or an affinity change.
 */
bool sched_try_resume_current_user_frame(const struct trap_frame *frame);

/**
 * A remote exit may mark a task terminal while it is still executing in
 * Ring-3 on this CPU.  Keep its address space and descriptor table alive
 * until this CPU reaches a scheduling boundary, then drop only this CPU's
 * reservation.  Reapers and slot recycling require running_cpu == NONE.
 */
void sched_quiesce_exited_current(void);

/**
 * @brief Retire the old CR3 and select this CPU's earliest eligible deadline.
 * Caller holds the execution transaction with interrupts disabled and has
 * saved the live frame. scheduler_lock serializes ownership publication.
 * @return Reserved user task, or NULL after retiring into the kernel CR3.
 */
struct task *sched_select_next_user(void);

/**
 * @brief Retry selection without bypassing EEVDF eligibility or CPU ownership.
 */
struct task *sched_reclaim_current_user(void);

/**
 * @brief Return a pointer to the task's saved trap frame, or NULL for a NULL task.
 */
struct trap_frame *sched_task_frame(struct task *task);

/**
 * @brief Return the task's address-space CR3, or 0 for a NULL task.
 */
uint64_t sched_task_cr3(struct task *task);

/**
 * @brief Wake a non-exited task: clear its wait state and set it READY.
 */
void sched_block_current(void);

/**
 * @brief Publish signal-wait sleep before rechecking pending notifications.
 * @param deadline Absolute tick deadline, or zero for no timeout.
 */
void sched_signal_wait_current(uint64_t deadline);

/**
 * @brief Wake a published interruptible wait without resuming stopped/exited tasks.
 * @param task Task pinned by the kernel execution lock; it may still be retiring on another CPU.
 */
void sched_wake_interruptible(struct task *task);

void sched_mark_ready(uint32_t pid);

/**
 * @brief Put the current task to sleep until wake_tick (no-op for the idle or an exited task).
 */
void sched_sleep_current_until(uint64_t wake_tick);

/**
 * @brief Terminate a user task by pid, returning a negative error for idle/services/exited/self.
 */
int sched_kill_user_task(uint32_t pid, uint64_t code);

/**
 * @brief Record a POSIX signal on a user task, handling STOP/CONT state changes and fatal signals.
 */
int sched_signal_user_task(uint32_t pid, int signal_number);

void sched_signal_discard(struct task *task, int signal_number);

void sched_signal_job_control(uint32_t tgid, int signal_number);

int sched_signal_user_process(uint32_t tgid, int signal_number);

/**
 * @brief Queue process-directed siginfo using the addressed PID or TID's thread group.
 * @param pid Addressed task whose credentials and limits govern the send.
 * @param signal_number Native signal number, zero for an existence probe.
 * @param info Explicit signal data, or NULL for a kernel notification.
 * @return Zero on success, or a negative target/queue error.
 */
int sched_signal_user_process_info(uint32_t pid, int signal_number,
                                   const struct linux_siginfo *info);

/**
 * @brief Read or update a task's default/ignored signal disposition.
 * @param pid Target user-task ID.
 * @param signal_number POSIX signal number.
 * @param operation Query or update operation.
 * @param disposition Requested value for update and previous value on return.
 * @return Zero on success or a negative scheduler error.
 */
int sched_signal_action(uint32_t pid, int signal_number, uint32_t operation,
                        uint32_t *disposition);

/**
 * @brief Signal every user task in a group the caller owns; returns the count or a negative error.
 */
int sched_signal_kernel_group(uint32_t process_group, int signal_number);

int64_t sched_process_group_session(uint32_t process_group);

int sched_process_group_orphaned(uint32_t process_group);

int sched_signal_process_group(uint32_t sender_pid, uint32_t process_group,
                               int signal_number);

/**
 * @brief Move a caller-controlled task into an existing or newly created process group.
 */
int sched_set_process_group(uint32_t caller_pid, uint32_t pid,
                            uint32_t process_group);

/**
 * @brief Return the process group of pid (0 = current), or a negative error.
 */
int64_t sched_process_identity(uint32_t pid, bool session);

int64_t sched_get_process_group(uint32_t pid);

/**
 * @brief Make the calling task the leader of a new session and process group.
 */
int64_t sched_create_process_session(uint32_t pid);

/**
 * @brief Count open PTY endpoint descriptors and controlling attachments.
 * @param pty_id PTY identifier.
 * @return Number of live references.
 */
uint32_t sched_pty_reference_count(uint32_t pty_id);

uint32_t sched_pty_master_reference_count(uint32_t pty_id);

/**
 * @brief True when some live task in this process group is attached to the given PTY.
 */
int sched_process_group_has_pty(uint32_t process_group, uint32_t pty_id);

/**
 * @brief Return the process session of pid, or a negative error.
 */
int64_t sched_get_process_session(uint32_t pid);

/**
 * @brief Read or set a user task's nice-style priority, clamping new values to [-20, 19].
 */
int sched_task_priority(uint32_t pid, int priority, int set);

/**
 * @brief Kill every user task (except keep_pid) attached to pty_id and clear its PTY fds.
 */
int sched_kill_user_tasks_for_pty(uint32_t pty_id, uint32_t keep_pid,
                                  uint64_t code);

int sched_hangup_user_tasks_for_pty(uint32_t pty_id, uint32_t keep_pid);

/**
 * @brief Kill every non-service user task in this uid/session except keep_pid.
 */
int sched_kill_user_tasks_for_logout(uint32_t uid, uint32_t session_id,
                                     uint32_t keep_pid, uint64_t code);

/**
 * @brief Observe child events or consume a quiescent zombie, logging a successful reap after unlock.
 * @param waiter_pid Waiting task identifier.
 * @param wanted_pid Child or process-group selector using waitpid semantics.
 * @param options Wait-event selection and WNOWAIT flags.
 * @param status Optional output wait status.
 * @param info Optional output child siginfo.
 * @return Child PID on an observed event, zero without a matching child,
 * -2 for an absent waiter, or -RELIEFOS_EAGAIN while a matching child is pending.
 * The caller must not hold scheduler_lock; resource teardown and diagnostics
 * execute outside it. WNOWAIT observes without consuming or logging a reap.
 */
int64_t sched_wait_reap(uint32_t waiter_pid, int32_t wanted_pid,
                        uint32_t options, int *status, struct linux_siginfo *info);

/**
 * @brief Fill out[] with up to capacity task snapshots, storing the tick count via tick.
 */
uint32_t sched_snapshot(struct task_snapshot_info *out, uint32_t capacity, uint64_t *tick);

/**
 * @brief Set a task's uid/role/session/username/home from user, or clear it when user is empty.
 */
void sched_set_task_identity(uint32_t pid, const struct reliefos_user_info *user,
                             uint32_t session_id);

/**
 * @brief Apply identity to the given task and its non-service children in a session.
 */
void sched_set_session_identity(uint32_t parent_pid, const struct reliefos_user_info *user,
                                uint32_t session_id);

/**
 * @brief Clear identity and reset cwd for every non-service task in this session.
 */
void sched_clear_session_identity(uint32_t session_id);

/**
 * @brief Return the next session id, wrapping past zero back to 1.
 */
uint32_t sched_next_session_id(void);

/**
 * @brief Print one console line per task with pid, identity, name, state, and flags.
 */
void sched_dump(void);

/**
 * @brief Publish a controlling tty change to the process thread group.
 * @param pid Target thread ID.
 * @param pty_id New tty ID, or zero.
 */
void sched_set_controlling_pty(uint32_t pid, uint32_t pty_id);

/**
 * @brief Detach all processes when a privileged session steals a tty.
 * @param pty_id Controlling tty ID to clear.
 */
void sched_clear_controlling_pty(uint32_t pty_id);

#endif
