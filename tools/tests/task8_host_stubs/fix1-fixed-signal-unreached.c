/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Trap unrelated descriptor, scheduler and provider branches. */
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void block_device_put(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void character_file_close(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void input_evdev_release(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void pty_transfer_put(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void sched_block_current(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void storage_inode_write_put(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void storage_release_task_io(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void syscall_flock_release_owner(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_descriptor_for_fd(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_epoll_destroy(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_epoll_remove_file_description(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_inet_release(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_pipe_release(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_shm_release(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_socket_collect(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void task_socket_release(void) { __builtin_trap(); }
/** @brief Trap an unrelated branch. */
__attribute__((noreturn)) void vfs_file_close(void) { __builtin_trap(); }
