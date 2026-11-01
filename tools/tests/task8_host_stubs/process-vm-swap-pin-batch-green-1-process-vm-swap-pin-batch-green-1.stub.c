/* SPDX-License-Identifier: Apache-2.0 */
/* Unreached host-fixture dependencies trap if invoked. */
/** @brief Trap if the fixture reaches unmodeled copy_user_path. */
__attribute__((noreturn)) void copy_user_path(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled framebuffer_get. */
__attribute__((noreturn)) void framebuffer_get(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled kernel_spin_lock. */
__attribute__((noreturn)) void kernel_spin_lock(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled kernel_spin_unlock. */
__attribute__((noreturn)) void kernel_spin_unlock(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled mm_phys_is_ram. */
__attribute__((noreturn)) void mm_phys_is_ram(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled paging_trusted_table_alias. */
__attribute__((noreturn)) void paging_trusted_table_alias(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled smp_current_cpu. */
__attribute__((noreturn)) void smp_current_cpu(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled storage_inode_deny_write. */
__attribute__((noreturn)) void storage_inode_deny_write(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled storage_inode_get. */
__attribute__((noreturn)) void storage_inode_get(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled vfs_path_get. */
__attribute__((noreturn)) void vfs_path_get(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled vfs_path_node. */
__attribute__((noreturn)) void vfs_path_node(void) { __builtin_trap(); }
/** @brief Trap if the fixture reaches unmodeled vfs_path_same_inode. */
__attribute__((noreturn)) void vfs_path_same_inode(void) { __builtin_trap(); }
