/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_X86_PAT_INTERNAL_H
#define RELIEFNT_X86_PAT_INTERNAL_H
#include <reliefnt/types.h>
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
