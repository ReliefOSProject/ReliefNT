/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_E1000E_INTERNAL_H
#define RELIEFNT_E1000E_INTERNAL_H
#include <reliefos/driver.h>
#include <reliefnt/lock.h>

#define E1K_CTRL       0x0000u
#define E1K_STATUS     0x0008u
#define E1K_EECD       0x0010u
#define E1K_EERD       0x0014u
#define E1K_CTRL_EXT   0x0018u
#define E1K_MDIC       0x0020u
#define E1K_FCAL       0x0028u
#define E1K_FCAH       0x002cu
#define E1K_FCT        0x0030u
#define E1K_ICR        0x00c0u
#define E1K_IMC        0x00d8u
#define E1K_RCTL       0x0100u
#define E1K_TCTL       0x0400u
#define E1K_TIPG       0x0410u
#define E1K_EXTCNF     0x0f00u
#define E1K_PBA        0x1000u
#define E1K_RXCSUM     0x5000u
#define E1K_RFCTL      0x5008u
#define E1K_MTA        0x5200u
#define E1K_RAL        0x5400u
#define E1K_RAH        0x5404u
#define E1K_GCR        0x5b00u
#define E1K_MMIO_BYTES 0x6000u
#define E1K_RX_BASE    0x2800u
#define E1K_TX_BASE    0x3800u
#define E1K_CTRL_RST   (1u << 26)
#define E1K_CTRL_SLU   (1u << 6)
#define E1K_CTRL_GIO_DISABLE (1u << 2)
#define E1K_STATUS_LINK (1u << 1)
#define E1K_STATUS_MASTER (1u << 19)
#define E1K_DRV_LOAD   (1u << 28)
#define E1K_SW_OWN     (1u << 5)
#define E1K_RX_COUNT   256u
#define E1K_TX_COUNT   64u
#define E1K_BUFFER     2048u
#define E1K_MAX_FRAME  1518u
#define E1K_RX_PAGES   (E1K_RX_COUNT * E1K_BUFFER / 4096u)
#define E1K_TX_PAGES   (E1K_TX_COUNT * E1K_BUFFER / 4096u)

struct e1k_rx_descriptor {
    uint64_t address;
    uint16_t length, checksum;
    uint8_t status, errors;
    uint16_t special;
} __attribute__((packed));
struct e1k_tx_descriptor {
    uint64_t address;
    uint16_t length;
    uint8_t checksum_start, command, status, checksum_offset;
    uint16_t special;
} __attribute__((packed));
_Static_assert(sizeof(struct e1k_rx_descriptor)==16, "Intel legacy RX descriptor");
_Static_assert(sizeof(struct e1k_tx_descriptor)==16, "Intel legacy TX descriptor");
_Static_assert(E1K_RX_COUNT*sizeof(struct e1k_rx_descriptor)==4096, "RX ring page");
_Static_assert(E1K_TX_COUNT*sizeof(struct e1k_tx_descriptor)<=4096, "TX ring page");

struct e1k_device {
    struct e1k_device *next;
    const struct reliefos_driver_kernel_api *api;
    struct reliefos_driver_pci_device pci;
    volatile uint8_t *registers;
    uint16_t original_command;
    bool command_saved, present, active, dropping_fragments, software_owns_phy;
    uint8_t mac[6];
    uint64_t rx_physical, tx_physical, rx_data_physical, tx_data_physical;
    volatile struct e1k_rx_descriptor *rx;
    volatile struct e1k_tx_descriptor *tx;
    uint8_t *rx_data, *tx_data;
    unsigned rx_next, tx_next, tx_clean;
    struct kernel_spinlock rx_lock, tx_lock;
};

/** @brief Read one aligned device register through the owned UC mapping.
 * @param d Initialized device with a live MMIO lease. @param offset Register byte offset.
 * @return Little-endian x86 register value; no allocation or lock acquisition.
 */
static inline uint32_t e1k_read(const struct e1k_device *d, unsigned offset)
{ return *(volatile uint32_t *)(d->registers+offset); }
/** @brief Issue one aligned device register write; callers flush when required.
 * @param d Device with a live MMIO lease. @param offset Register offset. @param value Register value.
 */
static inline void e1k_write(struct e1k_device *d, unsigned offset, uint32_t value)
{ *(volatile uint32_t *)(d->registers+offset)=value; }
/** @brief Flush posted PCI writes before changing device ownership or DMA state.
 * @param d Device whose MMIO lease remains live throughout the read.
 */
static inline void e1k_flush(struct e1k_device *d)
{ (void)e1k_read(d,E1K_STATUS); }

/** @brief Map and reset one already selected supported PCIe controller.
 * @param d Selected PCI identity and validated services, retained for rollback.
 * @return Zero, or negative Linux errno; all acquired resources remain owned by d.
 */
int e1k_hardware_start(struct e1k_device *d);
/** @brief Stop all ring DMA before backing or MMIO can be released.
 * @param d Retained initialized or partially initialized device; callbacks already drained.
 * @return Zero for safe release, or -ETIMEDOUT retaining every backing allocation.
 */
int e1k_hardware_stop(struct e1k_device *d);
/** @brief Read the station address and negotiate standard copper Ethernet link.
 * @param d Reset controller with live BAR and completed NVM autoload.
 * @return Zero even without a cable, or negative NVM/MDIO/PHY error.
 */
int e1k_setup_link(struct e1k_device *d);
/** @brief Allocate coherent physical descriptor and packet buffers for both queues.
 * @param d Stopped device retaining every successful allocation for cleanup.
 * @return Zero or -ENOMEM; does not enable DMA or release partially acquired memory.
 */
int e1k_allocate_rings(struct e1k_device *d);
/** @brief Program legacy descriptor queues before enabling RX/TX.
 * @param d Reset device with complete DMA buffers; bus mastering may now be enabled.
 */
void e1k_enable_rings(struct e1k_device *d);
/** @brief Release exactly the DMA allocations retained by this stopped device.
 * @param d Stopped device after successful hardware_stop; safe for partial init.
 */
void e1k_free_rings(struct e1k_device *d);
/** @brief Copy and enqueue one Ethernet frame without waiting for hardware progress.
 * @param d Active device retained by callback pin. @param frame Borrowed bytes.
 * @param length Frame size excluding hardware FCS, at most E1K_MAX_FRAME.
 * @return Zero on admission, -EAGAIN for a full ring, or negative input/link error.
 */
int e1k_send(struct e1k_device *d, const void *frame, uint32_t length);
/** @brief Consume one complete frame and return its descriptor to the device.
 * @param d Active pinned device. @param frame Writable capacity-byte buffer.
 * @param capacity Buffer size. @param length Optional output reset to zero on failure.
 * @return One for a frame, zero for an empty ring, or negative malformed/size error.
 */
int e1k_receive(struct e1k_device *d, void *frame, uint32_t capacity, uint32_t *length);
#endif
