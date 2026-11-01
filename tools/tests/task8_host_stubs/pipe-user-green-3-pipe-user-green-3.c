/* SPDX-License-Identifier: Apache-2.0 */
/* Unreached host-fixture dependencies trap if invoked. */
/** @brief Trap if the fixture reaches unmodeled character_file_read. */
__attribute__((noreturn)) void character_file_read(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled character_file_write. */
__attribute__((noreturn)) void character_file_write(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled console_write_len. */
__attribute__((noreturn)) void console_write_len(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled file_can_read. */
__attribute__((noreturn)) void file_can_read(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled file_can_write. */
__attribute__((noreturn)) void file_can_write(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled kernel_malloc. */
__attribute__((noreturn)) void kernel_malloc(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled kernel_object_insert. */
__attribute__((noreturn)) void kernel_object_insert(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled pty_read_input. */
__attribute__((noreturn)) void pty_read_input(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled pty_write_output. */
__attribute__((noreturn)) void pty_write_output(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled storage_inode_get. */
__attribute__((noreturn)) void storage_inode_get(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled syscall_dispatch_regs_legacy. */
__attribute__((noreturn)) void syscall_dispatch_regs_legacy(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled syscall_regular_io. */
__attribute__((noreturn)) void syscall_regular_io(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled syscall_sysv_msg. */
__attribute__((noreturn)) void syscall_sysv_msg(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled syscall_sysv_sem. */
__attribute__((noreturn)) void syscall_sysv_sem(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled syscall_sysv_shm. */
__attribute__((noreturn)) void syscall_sysv_shm(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_allocate_fd. */
__attribute__((noreturn)) void task_allocate_fd(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_device_is. */
__attribute__((noreturn)) void task_device_is(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_device_read. */
__attribute__((noreturn)) void task_device_read(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_device_write. */
__attribute__((noreturn)) void task_device_write(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_device_write_length. */
__attribute__((noreturn)) void task_device_write_length(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_discard_file_fd. */
__attribute__((noreturn)) void task_discard_file_fd(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_fifo_cancel. */
__attribute__((noreturn)) void task_fifo_cancel(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_file_for_fd. */
__attribute__((noreturn)) void task_file_for_fd(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_inet_read. */
__attribute__((noreturn)) void task_inet_read(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_inet_write. */
__attribute__((noreturn)) void task_inet_write(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_pty_stream_for_fd. */
__attribute__((noreturn)) void task_pty_stream_for_fd(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_shm_read. */
__attribute__((noreturn)) void task_shm_read(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_shm_write. */
__attribute__((noreturn)) void task_shm_write(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_signalfd_read. */
__attribute__((noreturn)) void task_signalfd_read(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_socket_read. */
__attribute__((noreturn)) void task_socket_read(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_socket_write. */
__attribute__((noreturn)) void task_socket_write(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_timerfd_update. */
__attribute__((noreturn)) void task_timerfd_update(void) { __builtin_trap(); }
