/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/port.h>

/** @brief Read one byte from this controller's assigned register window.
 * @param d Retained device. @param offset Byte offset. @return Register value.
 */
uint8_t r8139_read8(const struct r8139_device *d, unsigned offset)
{
    return d->api->inb(d->io_base + offset);
}

/** @brief Read one halfword from this controller's assigned register window.
 * @param d Retained device. @param offset Even offset. @return Register value.
 */
uint16_t r8139_read16(const struct r8139_device *d, unsigned offset)
{
    return x86_64_inw(d->io_base + offset);
}

/** @brief Read one dword from this controller's assigned register window.
 * @param d Retained device. @param offset Aligned offset. @return Register value.
 */
uint32_t r8139_read32(const struct r8139_device *d, unsigned offset)
{
    return d->api->inl(d->io_base + offset);
}

/** @brief Write one byte to this controller's assigned register window.
 * @param d Retained device. @param offset Byte offset. @param value New value.
 */
void r8139_write8(struct r8139_device *d, unsigned offset, uint8_t value)
{
    d->api->outb(d->io_base + offset, value);
}

/** @brief Write one halfword to this controller's assigned register window.
 * @param d Retained device. @param offset Even offset. @param value New value.
 */
void r8139_write16(struct r8139_device *d, unsigned offset, uint16_t value)
{
    x86_64_outw(value, d->io_base + offset);
}

/** @brief Write one dword to this controller's assigned register window.
 * @param d Retained device. @param offset Aligned offset. @param value New value.
 */
void r8139_write32(struct r8139_device *d, unsigned offset, uint32_t value)
{
    d->api->outl(d->io_base + offset, value);
}
