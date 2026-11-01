/* SPDX-License-Identifier: Apache-2.0 */
/* Unreached host-fixture dependencies trap if invoked. */
/** @brief Trap if the fixture reaches unmodeled kernel_wait_queue_remove. */
__attribute__((noreturn)) void kernel_wait_queue_remove(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_inet_write. */
__attribute__((noreturn)) void task_inet_write(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_pipe_write. */
__attribute__((noreturn)) void task_pipe_write(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_release_syscall_file. */
__attribute__((noreturn)) void task_release_syscall_file(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_socket_write. */
__attribute__((noreturn)) void task_socket_write(void) { __builtin_trap(); }
