/* SPDX-License-Identifier: Apache-2.0 */
/* Unreached host-fixture dependencies trap if invoked. */
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
