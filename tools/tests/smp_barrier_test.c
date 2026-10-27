/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <reliefnt/apic.h>
#include <reliefnt/lock.h>
#include <reliefnt/smp.h>

enum { ROUNDS=200, IRQ_ENABLE=1u<<9 };
static _Thread_local uint32_t current_cpu;
static _Thread_local uint64_t irq_state;
static atomic_uint done[2],flushes[2],ipis[2];
static pthread_barrier_t entry;
static const struct smp_cpu_info topology[2]={{.apic_id=10,.online=1},{.apic_id=11,.online=1}};

uint32_t smp_current_cpu(void){return current_cpu;}
uint32_t smp_cpu_count(void){return 2;}
bool smp_cpu_online(uint32_t cpu){return cpu<2;}
const struct smp_cpu_info *smp_cpu_info(uint32_t cpu){assert(cpu<2);return topology+cpu;}
void x86_64_flush_user_tlb(void)
{assert(!(irq_state&IRQ_ENABLE));atomic_fetch_add(flushes+current_cpu,1);}
uint64_t kernel_irq_save(void)
{uint64_t flags=irq_state;irq_state&=~(uint64_t)IRQ_ENABLE;return flags;}
void kernel_irq_restore(uint64_t flags){irq_state=flags;}
void apic_send_ipi(uint32_t target,uint8_t vector)
{
    assert(target>=10 && target<12 && vector==SMP_MEMBARRIER_VECTOR);
    /* IRQ delivery is deferred. A CPU with IF clear must service the mailbox
     * while contending for the ordinary kernel spinlock, as production does. */
    atomic_fetch_add(ipis+(target-10),1);
}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{
    *flags=irq_state;irq_state&=~(uint64_t)IRQ_ENABLE;
    while(__atomic_exchange_n(&lock->state,1,__ATOMIC_ACQUIRE)) {
        smp_membarrier_poll();__asm__ volatile("pause" ::: "memory");
    }
}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{
    assert(!(irq_state&IRQ_ENABLE));
    __atomic_store_n(&lock->state,0,__ATOMIC_RELEASE);irq_state=flags;
}
static void *worker(void *value)
{
    current_cpu=(uint32_t)(uintptr_t)value;
    for(unsigned round=1;round<=ROUNDS;++round) {
        irq_state=round&1 ? 0 : IRQ_ENABLE;
        uint64_t before=irq_state;
        int rc=pthread_barrier_wait(&entry);assert(!rc || rc==PTHREAD_BARRIER_SERIAL_THREAD);
        if(!current_cpu || !(round&1))smp_flush_user_tlb();
        else smp_membarrier(true);
        assert(irq_state==before);
        atomic_store_explicit(done+current_cpu,round,memory_order_release);
        while(atomic_load_explicit(done+(current_cpu^1),memory_order_acquire)<round)
            smp_membarrier_poll();
    }
    return NULL;
}
int main(void)
{
    pthread_t threads[2];assert(!pthread_barrier_init(&entry,NULL,2));
    for(unsigned i=0;i<2;++i)assert(!pthread_create(threads+i,NULL,worker,(void *)(uintptr_t)i));
    for(unsigned i=0;i<2;++i)assert(!pthread_join(threads[i],NULL));
    assert(atomic_load(flushes)==2*ROUNDS+ROUNDS/2);
    assert(atomic_load(flushes+1)==ROUNDS+ROUNDS);
    assert(atomic_load(ipis)==ROUNDS && atomic_load(ipis+1)==ROUNDS);
    assert(!pthread_barrier_destroy(&entry));
    puts("PASS concurrent TLB/sync-core rendezvous, deferred IPIs, exact flush obligations and original IRQ states");
    return 0;
}
