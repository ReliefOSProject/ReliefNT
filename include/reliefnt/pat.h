/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_PAT_H
#define RELIEFNT_PAT_H
#include <reliefnt/types.h>

/** @brief Reserve PAT slot four for WC before publishing any WC mapping.
 * @param enabled False for the Linux-compatible nopat boot option.
 * @return True when the BSP installed and verified a compatible PAT layout.
 * BSP boot only, before APs/users. Keeps all other firmware entries; unsupported
 * PAT/MTRR or an incompatible base layout retains the existing UC fallback.
 * The first attempt fixes the boot policy; subsequent calls cannot enable it.
 */
bool arch_pat_boot_init(bool enabled);
/** @brief Match the BSP's MTRR and PAT layouts before publishing an AP online.
 * @return True for matching cache policy, false when this AP cannot honor
 * shared mappings and must remain offline. Firmware MTRRs are synchronized
 * even with nopat; unsupported MSRs are never accessed.
 * AP boot only, IRQs masked and no shared lock held.
 */
bool arch_pat_ap_init(void);
/** @brief Query the boot-selected WC policy. @return True after verified setup. */
bool arch_pat_writecombine_ready(void);
/** @brief Drain caches and invalidate local translations around boot retyping.
 * BSP only before APs/users; caller masks IRQs. No mapping is changed here.
 */
void arch_pat_flush_boot(void);
#endif
