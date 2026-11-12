/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_R8169_INTERNAL_H
#define RELIEFNT_R8169_INTERNAL_H

#include <reliefos/driver.h>
#include <reliefnt/lock.h>

#define RTL_RX_COUNT 256u
#define RTL_TX_COUNT 256u
#define RTL_BUFFER_SIZE 2048u
#define RTL_MAX_FRAME 1518u
#define RTL_RX_PAGES (RTL_RX_COUNT * RTL_BUFFER_SIZE / 4096u)
#define RTL_TX_PAGES (RTL_TX_COUNT * RTL_BUFFER_SIZE / 4096u)
#define RTL_DESC_OWN (1u << 31)
#define RTL_DESC_EOR (1u << 30)
#define RTL_DESC_FIRST (1u << 29)
#define RTL_DESC_LAST (1u << 28)
#define RTL_RX_ERROR (1u << 21)

enum rtl_family { RTL_FAMILY_8168, RTL_FAMILY_8125 };

struct rtl_descriptor {
    uint32_t command;
    uint32_t options;
    uint64_t address;
} __attribute__((aligned(16)));

_Static_assert(sizeof(struct rtl_descriptor) == 16, "Realtek descriptor size");
_Static_assert(RTL_RX_COUNT * sizeof(struct rtl_descriptor) == 4096, "RX ring page");
_Static_assert(RTL_TX_COUNT * sizeof(struct rtl_descriptor) == 4096, "TX ring page");

struct rtl_device {
    const struct reliefos_driver_kernel_api *api;
    struct reliefos_driver_pci_device pci;
    volatile uint8_t *registers;
    uint32_t mmio_bytes;
    uint32_t xid;
    enum rtl_family family;
    unsigned chip_revision;
    const char *chip_name;
    uint32_t quirks;
    int io_error;
    uint16_t original_command;
    uint16_t io_base;
    bool command_saved;
    bool present;
    bool active;
    bool dropping_fragments;
    uint8_t mac[6];
    uint64_t rx_physical, tx_physical, rx_data_physical, tx_data_physical;
    volatile struct rtl_descriptor *rx, *tx;
    uint8_t *rx_data, *tx_data;
    unsigned rx_next, tx_next, tx_clean;
    struct kernel_spinlock rx_lock, tx_lock;
};

/** @brief Read one byte from the device's memory or legacy I/O BAR.
 * @param d Bound device. @param offset Register offset within mmio_bytes.
 * @return Hardware register byte. No allocation or scheduling.
 */
uint8_t rtl_read8(const struct rtl_device *d, unsigned offset);
/** @brief Read an aligned little-endian 16-bit device register.
 * @param d Bound device. @param offset In-range even byte offset.
 * @return Hardware register value. No allocation or scheduling.
 */
uint16_t rtl_read16(const struct rtl_device *d, unsigned offset);
/** @brief Read an aligned little-endian 32-bit device register.
 * @param d Bound device. @param offset In-range aligned byte offset.
 * @return Hardware register value. No allocation or scheduling.
 */
uint32_t rtl_read32(const struct rtl_device *d, unsigned offset);
/** @brief Write one byte through the live device MMIO lease.
 * @param d Bound device. @param offset In-range offset. @param value Register value.
 */
void rtl_write8(struct rtl_device *d, unsigned offset, uint8_t value);
/** @brief Write an aligned 16-bit device register without scheduling.
 * @param d Bound device. @param offset In-range even offset. @param value Register value.
 */
void rtl_write16(struct rtl_device *d, unsigned offset, uint16_t value);
/** @brief Write an aligned 32-bit device register without scheduling.
 * @param d Bound device. @param offset In-range aligned offset. @param value Register value.
 */
void rtl_write32(struct rtl_device *d, unsigned offset, uint32_t value);
/** @brief Provide the requested settling delay in initialization context.
 * @param d Bound device with sleep service. @param microseconds Minimum wait.
 * Uses bounded boot-uptime polling or tick sleep; no spinlock may be held.
 * Short-wait accuracy depends on the boot clock.
 * A sleep can temporarily enable interrupts; entry IRQ state is restored.
 */
void rtl_delay_us(struct rtl_device *d, unsigned microseconds);

/** @brief Identify a supported RTL8111/8168 MAC by its TxConfig XID.
 * @param d Mapped device with xid read before any hardware reset.
 * @return Zero with chip metadata, or -ENODEV for an unrecognized revision.
 */
int rtl8168_identify(struct rtl_device *d);
/** @brief Configure the gigabit MAC, PHY and the already allocated DMA rings.
 * @param d Identified mapped device with coherent rings and PCI bus mastering enabled.
 * @return Zero with RX/TX running, or negative errno retaining resources for stop.
 */
int rtl8168_start(struct rtl_device *d);
/** @brief Quiesce and reset the gigabit MAC before releasing any DMA memory.
 * @param d Identified device, including partially initialized state.
 * @return Zero on confirmed reset, or negative errno requiring resource retention.
 */
int rtl8168_stop(struct rtl_device *d);

/** @brief Identify a supported RTL8125 MAC using TxConfig's hardware revision.
 * @param d Mapped device with xid set before reset.
 * @return Zero with chip metadata, or -ENODEV for unsupported silicon.
 */
int rtl8125_identify(struct rtl_device *d);
/** @brief Load the matching firmware and configure one standard RX/TX queue.
 * @param d Identified mapped device with rings and PCI bus mastering enabled.
 * @return Zero with RX/TX running, or negative errno retaining all resources.
 */
int rtl8125_start(struct rtl_device *d);
/** @brief Disable and reset RTL8125 DMA before the owner releases its resources.
 * @param d Identified mapped device, including partial initialization.
 * @return Zero on confirmed quiescence, or negative errno requiring retention.
 */
int rtl8125_stop(struct rtl_device *d);

/** @brief Probe a supported PCI identity, enter D0, claim a BAR and identify the MAC.
 * @param d Zeroed state with validated services; retained for mandatory rollback.
 * @return Zero or negative Linux errno. Leaves bus mastering disabled.
 */
int rtl_hardware_probe(struct rtl_device *d);
/** @brief Stop controller DMA and disable PCI bus mastering before release.
 * @param d Owned initialized or partial device with callbacks drained.
 * @return Zero on confirmed STOP or negative errno requiring resource retention.
 */
int rtl_hardware_stop(struct rtl_device *d);
/** @brief Allocate coherent RX/TX descriptors and packet backing.
 * @param d Stopped device; successful partial allocations remain in state.
 * @return Zero or -ENOMEM; physical addresses are device DMA addresses.
 */
int rtl_allocate_rings(struct rtl_device *d);
/** @brief Release retained DMA allocations after confirmed device STOP.
 * @param d Quiescent device, including partial allocation state.
 */
void rtl_free_rings(struct rtl_device *d);
/** @brief Queue an Ethernet frame without waiting for hardware completion.
 * @param d Active pinned device. @param frame Borrowed bytes. @param length Excludes FCS.
 * @return Zero on admission or negative errno including -EAGAIN when full.
 */
int rtl_send(struct rtl_device *d, const void *frame, uint32_t length);
/** @brief Consume at most one completed RX descriptor and recycle it safely.
 * @param d Active pinned device. @param frame Output buffer. @param capacity Buffer size.
 * @param length Optional output, cleared on empty/error; successful size excludes FCS.
 * @return One for a frame, zero for empty, or negative Linux errno.
 */
int rtl_receive(struct rtl_device *d, void *frame, uint32_t capacity, uint32_t *length);

#endif
