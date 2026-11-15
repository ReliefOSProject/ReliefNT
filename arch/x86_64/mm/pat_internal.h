/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_X86_PAT_INTERNAL_H
#define RELIEFNT_X86_PAT_INTERNAL_H
#include <reliefnt/types.h>
/** @brief Read local CPUID leaf-one EDX cache capabilities. @return Feature bits. */
uint32_t x86_cache_native_features(void);
/** @brief Read a capability-validated cache MSR. @param msr Register number.
 * @return Its complete value. Caller masks IRQs during policy changes.
 */
uint64_t x86_cache_native_read(uint32_t msr);
/** @brief Write a capability-validated cache MSR with caches disabled.
 * @param msr Register number. @param value Valid register contents.
 */
void x86_cache_native_write(uint32_t msr, uint64_t value);
/** @brief Disable local caching and drain caches/global translations.
 * @return Saved CR0 for x86_cache_native_end. Boot only, IRQs already masked.
 */
uint64_t x86_cache_native_begin(void);
/** @brief Flush changed cache policies and restore local caching.
 * @param cr0 CR0 returned by x86_cache_native_begin. IRQs remain masked.
 */
void x86_cache_native_end(uint64_t cr0);
/** @brief Snapshot the BSP's firmware MTRRs before starting APs.
 * BSP only, IRQs masked; no firmware register is modified.
 */
void x86_mtrr_capture_boot(void);
/** @brief Restore and verify the BSP's MTRRs on one unstarted AP.
 * @return False for incompatible capabilities or failed readback. AP boot
 * only, IRQs masked; no other CPU's registers or mappings are changed.
 */
bool x86_mtrr_restore_ap(void);
/** @brief Check local PAT/MTRR support and firmware MTRR enablement.
 * @return True only when PAT can describe WC safely on this CPU.
 */
bool x86_pat_native_supported(void);
/** @brief Read this CPU's supported IA32_PAT MSR. @return Its eight entries. */
uint64_t x86_pat_native_read(void);
/** @brief Install a valid PAT layout with local cache/TLB serialization.
 * @param layout Eight valid IA32_PAT entries. Caller masks IRQs; boot only.
 */
void x86_pat_native_install(uint64_t layout);
/** @brief Drain and invalidate local caches/TLBs before/after boot retyping.
 * Caller masks IRQs; boot only, with no concurrent CPUs or users.
 */
void x86_pat_native_flush(void);
#endif
