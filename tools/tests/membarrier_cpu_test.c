#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include "../../arch/x86_64/cpu/smp.c"
#include "../../kernel/sync/lock.c"

static _Thread_local uint32_t test_cpu;
static atomic_int stop_workers;
static atomic_uint sent[4];
static atomic_uint tlb_flushes[4];
static _Thread_local uint64_t test_irq_flags = 1ULL << 9;
static atomic_uint producers_ready[201];
uint64_t kernel_irq_save(void)
{
    uint64_t saved = test_irq_flags;
    test_irq_flags = 0;
    return saved;
}
void kernel_irq_restore(uint64_t flags) { test_irq_flags = flags; }
void x86_64_flush_user_tlb(void)
{
    atomic_fetch_add(&tlb_flushes[test_cpu], 1);
}

uint32_t apic_id(void) { return test_cpu; }

void apic_send_ipi(uint32_t destination, uint8_t vector)
{
    assert(vector == SMP_MEMBARRIER_VECTOR && destination < 3 && destination != test_cpu);
    atomic_fetch_add_explicit(&sent[destination], 1, memory_order_relaxed);
}

static void *remote_cpu(void *argument)
{
    test_cpu = (uintptr_t)argument;
    while (!atomic_load_explicit(&stop_workers, memory_order_relaxed)) {
        /* Model the lock-free handler also used with masked interrupts. */
        smp_membarrier_poll();
    }
    return NULL;
}

static void *concurrent_cpu(void *argument)
{
    test_cpu = (uintptr_t)argument;
    test_irq_flags = 0; /* No asynchronous IPI handler can make progress here. */
    for (unsigned i = 0; i < 200; ++i) {
        atomic_fetch_add(&producers_ready[i], 1);
        while (atomic_load(&producers_ready[i]) < 3) smp_membarrier_poll();
        if (i & 1) smp_flush_user_tlb();
        else smp_membarrier(true);
        assert(test_irq_flags == 0);
    }
    atomic_fetch_add(&producers_ready[200], 1);
    while (atomic_load(&producers_ready[200]) < 3) smp_membarrier_poll();
    return NULL;
}

int main(void)
{
    alarm(10); /* Turn a rendezvous deadlock into a bounded test failure. */
    /* Host TSC_AUX identifies a Linux CPU, not this fixture's logical CPU.
     * Use the mocked LAPIC identity for these simulated worker threads. */
    smp_cpu_aux_method = 0;
    cpu_count = 4;
    for (uint32_t i = 0; i < 4; ++i) {
        cpus[i].apic_id = i;
        cpus[i].online = i < 3;
    }
    pthread_t workers[2];
    for (uintptr_t i = 1; i <= 2; ++i) assert(pthread_create(&workers[i - 1], NULL, remote_cpu, (void *)i) == 0);
    for (uint64_t sequence = 1; sequence <= 2000; ++sequence) {
        if (sequence % 3 == 0) smp_flush_user_tlb();
        else smp_membarrier(sequence % 2);
        for (unsigned cpu = 1; cpu <= 2; ++cpu)
            assert(__atomic_load_n(&membarrier_ack[cpu], __ATOMIC_ACQUIRE) == sequence);
        assert(membarrier_request[0] == 0 && membarrier_request[3] == 0);
        assert(test_irq_flags == (1ULL << 9));
    }
    atomic_store(&stop_workers, 1);
    for (unsigned i = 0; i < 2; ++i) {
        assert(pthread_join(workers[i], NULL) == 0);
        assert(atomic_load(&sent[i + 1]) == 2000);
        assert(atomic_load(&tlb_flushes[i + 1]) == 2000 / 3);
    }
    uint64_t before = membarrier_sequence;
    unsigned flushed_before[3];
    for (unsigned cpu = 0; cpu < 3; ++cpu)
        flushed_before[cpu] = atomic_load(&tlb_flushes[cpu]);
    pthread_t producers[3];
    for (uintptr_t cpu = 0; cpu < 3; ++cpu)
        assert(pthread_create(&producers[cpu], NULL, concurrent_cpu, (void *)cpu) == 0);
    for (unsigned cpu = 0; cpu < 3; ++cpu) assert(pthread_join(producers[cpu], NULL) == 0);
    assert(membarrier_sequence == before + 600);
    for (unsigned cpu = 0; cpu < 3; ++cpu)
        /* 100 local TLB barriers flush before/after, 200 remote ones once. */
        assert(atomic_load(&tlb_flushes[cpu]) == flushed_before[cpu] + 400);
    alarm(0);
    puts("PASS concurrent membarrier/TLB rendezvous with masked IRQs and preserved flags");
    puts("PASS membarrier CPU rendezvous: remote acknowledgement, offline CPUs, repeated normal/sync-core barriers");
}
