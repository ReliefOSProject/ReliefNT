/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/apic.h>
#include <reliefnt/lock.h>
#include <reliefnt/smp.h>

static struct kernel_spinlock membarrier_lock = KERNEL_SPINLOCK_INIT;
static uint64_t membarrier_sequence;
static uint64_t membarrier_request[SMP_MAX_CPUS];
static uint64_t membarrier_ack[SMP_MAX_CPUS];
static bool membarrier_sync_core;
static bool membarrier_flush_tlb;
/** @brief Invalidate local translation state without allocation or locks.
 * @return None. Architecture primitive, called with stable current mappings.
 */
extern void x86_64_flush_user_tlb(void);

/** @brief Complete local obligations for the published, stable request mode.
 * @param sync_core Also serialize instruction fetch with CPUID.
 * @return None. IRQ-masked/IPI context; never waits, allocates or takes locks.
 * The request owner preserves mode flags until every target acknowledges.
 */
static void cpu_memory_barrier(bool sync_core)
{
    if (membarrier_flush_tlb) x86_64_flush_user_tlb();
    if (sync_core) {
        uint32_t eax = 0, ebx, ecx, edx;
        __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : : "memory");
    } else {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
}

/** @brief Acknowledge a pending local request even while interrupts are masked.
 * @return None. IPI/task/lock-spin context without taking the request lock.
 * Acquire observes the owner's mode; release publishes completed invalidation
 * before that owner can reuse backing or change the next request's mode.
 * Local IRQ exclusion prevents a nested IPI from acknowledging an outer
 * request early and allowing its mode/acknowledgement to be overwritten.
 */
void smp_membarrier_poll(void)
{
    uint64_t flags = kernel_irq_save();
    uint32_t cpu = smp_current_cpu();
    uint64_t request = __atomic_load_n(&membarrier_request[cpu], __ATOMIC_ACQUIRE);
    if (request != __atomic_load_n(&membarrier_ack[cpu], __ATOMIC_RELAXED)) {
        cpu_memory_barrier(membarrier_sync_core);
        __atomic_store_n(&membarrier_ack[cpu], request, __ATOMIC_RELEASE);
    }
    kernel_irq_restore(flags);
}

/** @brief Serialize an all-CPU request while preserving peer progress.
 * @param sync_core Require instruction-stream serialization on each target.
 * @param flush_tlb Invalidate translation state before acknowledging.
 * @return None, only after every selected online peer acknowledges.
 * Boot/task context, nonrecursive and not NMI-safe. The ordinary spinlock masks
 * local IRQs and polls other requests while contending, so concurrent callers
 * cannot overwrite mode/sequence or strand each other's pending IPI.
 * No scheduler wait, allocation or page-table mutation occurs under this lock.
 */
static void smp_cpu_barrier(bool sync_core, bool flush_tlb)
{
    uint64_t irq_flags;
    kernel_spin_lock_irqsave(&membarrier_lock, &irq_flags);
    uint32_t current = smp_current_cpu();
    uint64_t targets = 0, sequence = ++membarrier_sequence;
    if (!sequence) sequence = ++membarrier_sequence;
    membarrier_sync_core = sync_core;
    membarrier_flush_tlb = flush_tlb;
    cpu_memory_barrier(sync_core);
    for (uint32_t cpu = 0; cpu < smp_cpu_count(); ++cpu) {
        if (cpu == current || !smp_cpu_online(cpu)) continue;
        targets |= 1ULL << cpu;
        __atomic_store_n(&membarrier_request[cpu], sequence, __ATOMIC_RELEASE);
        apic_send_ipi(smp_cpu_info(cpu)->apic_id, SMP_MEMBARRIER_VECTOR);
    }
    for (uint32_t cpu = 0; cpu < smp_cpu_count(); ++cpu) {
        if (!(targets & (1ULL << cpu))) continue;
        while (__atomic_load_n(&membarrier_ack[cpu], __ATOMIC_ACQUIRE) != sequence) {
            smp_membarrier_poll();
            __asm__ volatile("pause" : : : "memory");
        }
    }
    cpu_memory_barrier(sync_core);
    kernel_spin_unlock_irqrestore(&membarrier_lock, irq_flags);
}

/** @brief Order online CPUs, optionally serializing instruction fetch.
 * @param sync_core Require CPUID serialization on every participating CPU.
 * @return None after all acknowledgements, with original IRQ state preserved.
 * Nonrecursive boot/task context. Caller retains affected data/lifetimes and
 * any execution transaction required by its own operation.
 */
void smp_membarrier(bool sync_core) { smp_cpu_barrier(sync_core, false); }
/** @brief Invalidate online CPUs before unmapped backing may be reused.
 * @return None after acknowledgements, with original IRQ state preserved.
 * Nonrecursive boot/task context. Caller serializes PTE changes and retains
 * removed backing through this call; the rendezvous never frees pages.
 */
void smp_flush_user_tlb(void) { smp_cpu_barrier(false, true); }
