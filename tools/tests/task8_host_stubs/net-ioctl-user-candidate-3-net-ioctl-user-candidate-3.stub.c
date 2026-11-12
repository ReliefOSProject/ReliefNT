/* SPDX-License-Identifier: Apache-2.0 */
/* Unreached host-fixture dependencies trap if invoked. */
/** @brief Trap if the fixture reaches unmodeled console_log. */
__attribute__((noreturn)) void console_log(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled copy_user_path. */
__attribute__((noreturn)) void copy_user_path(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled e1000_init. */
__attribute__((noreturn)) void e1000_init(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled e1000_poll. */
__attribute__((noreturn)) void e1000_poll(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled e1000_send. */
__attribute__((noreturn)) void e1000_send(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled kernel_poll_readiness_notify. */
__attribute__((noreturn)) void kernel_poll_readiness_notify(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled kernel_wait_queue_add. */
__attribute__((noreturn)) void kernel_wait_queue_add(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled kernel_wait_queue_init. */
__attribute__((noreturn)) void kernel_wait_queue_init(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled kernel_wait_queue_remove. */
__attribute__((noreturn)) void kernel_wait_queue_remove(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled kernel_wait_queue_wake_all. */
__attribute__((noreturn)) void kernel_wait_queue_wake_all(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled net_packet_input. */
__attribute__((noreturn)) void net_packet_input(void) { __builtin_trap(); }
/** @brief Trap if the ioctl-only fixture attempts hardware transmission. */
__attribute__((noreturn)) void netdev_send(void) { __builtin_trap(); }
/** @brief Trap if the ioctl-only fixture attempts hardware receive polling. */
__attribute__((noreturn)) void netdev_poll(void) { __builtin_trap(); }
/** @brief Trap if the ioctl-only fixture delivers a received packet frame. */
__attribute__((noreturn)) void net_packet_input_on(void) { __builtin_trap(); }
/** @brief Trap if the ioctl-only fixture delivers an IPv6 frame. */
__attribute__((noreturn)) void net_ipv6_input_on(void) { __builtin_trap(); }
/** @brief Trap if the ioctl-only fixture runs IPv6 discovery maintenance. */
__attribute__((noreturn)) void net_ipv6_poll(void) { __builtin_trap(); }
/** @brief Trap if the ioctl-only fixture selects an IPv6 transport route. */
__attribute__((noreturn)) void net_ipv6_route(void) { __builtin_trap(); }
/** @brief Trap if the ioctl-only fixture computes an IPv6 transport checksum. */
__attribute__((noreturn)) void net_ipv6_checksum(void) { __builtin_trap(); }
/** @brief Trap if the ioctl-only fixture transmits an IPv6 packet. */
__attribute__((noreturn)) void net_ipv6_output(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled net_udp_input. */
__attribute__((noreturn)) void net_udp_input(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_sleep_current_until. */
__attribute__((noreturn)) void sched_sleep_current_until(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled storage_inode_deny_write. */
__attribute__((noreturn)) void storage_inode_deny_write(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled storage_inode_get. */
__attribute__((noreturn)) void storage_inode_get(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_packet_available. */
__attribute__((noreturn)) void task_packet_available(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_udp_available. */
__attribute__((noreturn)) void task_udp_available(void) { __builtin_trap(); }
/** @brief Trap if the ioctl-only fixture queries netlink receive bytes. */
__attribute__((noreturn)) void task_netlink_available(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled time_set_wall_clock. */
__attribute__((noreturn)) void time_set_wall_clock(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled time_ticks. */
__attribute__((noreturn)) void time_ticks(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled time_uptime_ms. */
__attribute__((noreturn)) void time_uptime_ms(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled vfs_kernel_read. */
__attribute__((noreturn)) void vfs_kernel_read(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled vfs_path_get. */
__attribute__((noreturn)) void vfs_path_get(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled vfs_path_node. */
__attribute__((noreturn)) void vfs_path_node(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled vfs_path_same_inode. */
__attribute__((noreturn)) void vfs_path_same_inode(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled vfs_system_namespace. */
__attribute__((noreturn)) void vfs_system_namespace(void) { __builtin_trap(); }
