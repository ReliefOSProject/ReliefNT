/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_RTL8139_INTERNAL_H
#define RELIEFNT_RTL8139_INTERNAL_H

#include <reliefos/driver.h>
#include <reliefnt/lock.h>

/* Classic RTL8139 registers, not RTL8169 or C+ descriptor mode. */
#define R8139_MAC 0x00u
#define R8139_MAR 0x08u
#define R8139_TSD 0x10u
#define R8139_TSAD 0x20u
#define R8139_RBSTART 0x30u
#define R8139_COMMAND 0x37u
#define R8139_CAPR 0x38u
#define R8139_IMR 0x3cu
#define R8139_ISR 0x3eu
#define R8139_TCR 0x40u
#define R8139_RCR 0x44u
#define R8139_MISSED 0x4cu
#define R8139_CFG9346 0x50u
#define R8139_CONFIG1 0x52u
#define R8139_MEDIA 0x58u

#define R8139_CMD_EMPTY 0x01u
#define R8139_CMD_TX 0x04u
#define R8139_CMD_RX 0x08u
#define R8139_CMD_RESET 0x10u
#define R8139_LINK_BAD 0x04u
#define R8139_RX_OVERFLOW 0x0050u
#define R8139_PCI_ERROR 0x8000u
#define R8139_TX_HOST 0x00002000u
#define R8139_TX_UNDERRUN 0x00004000u
#define R8139_TX_OK 0x00008000u
#define R8139_TX_ABORT 0x40000000u
#define R8139_RX_OK 0x0001u
#define R8139_RX_ERRORS 0x003eu

#define R8139_RX_BYTES 32768u
/* Extra space covers the chip's safety padding and a final DMA burst. */
#define R8139_RX_PAGES 9u
#define R8139_TX_COUNT 4u
#define R8139_TX_BYTES 2048u
#define R8139_TX_PAGES 2u
#define R8139_FRAME_MAX 1518u
/* Wrap inside the ring (RCR bit 7 clear), 32 KiB, accept station/broadcast/
 * multicast only, no runt/error packets. MAR admits IPv6 multicast. */
#define R8139_RX_CONFIG ((7u << 13) | (2u << 11) | (7u << 8) | 0x0eu)
#define R8139_TX_CONFIG ((3u << 24) | (7u << 8))

_Static_assert((R8139_RX_BYTES & (R8139_RX_BYTES - 1)) == 0, "power-of-two RX ring");
_Static_assert(R8139_RX_BYTES + 16 + R8139_TX_BYTES <= R8139_RX_PAGES * 4096,
               "RX DMA safety padding");
_Static_assert(R8139_TX_COUNT * R8139_TX_BYTES == R8139_TX_PAGES * 4096,
               "four TX DMA buffers");

struct r8139_device {
    struct r8139_device *next;
    const struct reliefos_driver_kernel_api *api;
    struct reliefos_driver_pci_device pci;
    uint16_t io_base, original_command;
    bool command_saved, active, fault;
    uint8_t mac[6];
    uint32_t ifindex, rx_next, tx_next, recoveries, tx_errors;
    uint64_t rx_pa, tx_pa;
    uint8_t *rx, *tx;
    bool tx_pending[R8139_TX_COUNT];
    uint64_t tx_submitted[R8139_TX_COUNT];
    struct kernel_spinlock lock;
};

/** @brief Read one register byte through the retained I/O BAR.
 * @param d Owned device. @param offset Register offset. @return Register byte.
 */
uint8_t r8139_read8(const struct r8139_device *d, unsigned offset);
/** @brief Read one register halfword through the retained I/O BAR.
 * @param d Owned device. @param offset Even register offset. @return Register halfword.
 */
uint16_t r8139_read16(const struct r8139_device *d, unsigned offset);
/** @brief Read one register dword through the retained I/O BAR.
 * @param d Owned device. @param offset Aligned register offset. @return Register dword.
 */
uint32_t r8139_read32(const struct r8139_device *d, unsigned offset);
/** @brief Write a register byte without waiting or allocating.
 * @param d Owned device. @param offset Register offset. @param value Byte to write.
 */
void r8139_write8(struct r8139_device *d, unsigned offset, uint8_t value);
/** @brief Write a register halfword without waiting or allocating.
 * @param d Owned device. @param offset Even register offset. @param value Halfword to write.
 */
void r8139_write16(struct r8139_device *d, unsigned offset, uint16_t value);
/** @brief Write a register dword without waiting or allocating.
 * @param d Owned device. @param offset Aligned register offset. @param value Dword to write.
 */
void r8139_write32(struct r8139_device *d, unsigned offset, uint32_t value);
/** @brief Enter D0, retain the I/O BAR and reset with PCI bus mastering closed.
 * @param d New device with validated services. @return Zero or negative errno.
 * Init task context; partial state remains owned for rollback.
 */
int r8139_probe(struct r8139_device *d);
/** @brief Allocate and map separate DMA32 RX and TX leases.
 * @param d Quiesced device. @return Zero or negative errno, retaining partial leases.
 */
int r8139_allocate(struct r8139_device *d);
/** @brief Publish complete DMA buffers and enable classic-mode TX/RX.
 * @param d Reset device with complete allocations. @return Zero or negative errno.
 * Called under exclusive initialization or the device lock; never sleeps.
 */
int r8139_activate(struct r8139_device *d);
/** @brief Reset and reinitialize existing DMA buffers after lost RX/TX progress.
 * @param d Live device under its lock. @return Zero or negative errno.
 * Task context; bounded polling, no allocation or sleeping. Failed reset
 * closes admission and retains all DMA leases until a proven STOP.
 */
int r8139_recover(struct r8139_device *d);
/** @brief Mask interrupts, reset and confirm PCI bus-master disable.
 * @param d Partial/live device after provider callback drain.
 * @return Zero only when DMA may be freed; otherwise retain all resources.
 * Task context; may sleep during the bounded reset wait.
 */
int r8139_stop(struct r8139_device *d);
/** @brief Release matching DMA leases after proven STOP.
 * @param d Quiesced partial/live device; clears the allocation fields.
 */
void r8139_free(struct r8139_device *d);
/** @brief Stop admitting work after an unrecoverable device error.
 * @param d Live device under its lock. No wait, free or callback detachment.
 */
void r8139_fault(struct r8139_device *d);
/** @brief Submit one frame, padding short Ethernet frames with zero bytes.
 * @param d Callback-pinned device. @param frame Borrowed bytes.
 * @param length Frame bytes excluding FCS. @return Zero or negative Linux errno.
 * Task context; queue-full returns EAGAIN without waiting.
 */
int r8139_send(struct r8139_device *d, const void *frame, uint32_t length);
/** @brief Consume one complete frame, stripping FCS and validating the DMA header.
 * @param d Callback-pinned device. @param frame Writable output.
 * @param capacity Output bytes. @param length Optional output length, zero on failure.
 * @return One frame, zero if empty/in-flight, or negative Linux errno.
 * Task context; at most one frame or one bounded recovery per call.
 */
int r8139_receive(struct r8139_device *d, void *frame, uint32_t capacity, uint32_t *length);

#endif
