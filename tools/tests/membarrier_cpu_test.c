#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <reliefnt/smp.h>
#include <reliefnt/lock.h>

static _Thread_local uint32_t test_cpu;
static atomic_int stop_workers;
static atomic_uint sent[4];
static atomic_uint tlb_flushes[4];
void x86_64_flush_user_tlb(void)
{
    atomic_fetch_add(&tlb_flushes[test_cpu], 1);
}

static _Thread_local uint64_t irq_state;
uint32_t smp_current_cpu(void) { return test_cpu; }
uint32_t smp_cpu_count(void) { return 4; }
bool smp_cpu_online(uint32_t cpu) { return cpu < 3; }
const struct smp_cpu_info *smp_cpu_info(uint32_t cpu)
{
    static const struct smp_cpu_info topology[4]={{.apic_id=0},{.apic_id=1},{.apic_id=2},{.apic_id=3}};
    assert(cpu < 4);return topology+cpu;
}
uint64_t kernel_irq_save(void) { uint64_t flags=irq_state;irq_state=0;return flags; }
void kernel_irq_restore(uint64_t flags) { irq_state=flags; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{
    *flags=kernel_irq_save();
    while(__atomic_exchange_n(&lock->state,1,__ATOMIC_ACQUIRE))smp_membarrier_poll();
}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{
    __atomic_store_n(&lock->state,0,__ATOMIC_RELEASE);kernel_irq_restore(flags);
}

void apic_send_ipi(uint32_t destination, uint8_t vector)
{
    assert(vector == SMP_MEMBARRIER_VECTOR && destination > 0 && destination < 3);
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

int main(void)
{
    pthread_t workers[2];
    for (uintptr_t i = 1; i <= 2; ++i) assert(pthread_create(&workers[i - 1], NULL, remote_cpu, (void *)i) == 0);
    for (uint64_t sequence = 1; sequence <= 2000; ++sequence) {
        if (sequence % 3 == 0) smp_flush_user_tlb();
        else smp_membarrier(sequence % 2);
        assert(atomic_load(&sent[0]) == 0 && atomic_load(&sent[3]) == 0);

    }
    atomic_store(&stop_workers, 1);
    for (unsigned i = 0; i < 2; ++i) {
        assert(pthread_join(workers[i], NULL) == 0);
        assert(atomic_load(&sent[i + 1]) == 2000);
        assert(atomic_load(&tlb_flushes[i + 1]) == 2000 / 3);
    }
    puts("PASS membarrier CPU rendezvous: remote acknowledgement, offline CPUs, repeated normal/sync-core barriers");
}
