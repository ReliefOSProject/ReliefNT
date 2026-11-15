/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
/** @brief Read a 32-bit register from the device's uncached BAR.
 * @param base Retained mapping. @param offset Aligned byte offset. @return Register value.
 */
uint32_t vmx_io_read(void *base, unsigned offset)
{
    return *(volatile uint32_t *)((volatile uint8_t *)base + offset);
}
/** @brief Write a 32-bit register through the device's uncached BAR.
 * @param base Retained mapping. @param offset Aligned byte offset. @param value Register value.
 */
void vmx_io_write(void *base, unsigned offset, uint32_t value)
{
    *(volatile uint32_t *)((volatile uint8_t *)base + offset) = value;
}
