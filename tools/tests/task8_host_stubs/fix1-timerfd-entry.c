/* SPDX-License-Identifier: Apache-2.0 */
/* Unreached host-fixture dependencies trap if invoked. */
/** @brief Trap if the fixture reaches unmodeled console_log. */
__attribute__((noreturn)) void console_log(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled kernel_signal_rt_sigreturn. */
__attribute__((noreturn)) void kernel_signal_rt_sigreturn(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_clone_current. */
__attribute__((noreturn)) void sched_clone_current(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled syscall_clone_prepared. */
__attribute__((noreturn)) void syscall_clone_prepared(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_exit. */
__attribute__((noreturn)) void sched_exit(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_fork_current. */
__attribute__((noreturn)) void sched_fork_current(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_vfork_current. */
__attribute__((noreturn)) void sched_vfork_current(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled syscall_clone3. */
__attribute__((noreturn)) void syscall_clone3(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled syscall_module_file_prepare. */
__attribute__((noreturn)) void syscall_module_file_prepare(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_device_is. */
__attribute__((noreturn)) void task_device_is(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_fifo_cancel. */
__attribute__((noreturn)) void task_fifo_cancel(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_pty_ensure_fd. */
__attribute__((noreturn)) void task_pty_ensure_fd(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_pty_fd_for_fd. */
__attribute__((noreturn)) void task_pty_fd_for_fd(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_pty_for_io. */
__attribute__((noreturn)) void task_pty_for_io(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_pty_status. */
__attribute__((noreturn)) void task_pty_status(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_pty_stream_for_fd. */
__attribute__((noreturn)) void task_pty_stream_for_fd(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_sysv_msg_cancel. */
__attribute__((noreturn)) void task_sysv_msg_cancel(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_sysv_sem_cancel. */
__attribute__((noreturn)) void task_sysv_sem_cancel(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled userland_exec_cancel. */
__attribute__((noreturn)) void userland_exec_cancel(void) { __builtin_trap(); }

/** @brief Trap mincore reclaim in this unrelated entry fixture. */
__attribute__((noreturn)) void syscall_mm_mincore_reclaim(void) { __builtin_trap(); }
/** @brief Trap open path reclaim in this unrelated entry fixture. */
__attribute__((noreturn)) void syscall_open_path_reclaim(void) { __builtin_trap(); }
/** @brief Trap ordinary read reclaim in this unrelated entry fixture. */
__attribute__((noreturn)) void syscall_regular_read_reclaim(void) { __builtin_trap(); }
