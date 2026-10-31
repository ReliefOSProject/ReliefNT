/* Production SMP CPU-id code with an observable call boundary around the
 * actual RDTSCP fallback, linked to the real execution gate and collector. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define smp_current_cpu fixture_real_smp_current_cpu
#include "../../arch/x86_64/cpu/smp.c"
#undef smp_current_cpu
#include <reliefnt/lock_profile.h>

static unsigned cpu_queries, clock_queries;
static uint64_t irq_flags = 1ULL << 9;

uint32_t smp_current_cpu(void)
{
    ++cpu_queries;
    return fixture_real_smp_current_cpu();
}
uint32_t apic_id(void) { return 42; }
void smp_membarrier_poll(void) {}
uint64_t kernel_irq_save(void) { uint64_t old = irq_flags; irq_flags = 0; return old; }
void kernel_irq_restore(uint64_t flags) { irq_flags = flags; }
uint64_t lock_profile_test_clock(void) { return ++clock_queries; }

static bool has_rdtscp(void)
{
    uint32_t a,b,c,d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(0x80000000u), "c"(0u));
    if (a < 0x80000001u) return false;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(0x80000001u), "c"(0u));
    return (d & (1u << 27)) != 0;
}

int main(void)
{
    uint64_t flags, nested;
    struct kernel_execution_suspend_token token = {0};
    cpu_count = 1; cpus[0].apic_id = 42;
    smp_cpu_aux_method = has_rdtscp() ? 1 : 0;
    unsigned before = cpu_queries;
    kernel_execution_lock_irqsave(&flags);
    assert(cpu_queries == before + 1 && clock_queries == 0);
    kernel_execution_lock_irqsave(&nested);
    assert(cpu_queries == before + 2 && clock_queries == 0);
    kernel_execution_unlock_irqrestore(nested);
    kernel_execution_unlock_irqrestore(flags);
    assert(cpu_queries == before + 4 && clock_queries == 0);

    before = cpu_queries;
    assert(kernel_execution_try_read_lock_irqsave(&flags));
    kernel_execution_read_unlock_irqrestore(flags);
    assert(cpu_queries == before && clock_queries == 0);
    kernel_execution_lock_irqsave(&flags);
    before = cpu_queries;
    assert(!kernel_execution_try_read_lock_irqsave(&nested));
    assert(cpu_queries == before && clock_queries == 0 && irq_flags == 0);
    kernel_execution_unlock_irqrestore(flags);

    before = cpu_queries;
    kernel_execution_lock_irqsave(&flags);
    assert(kernel_execution_suspend_irqrestore(&token) == 0);
    assert(kernel_execution_resume_irqsave(&token) == 0);
    kernel_execution_unlock_irqrestore(flags);
    assert(cpu_queries == before + 5 && clock_queries == 0);

    assert(!lock_profile_control("1", 1));
    before = cpu_queries;
    kernel_execution_lock_irqsave(&flags);
    kernel_execution_unlock_irqrestore(flags);
    assert(cpu_queries == before + 2 && clock_queries == 3);
    before = cpu_queries;
    assert(kernel_execution_try_read_lock_irqsave(&flags));
    kernel_execution_read_unlock_irqrestore(flags);
    assert(cpu_queries == before + 2 && clock_queries == 6);
    assert(!lock_profile_control("0", 1));
    before = cpu_queries;
    assert(kernel_execution_try_read_lock_irqsave(&flags));
    kernel_execution_read_unlock_irqrestore(flags);
    assert(cpu_queries == before && clock_queries == 6);
    printf("PASS disabled gate CPU lookup counts with production %s path\n",
           smp_cpu_aux_method == 1 ? "RDTSCP" : "APIC fallback (host lacks RDTSCP)");
}
