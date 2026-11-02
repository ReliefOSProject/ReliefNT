/* SPDX-License-Identifier: Apache-2.0 */
#include "pat_internal.h"

#define IA32_PAT 0x277u
#define IA32_MTRR_DEF_TYPE 0x2ffu

/** @brief Read a feature-validated model-specific register.
 * @param msr Supported register number. @return Its complete 64-bit value.
 */
static uint64_t pat_read_msr(uint32_t msr)
{
    uint32_t low,high;
    __asm__ volatile("rdmsr" : "=a"(low),"=d"(high) : "c"(msr));
    return (uint64_t)high<<32 | low;
}

/** @brief Require PAT and enabled MTRRs before installing WC page policies.
 * @return False when firmware or this CPU cannot honor the requested mode.
 */
bool x86_pat_native_supported(void)
{
    uint32_t a,b,c,d;
    __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(1),"c"(0));
    return (d & ((1u<<16)|(1u<<12)))==((1u<<16)|(1u<<12)) &&
           (pat_read_msr(IA32_MTRR_DEF_TYPE)&(1u<<11));
}

/** @brief Read IA32_PAT after local capability validation. @return Eight entries. */
uint64_t x86_pat_native_read(void) { return pat_read_msr(IA32_PAT); }

/** @brief Flush all local translations, including global leaves.
 * IRQs are masked by the boot caller; CR4 is restored without changing policy.
 */
static void pat_flush_tlb(void)
{
    uint64_t cr3,cr4;
    __asm__ volatile("mov %%cr3,%0; mov %%cr4,%1" : "=r"(cr3),"=r"(cr4));
    if(cr4 & (1u<<7)) {
        uint64_t clear=cr4 & ~(1ULL<<7);
        __asm__ volatile("mov %0,%%cr4" :: "r"(clear) : "memory");
    }
    __asm__ volatile("mov %0,%%cr3" :: "r"(cr3) : "memory");
    if(cr4 & (1u<<7)) __asm__ volatile("mov %0,%%cr4" :: "r"(cr4) : "memory");
}

/** @brief Serialize a boot-only PAT replacement with caches temporarily disabled.
 * @param layout Valid, capability-checked IA32_PAT entries.
 * Caller masks IRQs; preserves CR0/CR4 and drains WC/WB stores and translations
 * on both sides of the change. No lock or allocation occurs.
 */
void x86_pat_native_install(uint64_t layout)
{
    uint64_t cr0;
    __asm__ volatile("mfence; mov %%cr0,%0" : "=r"(cr0) :: "memory");
    uint64_t disabled=(cr0 | (1ULL<<30)) & ~(1ULL<<29);
    __asm__ volatile("mov %0,%%cr0; wbinvd" :: "r"(disabled) : "memory");
    pat_flush_tlb();
    __asm__ volatile("wrmsr" :: "c"(IA32_PAT),"a"((uint32_t)layout),
                     "d"((uint32_t)(layout>>32)) : "memory");
    __asm__ volatile("wbinvd" ::: "memory");
    pat_flush_tlb();
    __asm__ volatile("mov %0,%%cr0" :: "r"(cr0) : "memory");
}

/** @brief Drain local stores/caches and invalidate translations during BSP retyping.
 * IRQs masked, before APs/users; no mapping mutation or policy change.
 */
void x86_pat_native_flush(void)
{
    __asm__ volatile("mfence; wbinvd" ::: "memory");
    pat_flush_tlb();
}
