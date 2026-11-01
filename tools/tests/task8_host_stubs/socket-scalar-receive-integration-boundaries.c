/* SPDX-License-Identifier: Apache-2.0 */
/* These unrelated host paths must remain unreachable in this fixture. */
/** @brief Fail if the unmodeled net_socket_cancel_wait path is invoked. */
__attribute__((noreturn)) void net_socket_cancel_wait(void) { __builtin_trap(); }
/** @brief Fail if the unmodeled task_packet_address_size path is invoked. */
__attribute__((noreturn)) void task_packet_address_size(void) { __builtin_trap(); }
/** @brief Fail if the unmodeled task_packet_recv_staged path is invoked. */
__attribute__((noreturn)) void task_packet_recv_staged(void) { __builtin_trap(); }
/** @brief Fail if the unmodeled task_udp_recv_staged path is invoked. */
__attribute__((noreturn)) void task_udp_recv_staged(void) { __builtin_trap(); }
/** @brief Fail if the unmodeled task_process_vm_scope_release path is invoked. */
__attribute__((noreturn)) void task_process_vm_scope_release(void) { __builtin_trap(); }
/** @brief Fail if the unmodeled task_shm_write_admit path is invoked. */
__attribute__((noreturn)) void task_shm_write_admit(void) { __builtin_trap(); }
