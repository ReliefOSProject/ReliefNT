/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/power.h>

int xhci_timer_start(struct xhci_timer *timer)
{
    *timer=(struct xhci_timer){0};
    return power_acpi_pm_timer_read(&timer->last,&timer->mask) ? 0 : -EOPNOTSUPP;
}
bool xhci_timer_expired(struct xhci_timer *timer, uint64_t microseconds)
{
    uint32_t now,mask;
    if (!power_acpi_pm_timer_read(&now,&mask) || mask!=timer->mask) return true;
    timer->ticks+=(now-timer->last)&mask;timer->last=now;
    return timer->ticks>=(microseconds*ACPI_PM_TIMER_HZ+999999u)/1000000u;
}
void xhci_delay(unsigned milliseconds)
{
    struct xhci_timer timer;if(xhci_timer_start(&timer))return;
    for (unsigned spin=0;!xhci_timer_expired(&timer,(uint64_t)milliseconds*1000u);++spin) {
        if (spin==50000000u)return;
        __asm__ volatile("pause" ::: "memory");
    }
}
