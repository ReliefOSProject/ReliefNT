/* SPDX-License-Identifier: Apache-2.0 */
/* Unreached host-fixture dependencies trap if invoked. */
/* Unreached non-UNIX routing branches in the socket-dispatch fixture. */
#define TRAP(name) __attribute__((noreturn)) void name(void) { __builtin_trap(); }
TRAP(kernel_object_insert) TRAP(kernel_object_remove) TRAP(kernel_wait_queue_init)
TRAP(kernel_wait_queue_wake_one) TRAP(net_socket_address) TRAP(net_socket_connect_fd)
TRAP(net_socket_get_timeout_ticks) TRAP(net_socket_open) TRAP(net_socket_pin_fd)
TRAP(net_socket_set_timeout_ticks) TRAP(net_socket_shutdown_fd) TRAP(syscall_packet)
TRAP(syscall_udp) TRAP(task_discard_file_fd)
