/* SPDX-License-Identifier: Apache-2.0 */
/* Unreached host-fixture dependencies trap if invoked. */
/** @brief Trap if the fixture reaches unmodeled copy_user_path. */
__attribute__((noreturn)) void copy_user_path(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_task_vma_at. */
__attribute__((noreturn)) void sched_task_vma_at(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled sched_task_vma_capacity. */
__attribute__((noreturn)) void sched_task_vma_capacity(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled storage_inode_deny_write. */
__attribute__((noreturn)) void storage_inode_deny_write(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled storage_inode_get. */
__attribute__((noreturn)) void storage_inode_get(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_process_vm_scope_release. */
__attribute__((noreturn)) void task_process_vm_scope_release(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled task_unmap_pages. */
__attribute__((noreturn)) void task_unmap_pages(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled user_range_ok. */
__attribute__((noreturn)) void user_range_ok(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled vfs_path_get. */
__attribute__((noreturn)) void vfs_path_get(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled vfs_path_node. */
__attribute__((noreturn)) void vfs_path_node(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled vfs_path_same_inode. */
__attribute__((noreturn)) void vfs_path_same_inode(void) { __builtin_trap(); }
