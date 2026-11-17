/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

uint32_t i225_io_read(const volatile uint8_t *base, unsigned offset)
{
    return *(const volatile uint32_t *)(base + offset);
}

void i225_io_write(volatile uint8_t *base, unsigned offset, uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}
