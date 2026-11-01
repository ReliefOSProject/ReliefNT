/* SPDX-License-Identifier: Apache-2.0 */
/* Unreached host-fixture dependencies trap if invoked. */
/** @brief Trap if the fixture reaches unmodeled arch_set_user_fs. */
__attribute__((noreturn)) void arch_set_user_fs(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled console_log. */
__attribute__((noreturn)) void console_log(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled kernel_signal_queue_task_info. */
__attribute__((noreturn)) void kernel_signal_queue_task_info(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled kernel_signal_queueinfo. */
__attribute__((noreturn)) void kernel_signal_queueinfo(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled kernel_signal_set_action. */
__attribute__((noreturn)) void kernel_signal_set_action(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled linux_uts_names. */
__attribute__((noreturn)) void linux_uts_names(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled linux_uts_set. */
__attribute__((noreturn)) void linux_uts_set(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled power_reboot. */
__attribute__((noreturn)) void power_reboot(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled power_shutdown. */
__attribute__((noreturn)) void power_shutdown(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled reliefnt_system_info. */
__attribute__((noreturn)) void reliefnt_system_info(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_block_current. */
__attribute__((noreturn)) void sched_block_current(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_create_process_session. */
__attribute__((noreturn)) void sched_create_process_session(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_get_process_group. */
__attribute__((noreturn)) void sched_get_process_group(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_get_process_session. */
__attribute__((noreturn)) void sched_get_process_session(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_get_task_affinity. */
__attribute__((noreturn)) void sched_get_task_affinity(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_set_process_group. */
__attribute__((noreturn)) void sched_set_process_group(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_set_task_affinity. */
__attribute__((noreturn)) void sched_set_task_affinity(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_signal_process_group. */
__attribute__((noreturn)) void sched_signal_process_group(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_signal_user_process_info. */
__attribute__((noreturn)) void sched_signal_user_process_info(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_task_priority. */
__attribute__((noreturn)) void sched_task_priority(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_user_task_count. */
__attribute__((noreturn)) void sched_user_task_count(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled smp_membarrier. */
__attribute__((noreturn)) void smp_membarrier(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled time_set_wall_clock_ns. */
__attribute__((noreturn)) void time_set_wall_clock_ns(void) { __builtin_trap(); }
