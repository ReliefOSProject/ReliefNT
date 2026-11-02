/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../../arch/x86_64/cpu/smp.c"

static uint64_t irq_state = 1ULL << 9;
static unsigned sent;
static uint32_t last_destination;
uint64_t kernel_irq_save(void)
{ uint64_t saved = irq_state; irq_state = 0; return saved; }
void kernel_irq_restore(uint64_t flags) { irq_state = flags; }
uint32_t apic_id(void) { return 0x32; }
void apic_send_ipi(uint32_t destination, uint8_t vector)
{
    assert(!irq_state && vector == SMP_RESCHEDULE_VECTOR);
    ++sent;
    last_destination = destination;
}

int main(void)
{
    smp_cpu_aux_method = 0;
    cpu_count = 3;
    cpus[0] = (struct smp_cpu_info){.apic_id = 0x32, .online = 1};
    cpus[1] = (struct smp_cpu_info){.apic_id = 0x81, .online = 1};
    cpus[2] = (struct smp_cpu_info){.apic_id = 0x44};
    smp_scheduler_started = 0;
    smp_send_reschedule(1);
    assert(!sent && irq_state == (1ULL << 9));
    smp_scheduler_started = 1;
    smp_send_reschedule(1);
    assert(sent == 1 && last_destination == 0x81 && irq_state == (1ULL << 9));
    smp_send_reschedule(0);
    smp_send_reschedule(2);
    smp_send_reschedule(3);
    smp_send_reschedule(SMP_MAX_CPUS);
    assert(sent == 1 && irq_state == (1ULL << 9));
    cpus[2].online = 1;
    smp_send_reschedule(2);
    assert(sent == 2 && last_destination == 0x44);
    irq_state = 0;
    smp_send_reschedule(1);
    assert(sent == 3 && last_destination == 0x81 && !irq_state);
    puts("PASS SMP reschedule: logical/APIC mapping, boot barrier, self/offline guards and IRQ state");
}
