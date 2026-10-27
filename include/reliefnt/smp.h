/*
 * ReliefOS symmetric multiprocessing interface.
 * Provides the CPU topology boundary used by the scheduler and APIC code.
 */
#ifndef RELIEFNT_SMP_H
#define RELIEFNT_SMP_H

#include <reliefnt/types.h>

#define SMP_MAX_CPUS 64u
#define SMP_MEMBARRIER_VECTOR 0x41u

struct smp_cpu_info {
    uint32_t apic_id;
    uint64_t stack;
    volatile uint32_t online;
    volatile uint32_t started;
};

void smp_init(void);
uint32_t smp_cpu_count(void);
uint32_t smp_current_cpu(void);
bool smp_is_ready(void);
void smp_start_aps(void);
void smp_mark_bsp_user_entry(void);
/* Release APs after the BSP has completed initial userland construction. */
void smp_release_aps(void);
void smp_ap_entry(uint32_t cpu_index) __attribute__((noreturn));
bool smp_cpu_online(uint32_t cpu_index);
const struct smp_cpu_info *smp_cpu_info(uint32_t cpu_index);

/** @brief Acknowledge a pending local CPU barrier without taking locks.
 * @return None. IPI/task/spin context, including masked IRQs. Published mode is
 * acquired before invalidation; acknowledgement releases completed effects.
 * Saves/restores local IRQ state to prevent nested acknowledgements.
 */
void smp_membarrier_poll(void);
/**
 * @brief Order online CPUs and wait for every acknowledgement.
 * @param sync_core Also serialize instruction fetch on each target.
 * @return None; preserves the caller's original IRQ state.
 * @note Nonrecursive boot/task context, not NMI-safe. Requests serialize
 * internally while contenders continue polling; caller retains affected data,
 * lifetimes and any execution transaction required by its own operation.
 */
void smp_membarrier(bool sync_core);
/** @brief Invalidate online CPUs before reusing unmapped user or kernel backing.
 * @return None after acknowledgements, preserving the original IRQ state.
 * @note Nonrecursive boot/task context, not NMI-safe. Caller serializes PTE
 * changes and retains backing through this internally serialized rendezvous.
 */
void smp_flush_user_tlb(void);

#endif
