/*
 * ReliefOS scheduler interface: defines task, VMA, file, and session state.
 * Declares task lifecycle, scheduling, waiting, and process-inspection APIs.
 */
#ifndef RELIEFNT_SCHED_H
#define RELIEFNT_SCHED_H

#include <reliefnt/task/types.h>
#include <reliefnt/task/accessors.h>

/**
 * @brief Initialize the scheduler, its run queue, and the tick bookkeeping.
 */
void sched_init(void);
/**
 * @brief Create a kernel task named name that starts at entry; returns its pid.
 */
uint32_t sched_create_kernel_task(const char *name, uint64_t entry);
/**
 * @brief Create a user task named name with the given entry, stack, parent, and flags; returns pid.
 */
uint32_t sched_create_user_task(const char *name, uint64_t entry, uint64_t stack_top,
                                uint32_t parent_pid, uint32_t flags);
/**
 * @brief Wake (or release) task's CLONE_VFORK child as exec/exit completes.
 *
 * Called on the child lifecycle paths after no further failure can occur.  The
 * caller must not hold scheduler_lock.
 */
void sched_vfork_child_done(struct task *child);
/**
 * @brief Duplicates the current user task using copy-on-write user mappings.
 * @param parent_frame Saved fork syscall frame; the child receives a copy with rax set to zero.
 * @return Positive child PID to the parent or a negative errno-style failure.
 */
int64_t sched_fork_current(const struct trap_frame *parent_frame);
/**
 * @brief Attach the executable image (image/image_len) to task pid for a later exec.
 */
void sched_set_task_image(uint32_t pid, const void *image, size_t image_len);
/**
 * @brief Attach the executable image described by node to task pid.
 */
void sched_set_task_image_node(uint32_t pid, const struct storage_node *node);
/**
 * @brief Record the executable path that task pid will run.
 */
void sched_set_task_path(uint32_t pid, const char *path);
/**
 * @brief Replace exec metadata with an owned copy, preserving old data on failure.
 * @param task Destination task. @param argc Number of arguments.
 * @param argv Argument pointers into data. @param envc Number of environment entries.
 * @param envp Environment pointers into data. @param data Packed string buffer.
 * @param data_len Buffer length in bytes.
 * @return Zero, -EINVAL for invalid pointers, -E2BIG, or -ENOMEM.
 */
int sched_copy_task_exec_params(struct task *task, uint32_t argc, char *const argv[],
                                uint32_t envc, char *const envp[],
                                const char *data, uint32_t data_len);
/**
 * @brief Assign owned exec metadata to a task identified by pid.
 * @param pid Task identifier. @param argc Number of arguments.
 * @param argv Argument pointers into data. @param envc Number of environment entries.
 * @param envp Environment pointers into data. @param data Packed string buffer.
 * @param data_len Buffer length in bytes.
 * @return Zero or the negative copy/allocation error.
 */
int sched_set_task_exec_params(uint32_t pid,
                                uint32_t argc, char *const argv[],
                                uint32_t envc, char *const envp[],
                                const char *data, uint32_t data_len);
struct task_vma *sched_task_vma_at(struct task *task, uint32_t index);
uint32_t sched_task_vma_capacity(const struct task *task);
void sched_task_vma_release(struct task *task);
/* Called with the kernel execution lock held, before file pages are released. */
void sched_truncate_file_mappings(const struct storage_node *node, uint64_t size);
struct task_file *sched_task_file_at(struct task *task, uint32_t index);
/** @brief Resolve a Linux fd, including stdio and descriptors beyond the inline table.
 * @param task Owner under execution serialization. @param fd Nonnegative fd.
 * @return Borrowed live descriptor slot, or NULL; does not promote or retain it.
 */
struct task_file *task_descriptor_for_fd(struct task *task, int fd);
uint32_t sched_task_file_capacity(const struct task *task);
void sched_task_file_release(struct task *task);
/**
 * @brief Create the always-runnable idle task that runs when nothing else can.
 */
void sched_create_idle_task(void);
/**
 * @brief Mark task pid as the currently running task.
 */
void sched_set_running(uint32_t pid);
/**
 * @brief Publish terminal task state and log its first exit after releasing the scheduler lock.
 * @param pid Task identifier; PID 1 panics and an absent PID has no exit event.
 * @param code Exit status; repeated calls retain the existing status-update and cleanup semantics.
 * The caller must not hold scheduler_lock. Capture a bounded diagnostic under
 * that lock; defer resource release until the CPU reservation is quiescent.
 */
void sched_exit(uint32_t pid, uint64_t code);
/**
 * @brief Free the memory, descriptors, and mappings still held by task.
 */
void sched_release_task_resources(struct task *task);
/**
 * @brief Advance scheduler time and preempt or wake tasks as needed.
 */
void sched_on_tick(void);
/** Request local preemption without advancing wall time or CPU usage. */
void sched_on_cpu_tick(void);
/**
 * @brief Return the number of scheduler ticks since boot.
 */
uint64_t sched_tick_count(void);
void sched_yield_current(void);
/**
 * @brief Request a normal scheduling decision at the next current-CPU user-return boundary.
 * @note Caller holds the IRQ-disabled syscall execution transaction and will
 *       soon reach that boundary. This publishes only the current CPU's
 *       reschedule flag; it does not change task state, queues, EEVDF
 *       accounting or deadlines, or any remote CPU's flag.
 */
void sched_request_reschedule_current(void);
/**
 * @brief Report elapsed busy and idle CPU time in Linux USER_HZ (100) units.
 */
void sched_cpu_ticks(uint64_t *busy_ticks, uint64_t *idle_ticks);
/** Copy elapsed busy and idle time for each CPU, in the same 100 Hz units. */
void sched_cpu_ticks_per_cpu(uint64_t *busy_ticks, uint64_t *idle_ticks,
                             uint32_t capacity);
/** Copy a coherent per-CPU runtime snapshot, including current pid and
 * eligible ready-queue counts. */
void sched_cpu_runtime_snapshot(uint64_t *busy_ticks, uint64_t *idle_ticks,
                                uint32_t *current_pids, uint32_t *ready_counts,
                                uint32_t capacity);
/** Return a coherent task CPU-time snapshot in USER_HZ units. With process
 * true, sum the existing thread-group members. Caller pins
 * task lifetime with the execution transaction. Reaped children are separate. */
uint64_t sched_task_cpu_ticks(const struct task *task, bool process);
/**
 * @brief Read Linux sysinfo-compatible 1/5/15-minute load averages.
 * @param loads Output array of three Q16 values; must not be NULL.
 * @return None. Values are sampled every five seconds under the scheduler lock.
 */
void sched_load_averages(uint64_t loads[3]);

/**
 * @brief Report the current task totals: all, running, ready, and sleeping.
 */
void sched_task_counts(uint32_t *task_count, uint32_t *running_tasks,
                       uint32_t *ready_tasks, uint32_t *sleeping_tasks);
/**
 * @brief Return the pid of the task currently running on this CPU.
 */
uint32_t sched_current_pid(void);
uint64_t sched_all_cpu_mask(void);
int sched_get_task_affinity(uint32_t pid, uint64_t *mask);
int sched_set_task_affinity(uint32_t pid, uint64_t mask);
struct task *sched_current_task(void);
struct task *sched_find(uint32_t pid);

static inline uint64_t *sched_task_timer_pending(const struct task *task)
{
    struct task *leader = task ? sched_find(sched_task_tgid(task)) : NULL;
    return leader ? &leader->timer_pending_signals : NULL;
}

struct task *sched_find_by_name(const char *name);
struct task *sched_find_by_path(const char *path);
struct task *sched_find_by_path_basename(const char *basename);
/** Returns true when a CWD, file, image, or mapping references a volume. */
bool sched_volume_in_use(uint32_t volume_id);
bool sched_volume_has_writers(uint32_t volume_id);
/**
 * @brief Save the current user frame without releasing its CPU reservation.
 * @param frame User context to publish while holding the execution transaction.
 * @return True on capture; false if the frame or CPU ownership is invalid.
 */
bool sched_capture_current_user_frame(const struct trap_frame *frame);
/**
 * @brief Save a valid frame and continue the current task without ending its fair-accounting interval.
 * @param frame Live user context to publish while holding the execution transaction.
 * @return True only when the task is still RUNNING on this CPU, has no pending
 *         reschedule or deliverable signal, and remains eligible on this CPU.
 */
bool sched_try_resume_current_user_frame(const struct trap_frame *frame);
/** Release this CPU's ownership after its current task was marked EXITED. */
void sched_quiesce_exited_current(void);
/**
 * @brief Retire the old CR3 and reserve the next eligible task on this CPU.
 * Caller holds the execution transaction, has saved the live user frame,
 * and has interrupts disabled. Even an idle return retires the old task.
 * @return CPU-reserved task, or NULL with the kernel CR3 active.
 */
struct task *sched_select_next_user(void);
/* Query the local CPU's tick/wakeup reschedule request before taking the
 * returning-task fast path. Other CPUs clear only their own request. */
bool sched_current_cpu_needs_resched(void);
/* Notify CPUs after publishing a runnable task. The caller must already
 * serialize the task-state transition; this only marks pending bits and does
 * not acquire scheduler_lock, so it is safe while that lock is held. */
void sched_notify_runnable(void);
/* Reclaim the task just saved by this CPU when no other task is runnable. */
struct task *sched_reclaim_current_user(void);
struct trap_frame *sched_task_frame(struct task *task);
/**
 * @brief Return the page-table root (CR3 value) of task.
 */
uint64_t sched_task_cr3(struct task *task);
/**
 * @brief Move task pid back onto the ready queue so it can be scheduled.
 */
void sched_mark_ready(uint32_t pid);
/**
 * @brief Put the current task to sleep with no wake deadline (wait queues own
 * the wakeup path). Used by IPC primitives before returning EAGAIN internally;
 * the syscall epilogue rewinds and retries the instruction after wakeup.
 */
void sched_block_current(void);
/**
 * @brief Put the current task to sleep until the given tick.
 */
void sched_sleep_current_until(uint64_t wake_tick);
/**
 * @brief Sleep the current task until a window event or the given tick.
 */
/**
 * @brief Wake task pid if it is waiting on window_id.
 */
/**
 * @brief Terminate user task pid with code; 0 on success.
 */
int sched_kill_user_task(uint32_t pid, uint64_t code);
/**
 * @brief Sends a supported signal to a user task.
 * @param pid Target process identifier.
 * @param signal_number POSIX signal number.
 * @return Zero on success or a negative scheduler error.
 */
int sched_signal_user_task(uint32_t pid, int signal_number);
int sched_signal_user_process(uint32_t tgid, int signal_number);
/** @brief Send to a thread group's shared queue, preserving the supplied siginfo. */
int sched_signal_user_process_info(uint32_t tgid, int sig, const struct linux_siginfo *info);
/** @brief Park a sigtimedwait caller, rechecking pending signals after publishing sleep. */
void sched_signal_wait_current(uint64_t deadline);
/** @brief Wake a blocked task pinned by the execution lock, retaining current CPU ownership. */
void sched_wake_interruptible(struct task *task);
/** @brief Share SEM_UNDO on CLONE_SYSVSEM; ordinary fork starts with no undo records. */
int task_sysv_sem_clone(struct task *parent, struct task *child, uint64_t flags);
/** @brief Apply the final owner's undo adjustments on exit and release its records. */
void task_sysv_sem_exit(struct task *task);
/** @brief Finish interruptible semaphore waits before a group stop. */
void task_sysv_sem_stop(struct task *task);
void sched_signal_job_control(uint32_t tgid, int signal_number);
void sched_signal_discard(struct task *task, int signal_number);
uint32_t sched_alarm_task(struct task *task, uint32_t seconds);
struct linux_itimerval;
void sched_itimer_task(struct task *task, const struct linux_itimerval *value, struct linux_itimerval *old);
void sched_alarm_rearm(struct task *task);
struct linux_itimerspec;
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
 * @brief Sends a signal to all eligible user tasks in a process group.
 * @param sender_pid Process that requested the signal.
 * @param process_group Target process group identifier.
 * @param signal_number POSIX signal number.
 * @return Number of signalled tasks, or a negative scheduler error.
 */
int sched_signal_process_group(uint32_t sender_pid, uint32_t process_group,
                               int signal_number);
/* Terminal-generated signals originate in the kernel, independent of UID. */
int sched_signal_kernel_group(uint32_t process_group, int signal_number);
int64_t sched_process_group_session(uint32_t process_group);
int sched_process_group_orphaned(uint32_t process_group);
/**
 * @brief Updates the process group of the caller or one of its direct children.
 * @param caller_pid Process issuing setpgid.
 * @param pid Target process, or zero for the caller.
 * @param process_group Target group, or zero to create a group led by the target.
 * @return Zero on success or a negative errno-style failure.
 */
int sched_set_process_group(uint32_t caller_pid, uint32_t pid,
                            uint32_t process_group);
/**
 * @brief Returns the process group for a task.
 * @param pid Target process, or zero for the current process.
 * @return Positive process-group identifier or a negative scheduler error.
 */
int64_t sched_get_process_group(uint32_t pid);
/**
 * @brief Creates a new POSIX-style session for the calling process.
 * @param pid Calling process identifier.
 * @return New session identifier or a negative scheduler error.
 */
int64_t sched_create_process_session(uint32_t pid);
/**
 * @brief Checks whether a process group has a task attached to a PTY.
 * @param process_group Process group identifier.
 * @param pty_id PTY identifier.
 * @return Non-zero when an attached group member exists.
 */
int sched_process_group_has_pty(uint32_t process_group, uint32_t pty_id);
/**
 * @brief Count open PTY endpoint descriptors and legacy controlling-TTY
 * attachments referencing pty_id.
 */
uint32_t sched_pty_reference_count(uint32_t pty_id);
uint32_t sched_pty_master_reference_count(uint32_t pty_id);
/**
 * @brief Returns the POSIX process session containing a task.
 * @param pid Task process identifier.
 * @return Positive session identifier or a negative scheduler error.
 */
int64_t sched_get_process_session(uint32_t pid);
/**
 * @brief Reads or updates a task's nice-style priority.
 * @param pid Target process identifier.
 * @param priority New priority when set is non-zero.
 * @param set Non-zero to update, zero to read.
 * @return Priority on read/update or a negative error.
 */
int sched_task_priority(uint32_t pid, int priority, int set);
/**
 * @brief Kill every task attached to pty_id except keep_pid; returns the count.
 */
int sched_kill_user_tasks_for_pty(uint32_t pty_id, uint32_t keep_pid,
                                  uint64_t code);
/**
 * @brief Send SIGHUP to tasks attached to a closed PTY and detach survivors.
 * @param pty_id Closed PTY identifier.
 * @param keep_pid PTY owner to leave untouched.
 * @return Number of signalled tasks, or zero when no PTY was supplied.
 */
int sched_hangup_user_tasks_for_pty(uint32_t pty_id, uint32_t keep_pid);
/**
 * @brief Kill tasks of uid/session_id except keep_pid; returns the count.
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
 * @brief Copy up to capacity task summaries into out and report the current tick.
 */
uint32_t sched_snapshot(struct task_snapshot_info *out, uint32_t capacity, uint64_t *tick);
/**
 * @brief Attach user identity and session to task pid.
 */
void sched_set_task_identity(uint32_t pid, const struct reliefos_user_info *user,
                             uint32_t session_id);
/**
 * @brief Attach user identity and session to parent_pid's existing session.
 */
void sched_set_session_identity(uint32_t parent_pid, const struct reliefos_user_info *user,
                                uint32_t session_id);
/**
 * @brief Detach the identity associated with session_id.
 */
void sched_clear_session_identity(uint32_t session_id);
/**
 * @brief Allocate and return a fresh session identifier.
 */
uint32_t sched_next_session_id(void);
/**
 * @brief Print scheduler and task state to the console for debugging.
 */
void sched_dump(void);


/**
 * @brief Set the controlling tty of every thread in a process.
 * @param pid Any thread ID in the process.
 * @param pty_id New controlling tty ID, or zero to detach.
 */
void sched_set_controlling_pty(uint32_t pid, uint32_t pty_id);
/**
 * @brief Clear a stolen controlling tty from all its former processes.
 * @param pty_id Controlling tty to detach.
 */
void sched_clear_controlling_pty(uint32_t pty_id);

#endif
