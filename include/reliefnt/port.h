/*
 * ReliefOS port interface: declares low-level platform I/O abstractions.
 * Keeps architecture-specific port access behind a stable kernel header.
 */
#ifndef RELIEFNT_PORT_H
#define RELIEFNT_PORT_H

#include <reliefnt/types.h>

/**
 * @brief Write the byte value to the I/O port.
 */
void x86_64_outb(uint8_t value, uint16_t port);
/**
 * @brief Read a byte from the I/O port.
 */
uint8_t x86_64_inb(uint16_t port);
/**
 * @brief Write the 16-bit value to the I/O port.
 */
void x86_64_outw(uint16_t value, uint16_t port);
/**
 * @brief Read a 16-bit value from the I/O port.
 */
uint16_t x86_64_inw(uint16_t port);
/**
 * @brief Read repeated 16-bit words from one x86 port into a CPU buffer.
 * @param port Privileged I/O port with a ready data-in phase.
 * @param buffer Required writable current kernel mapping of count*2 bytes;
 * may be unaligned. The caller retains it throughout the synchronous transfer.
 * @param count Number of words admitted by the device's current protocol phase.
 * Caller owns device/transport exclusion and module lifetime. Does no DMA,
 * allocation, readiness polling or byte swapping; leaves the direction flag clear.
 */
void x86_64_insw(uint16_t port, void *buffer, size_t count);
/**
 * @brief Write repeated 16-bit words from a CPU buffer to one x86 port.
 * @param port Privileged I/O port with a ready data-out phase.
 * @param buffer Required readable current kernel mapping of count*2 bytes;
 * may be unaligned. The caller retains it throughout the synchronous transfer.
 * @param count Number of words admitted by the device's current protocol phase.
 * Caller owns device/transport exclusion and module lifetime. Does no DMA,
 * allocation, readiness polling or byte swapping; leaves the direction flag clear.
 */
void x86_64_outsw(uint16_t port, const void *buffer, size_t count);
/**
 * @brief Write the 32-bit value to the I/O port.
 */
void x86_64_outl(uint32_t value, uint16_t port);
/**
 * @brief Read a 32-bit value from the I/O port.
 */
uint32_t x86_64_inl(uint16_t port);

#endif
