/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Trap unrelated branches of the real legacy syscall dispatcher. */
#define UNREACHED(name) __attribute__((noreturn)) void name(void) { __builtin_trap(); }
UNREACHED(copy_exec_params_from_user)
UNREACHED(copy_text)
UNREACHED(exec_resolve_scripts)
UNREACHED(free_exec_params)
UNREACHED(path_append_char)
UNREACHED(path_append_text)
UNREACHED(path_append_u64)
UNREACHED(resolve_kernel_object_at_flags)
#ifndef TASK8_WAIT_FRAME_TEST
UNREACHED(storage_release_task_io)
#endif
UNREACHED(syscall_linux_signal)
UNREACHED(syscall_process_control)
UNREACHED(syscall_process_prctl)
UNREACHED(syscall_process_vm)
UNREACHED(task_fd_descriptor_flags)
UNREACHED(task_lookup_user_text)
UNREACHED(vfs_system_namespace)
UNREACHED(__wrap_userland_exec_current_path)
UNREACHED(__wrap_userland_process_exit)
UNREACHED(__wrap_sched_exit_group)
UNREACHED(__wrap_sched_fork_current)
UNREACHED(__wrap_sched_vfork_current)
UNREACHED(__wrap_sched_yield_current)
UNREACHED(__wrap_sched_block_current)
