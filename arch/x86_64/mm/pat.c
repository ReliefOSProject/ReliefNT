/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/pat.h>
#include <reliefnt/lock.h>
#include "pat_internal.h"

static bool attempted, ready;
static uint64_t boot_layout;

/** @brief Reserve the unused fourth-index PAT entry while preserving firmware.
 * @param enabled False when nopat requests the conservative boot policy.
 * @return True after a verified install, false with the original layout retained.
 * BSP only before APs/users; IRQ state is restored exactly. The own bootstrap
 * maps use only slot zero (WB) and slot three (UC) before this call.
 */
bool arch_pat_boot_init(bool enabled)
{
    if (attempted) return arch_pat_writecombine_ready();
    attempted=true;
    uint64_t flags=kernel_irq_save();
    x86_mtrr_capture_boot();
    if (enabled && x86_pat_native_supported()) {
        uint64_t firmware=x86_pat_native_read();
        if ((firmware & 0xffu)==6u && ((firmware>>24)&0xffu)==0u) {
            uint64_t layout=(firmware & ~(0xffULL<<32)) | (1ULL<<32);
            x86_pat_native_install(layout);
            if (x86_pat_native_read()==layout) {
                boot_layout=layout;
                __atomic_store_n(&ready,true,__ATOMIC_RELEASE);
            } else x86_pat_native_install(firmware);
        }
    }
    kernel_irq_restore(flags);
    return arch_pat_writecombine_ready();
}

/** @brief Match BSP MTRRs and PAT before an AP can use shared mappings.
 * @return False if this CPU cannot implement the BSP policy; it stays offline.
 * AP bootstrap only. Preserves IRQ state, takes no shared lock and never changes
 * the BSP policy. MTRRs are synchronized even when nopat disables WC pages;
 * unsupported cache MSRs are never accessed.
 */
bool arch_pat_ap_init(void)
{
    uint64_t flags=kernel_irq_save();
    bool compatible=false;
    if (!x86_mtrr_restore_ap()) goto done;
    if (!arch_pat_writecombine_ready()) compatible=true;
    else if (x86_pat_native_supported()) {
        if (x86_pat_native_read()!=boot_layout) x86_pat_native_install(boot_layout);
        compatible=x86_pat_native_read()==boot_layout;
    }
done:
    kernel_irq_restore(flags);
    return compatible;
}

/** @brief Query the BSP's verified WC layout with AP publication ordering.
 * @return True only after a successful enabled bootstrap.
 */
bool arch_pat_writecombine_ready(void)
{
    return __atomic_load_n(&ready,__ATOMIC_ACQUIRE);
}

/** @brief Drain local data and translations around bootstrap cache retyping.
 * Only the BSP calls this before APs/users, with IRQs masked.
 */
void arch_pat_flush_boot(void)
{
    if (arch_pat_writecombine_ready()) x86_pat_native_flush();
}
