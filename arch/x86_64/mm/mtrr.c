/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/paging.h>
#include "pat_internal.h"

#define MTRR_CAP 0xfeu
#define MTRR_BASE 0x200u
#define MTRR_DEFAULT 0x2ffu
#define MTRR_ENABLE (1u<<11)
#define MTRR_FIXED_ENABLE (1u<<10)
#define MTRR_FIXED_CAP (1u<<8)
#define MTRR_CAP_MASK (0xffu|MTRR_FIXED_CAP|(1u<<10))

static const uint32_t fixed_msrs[]={
    0x250u,0x258u,0x259u,0x268u,0x269u,0x26au,
    0x26bu,0x26cu,0x26du,0x26eu,0x26fu
};
/* VCNT is an eight-bit architectural field; no allocation at AP startup. */
static struct {
    uint64_t capabilities, default_type, variable[256][2], fixed[11];
    uint8_t physical_bits;
    bool present;
} boot_mtrrs;
static bool captured;

/** @brief Capture every firmware range that affects the BSP's cache policy.
 * BSP boot only, IRQs masked. Unsupported MTRRs cause no MSR accesses; the
 * published snapshot is immutable before the first AP is started.
 */
void x86_mtrr_capture_boot(void)
{
    if (!(x86_cache_native_features() & (1u<<12))) goto publish;
    boot_mtrrs.present=true;
    boot_mtrrs.capabilities=x86_cache_native_read(MTRR_CAP) & MTRR_CAP_MASK;
    boot_mtrrs.physical_bits=paging_physical_bits();
    boot_mtrrs.default_type=x86_cache_native_read(MTRR_DEFAULT);
    for (uint32_t i=0;i<(boot_mtrrs.capabilities & 0xffu);++i) {
        boot_mtrrs.variable[i][0]=x86_cache_native_read(MTRR_BASE+2u*i);
        boot_mtrrs.variable[i][1]=x86_cache_native_read(MTRR_BASE+2u*i+1u);
    }
    if (boot_mtrrs.capabilities & MTRR_FIXED_CAP)
        for (uint32_t i=0;i<11;++i)
            boot_mtrrs.fixed[i]=x86_cache_native_read(fixed_msrs[i]);
publish:
    __atomic_store_n(&captured,true,__ATOMIC_RELEASE);
}

/** @brief Compare the current compatible CPU with the immutable BSP snapshot.
 * @return True only for identical default, variable and supported fixed ranges.
 * AP boot only; caller masks IRQs and has already validated MSR availability.
 */
static bool mtrrs_match(void)
{
    if (x86_cache_native_read(MTRR_DEFAULT)!=boot_mtrrs.default_type) return false;
    for (uint32_t i=0;i<(boot_mtrrs.capabilities & 0xffu);++i)
        for (uint32_t half=0;half<2;++half)
            if (x86_cache_native_read(MTRR_BASE+2u*i+half)!=boot_mtrrs.variable[i][half])
                return false;
    if (boot_mtrrs.capabilities & MTRR_FIXED_CAP)
        for (uint32_t i=0;i<11;++i)
            if (x86_cache_native_read(fixed_msrs[i])!=boot_mtrrs.fixed[i]) return false;
    return true;
}

/** @brief Restore firmware MTRRs lost when INIT resets an application CPU.
 * @return True after exact readback, false for incompatible CPUs or writes.
 * Caller masks IRQs before this CPU can use shared mappings or run users.
 * Only local registers change, with caches/TLBs serialized on both sides;
 * PAT admission remains responsible for matching the page-level selectors.
 */
bool x86_mtrr_restore_ap(void)
{
    if (!__atomic_load_n(&captured,__ATOMIC_ACQUIRE)) return false;
    if (!boot_mtrrs.present) return !(x86_cache_native_features() & (1u<<12));
    if (!(x86_cache_native_features() & (1u<<12)) ||
        (x86_cache_native_read(MTRR_CAP) & MTRR_CAP_MASK)!=boot_mtrrs.capabilities ||
        paging_physical_bits()!=boot_mtrrs.physical_bits) return false;
    if (mtrrs_match()) return true;
    uint64_t cr0=x86_cache_native_begin();
    uint64_t current=x86_cache_native_read(MTRR_DEFAULT);
    x86_cache_native_write(MTRR_DEFAULT,current & ~(uint64_t)(MTRR_ENABLE|MTRR_FIXED_ENABLE));
    if (boot_mtrrs.capabilities & MTRR_FIXED_CAP)
        for (uint32_t i=0;i<11;++i)
            x86_cache_native_write(fixed_msrs[i],boot_mtrrs.fixed[i]);
    for (uint32_t i=0;i<(boot_mtrrs.capabilities & 0xffu);++i)
        for (uint32_t half=0;half<2;++half)
            x86_cache_native_write(MTRR_BASE+2u*i+half,boot_mtrrs.variable[i][half]);
    x86_cache_native_write(MTRR_DEFAULT,boot_mtrrs.default_type);
    x86_cache_native_end(cr0);
    return mtrrs_match();
}
