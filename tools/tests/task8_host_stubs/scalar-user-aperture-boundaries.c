/* SPDX-License-Identifier: Apache-2.0 */
/* These unrelated host paths must remain unreachable in this fixture. */
/** @brief Fail if the unmodeled task_shm_write_admit path is invoked. */
__attribute__((noreturn)) void task_shm_write_admit(void) { __builtin_trap(); }
/** @brief Fail if the unmodeled task_socket_receive_user path is invoked. */
__attribute__((noreturn)) void task_socket_receive_user(void) { __builtin_trap(); }
