/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/power.h>

/** @brief Start a hardware timer that advances even with every CPU's IRQs masked. */
int ehci_timer_start(struct ehci_timer *timer)
{
    *timer=(struct ehci_timer){0};
    return power_acpi_pm_timer_read(&timer->last,&timer->mask) ? 0 : -EOPNOTSUPP;
}

/** @brief Accumulate 24/32-bit PM timer wrap without relying on BSP timer IRQs. */
bool ehci_timer_expired(struct ehci_timer *timer,uint64_t microseconds)
{
    uint32_t now,mask;
    if(!power_acpi_pm_timer_read(&now,&mask) || mask!=timer->mask)return true;
    timer->ticks+=(now-timer->last)&mask;timer->last=now;
    return timer->ticks >= (microseconds*ACPI_PM_TIMER_HZ+999999u)/1000000u;
}

/** @brief Wait a minimum reset/debounce interval without waiting for IRQ delivery. */
void ehci_delay(unsigned milliseconds)
{
    struct ehci_timer timer;
    if(ehci_timer_start(&timer))return;
    for(unsigned spin=0;!ehci_timer_expired(&timer,(uint64_t)milliseconds*1000u);++spin) {
        if(spin==50000000u)return; /* A stopped clock must not hang the kernel. */
        __asm__ volatile("pause" ::: "memory");
    }
}
