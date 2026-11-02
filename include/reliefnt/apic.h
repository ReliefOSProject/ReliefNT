/*
 * ReliefOS local APIC and IOAPIC capability interface.
 * The BSP uses either IOAPIC routes or the LAPIC virtual-wire bridge for
 * legacy hardware IRQs.  AP startup relies on the same LAPIC interface.
 */
#ifndef RELIEFNT_APIC_H
#define RELIEFNT_APIC_H

#include <reliefnt/types.h>

void apic_init(void);
void ioapic_init(void);
/**
 * @brief Rebind retained LAPIC/IOAPIC MMIO pages to the kernel direct map.
 * @return None; unavailable devices are disabled if their alias cannot be set.
 * Call after physical memory and the supervisor direct map are initialized,
 * before interrupt routing, AP startup, or user address spaces are activated.
 */
void apic_map_kernel_aliases(void);
bool apic_available(void);
bool ioapic_available(void);
uint32_t apic_id(void);
uint32_t apic_bsp_id(void);
uint32_t apic_cpu_count(void);
uint32_t apic_cpu_id_at(uint32_t index);
void apic_enable(void);
bool apic_enabled(void);
void apic_enable_legacy_pic(void);
/* Scheduler preemption is independent of the 100 Hz PIT/public tick clock. */
#define APIC_SCHED_TICK_HZ 250u
#define APIC_SCHED_FALLBACK_COUNT 400000u
/**
 * @brief Calibrate a local periodic timer against the free-running ACPI PM clock.
 * @param vector Scheduler interrupt vector installed in this CPU's IDT.
 * @param hz Requested interrupts per second; zero leaves the timer unchanged.
 * @param fallback_count Nominal initial count on bounded calibration failure;
 * zero selects the conservative default, without certifying an actual rate.
 * Caller masks local IRQs and holds no shared locks. BSP calls after platform
 * and scheduler initialization; APs call after the scheduler-start barrier.
 */
void apic_timer_init(uint8_t vector, uint32_t hz, uint32_t fallback_count);
void apic_eoi(void);
void apic_send_init(uint32_t destination);
void apic_send_startup(uint32_t destination, uint8_t vector);
/** @brief Send a fixed-delivery interrupt to one physical local APIC. */
void apic_send_ipi(uint32_t destination, uint8_t vector);
/**
 * @brief Send an NMI to one online physical xAPIC without taking locks.
 * @param destination Physical xAPIC ID; the caller has interrupts masked.
 * @return True if submitted, false for unavailable APIC or delivery timeout.
 */
bool apic_send_nmi(uint32_t destination);
bool ioapic_route_irq(uint32_t irq, uint8_t vector, uint32_t destination);

#endif
