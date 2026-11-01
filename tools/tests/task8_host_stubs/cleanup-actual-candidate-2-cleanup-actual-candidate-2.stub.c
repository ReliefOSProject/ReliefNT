/* SPDX-License-Identifier: Apache-2.0 */
/* Unreached host-fixture dependencies trap if invoked. */
/* Unreached non-UNIX routing branches in the socket-dispatch fixture. */
#define TRAP(name) __attribute__((noreturn)) void name(void) { __builtin_trap(); }
TRAP(kernel_object_insert) TRAP(kernel_object_remove) TRAP(kernel_wait_queue_init)
TRAP(kernel_wait_queue_wake_one) TRAP(net_socket_address) TRAP(net_socket_connect_fd)
TRAP(net_socket_get_timeout_ticks) TRAP(net_socket_open) TRAP(net_socket_pin_fd)
TRAP(net_socket_set_timeout_ticks) TRAP(net_socket_shutdown_fd) TRAP(syscall_packet)
TRAP(syscall_udp)
TRAP(task_process_vm_scope_release)

/** @brief Trap if the fixture reaches unmodeled task_pipe_release. */
__attribute__((noreturn)) void task_pipe_release(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_packet_release. */
__attribute__((noreturn)) void task_packet_release(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_udp_release. */
__attribute__((noreturn)) void task_udp_release(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled net_socket_release_fd. */
__attribute__((noreturn)) void net_socket_release_fd(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled syscall_flock_release_owner. */
__attribute__((noreturn)) void syscall_flock_release_owner(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_epoll_remove_file_description. */
__attribute__((noreturn)) void task_epoll_remove_file_description(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled character_file_close. */
__attribute__((noreturn)) void character_file_close(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled pty_transfer_put. */
__attribute__((noreturn)) void pty_transfer_put(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled block_device_put. */
__attribute__((noreturn)) void block_device_put(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled storage_inode_write_put. */
__attribute__((noreturn)) void storage_inode_write_put(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_shm_release. */
__attribute__((noreturn)) void task_shm_release(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_epoll_destroy. */
__attribute__((noreturn)) void task_epoll_destroy(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled input_evdev_release. */
__attribute__((noreturn)) void input_evdev_release(void) { __builtin_trap(); }
