/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Trap unrelated entry branches in the wait frame fixture. */
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void kernel_signal_rt_sigreturn(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void syscall_clone3(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void syscall_clone_prepared(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void syscall_module_file_prepare(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_device_is(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_fifo_cancel(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_file_for_fd(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_file_get(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_pty_ensure_fd(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_pty_fd_for_fd(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_pty_for_io(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_pty_status(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_pty_stream_for_fd(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_sysv_msg_cancel(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_sysv_sem_cancel(void) { __builtin_trap(); }

/** @brief Trap mincore reclaim in this unrelated entry fixture. */
__attribute__((noreturn)) void syscall_mm_mincore_reclaim(void) { __builtin_trap(); }
/** @brief Trap open path reclaim in this unrelated entry fixture. */
__attribute__((noreturn)) void syscall_open_path_reclaim(void) { __builtin_trap(); }
/** @brief Trap ordinary read reclaim in this unrelated entry fixture. */
__attribute__((noreturn)) void syscall_regular_read_reclaim(void) { __builtin_trap(); }

/* Wait4 never enters the pipe reader fast path. */
__attribute__((noreturn)) void kernel_execution_try_read_lock_irqsave(void) { __builtin_trap(); }
__attribute__((noreturn)) void kernel_execution_read_unlock_irqrestore(void) { __builtin_trap(); }
__attribute__((noreturn)) void address_space_user_page_copyable(void) { __builtin_trap(); }
__attribute__((noreturn)) void task_pipe_try_io(void) { __builtin_trap(); }
