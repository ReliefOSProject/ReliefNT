/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/port.h>
#include <reliefnt/time.h>

/** @brief Read a device byte through its owned UC mapping.
 * @param d Mapped device. @param offset In-range byte offset. @return Register value.
 */
uint8_t rtl_read8(const struct rtl_device *d, unsigned offset)
{
    if (d->io_base)
        return d->api->inb(d->io_base + offset);
    return *(volatile uint8_t *)(d->registers + offset);
}

/** @brief Read an aligned device word through its owned UC mapping.
 * @param d Mapped device. @param offset In-range even offset. @return Register value.
 */
uint16_t rtl_read16(const struct rtl_device *d, unsigned offset)
{
    if (d->io_base)
        return x86_64_inw(d->io_base + offset);
    return *(volatile uint16_t *)(d->registers + offset);
}

/** @brief Read an aligned device dword through its owned UC mapping.
 * @param d Mapped device. @param offset In-range aligned offset. @return Register value.
 */
uint32_t rtl_read32(const struct rtl_device *d, unsigned offset)
{
    if (d->io_base)
        return d->api->inl(d->io_base + offset);
    return *(volatile uint32_t *)(d->registers + offset);
}

/** @brief Issue a byte write without sleeping or acquiring locks.
 * @param d Mapped device. @param offset In-range offset. @param value Register value.
 */
void rtl_write8(struct rtl_device *d, unsigned offset, uint8_t value)
{
    if (d->io_base) {
        d->api->outb(d->io_base + offset, value);
        return;
    }
    *(volatile uint8_t *)(d->registers + offset) = value;
}

/** @brief Issue a word write without sleeping or acquiring locks.
 * @param d Mapped device. @param offset In-range even offset. @param value Register value.
 */
void rtl_write16(struct rtl_device *d, unsigned offset, uint16_t value)
{
    if (d->io_base) {
        x86_64_outw(value, d->io_base + offset);
        return;
    }
    *(volatile uint16_t *)(d->registers + offset) = value;
}

/** @brief Issue a dword write without sleeping or acquiring locks.
 * @param d Mapped device. @param offset In-range aligned offset. @param value Register value.
 */
void rtl_write32(struct rtl_device *d, unsigned offset, uint32_t value)
{
    if (d->io_base) {
        d->api->outl(d->io_base + offset, value);
        return;
    }
    *(volatile uint32_t *)(d->registers + offset) = value;
}

/** @brief Provide a settling delay while initialization owns the hardware.
 * @param d Device with validated sleep callback. @param microseconds Minimum interval.
 * Caller holds no spinlock. Short waits pin the CPU with IRQs masked and poll
 * boot uptime; bounded failure or longer waits use the tick sleep service,
 * which temporarily permits interrupts. The caller's IRQ state is restored.
 */
void rtl_delay_us(struct rtl_device *d, unsigned microseconds)
{
    uint64_t flags;

    if (!microseconds)
        return;

    /* NULL saves IRQ state without acquiring a lock. This pins the short
     * clock comparison to one CPU, while allowing the fallback to sleep. */
    kernel_spin_lock_irqsave(NULL, &flags);
    if (microseconds < 1000u) {
        uint64_t start = time_uptime_us();
        uint64_t previous = start;

        /* Uptime can stop at a pending tick, or regress after PIT rollover.
         * The extra microsecond covers truncation at both sample boundaries. */
        for (unsigned samples = 0; samples < 4096u; ++samples) {
            uint64_t now = time_uptime_us();
            if (now < previous)
                break;
            if (now - start >= (uint64_t)microseconds + 1u) {
                kernel_spin_unlock_irqrestore(NULL, flags);
                return;
            }
            previous = now;
            __asm__ volatile("pause" : : : "memory");
        }
    }

    /* A tick sleep can start just before its first tick. One extra whole tick
     * ensures the remaining ticks cover at least the requested interval. */
    uint64_t milliseconds = ((uint64_t)microseconds + 999u) / 1000u;
    milliseconds += (1000u + RELIEFNT_TICK_HZ - 1u) / RELIEFNT_TICK_HZ;
    d->api->sleep_ms(milliseconds);
    kernel_spin_unlock_irqrestore(NULL, flags);
}
