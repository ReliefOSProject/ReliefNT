/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Trap branches outside the real PACKET recvfrom control. */
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void address_space_user_page_flags(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void address_space_user_page_phys(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void device_user_page_get(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void device_user_page_put(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void kernel_object_insert(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void kernel_object_lookup(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void kernel_object_remove(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void kernel_objects(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void kernel_poll_readiness_notify(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void kernel_wait_queue_add(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void kernel_wait_queue_init(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void kernel_wait_queue_wake_all(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void kernel_wait_queue_wake_one(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void mm_free_page(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void mm_page_refcount(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void mm_phys_to_virt(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void mm_retain_page(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void net_socket_address(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void net_socket_cancel_wait(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void net_socket_connect_fd(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void net_socket_get_timeout_ticks(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void net_socket_open(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void net_socket_pin_fd(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void net_socket_send(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void net_socket_set_timeout_ticks(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void net_socket_shutdown_fd(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void net_socket_wait_writable(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void page_cache_mark_shared_writable(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void page_cache_release(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void page_cache_retain(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void paging_writable_alias_add(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void paging_writable_alias_remove(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void sched_block_current(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void sched_find(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void sched_signal_user_task(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void sched_socket_stage(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void sched_socket_stage_set(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void sched_socket_stage_take(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void syscall_socket_mmsg(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void syscall_udp(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void syscall_unix_address(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_descriptor_for_fd(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_discard_file_fd(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_file_get(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_file_put(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_file_reference(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_pty_export_fd(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_pty_import_fd(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_pty_release_entry(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_udp_send(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void vfs_system_namespace(void) { __builtin_trap(); }
