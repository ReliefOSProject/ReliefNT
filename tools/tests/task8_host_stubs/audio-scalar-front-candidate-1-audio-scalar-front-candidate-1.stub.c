/* SPDX-License-Identifier: Apache-2.0 */
/* Unreached host-fixture dependencies trap if invoked. */
/** @brief Trap if the fixture reaches unmodeled console_write_len. */
__attribute__((noreturn)) void console_write_len(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled pty_check_change. */
__attribute__((noreturn)) void pty_check_change(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled pty_input_available. */
__attribute__((noreturn)) void pty_input_available(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled pty_is_hungup. */
__attribute__((noreturn)) void pty_is_hungup(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled pty_output_available. */
__attribute__((noreturn)) void pty_output_available(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled pty_read_input. */
__attribute__((noreturn)) void pty_read_input(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled pty_read_output. */
__attribute__((noreturn)) void pty_read_output(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled pty_write_input. */
__attribute__((noreturn)) void pty_write_input(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled pty_write_output. */
__attribute__((noreturn)) void pty_write_output(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_signal_user_task. */
__attribute__((noreturn)) void sched_signal_user_task(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled syscall_regular_io. */
__attribute__((noreturn)) void syscall_regular_io(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_device_read. */
__attribute__((noreturn)) void task_device_read(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_device_write. */
__attribute__((noreturn)) void task_device_write(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_inet_read. */
__attribute__((noreturn)) void task_inet_read(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_inet_write. */
__attribute__((noreturn)) void task_inet_write(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_pty_status. */
__attribute__((noreturn)) void task_pty_status(void) { __builtin_trap(); }
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
/** @brief Trap if the fixture reaches unmodeled task_socket_receive_user. */
__attribute__((noreturn)) void task_socket_receive_user(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_socket_send_user. */
__attribute__((noreturn)) void task_socket_send_user(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_socket_write. */
__attribute__((noreturn)) void task_socket_write(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_timerfd_update. */
__attribute__((noreturn)) void task_timerfd_update(void) { __builtin_trap(); }
