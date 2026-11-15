/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_VMXNET3_INTERNAL_H
#define RELIEFNT_VMXNET3_INTERNAL_H
#include <reliefos/driver.h>
#include <reliefnt/lock.h>

/* VMXNET3 revision 1 / UPT 1: x86_64 little-endian wire fields. */
#define VMX_BAR_BYTES 4096u
#define VMX_TX_COUNT 64u
#define VMX_RX_COUNT 64u
#define VMX_RX_BODY_COUNT 32u
#define VMX_RXC_COUNT (VMX_RX_COUNT + VMX_RX_BODY_COUNT)
#define VMX_BUFFER_BYTES 2048u
#define VMX_FRAME_MAX 1518u
#define VMX_DATA_PAGES (VMX_TX_COUNT * VMX_BUFFER_BYTES / 4096u)
#define VMX_RING_PAGES 2u
#define VMX_QUEUE_OFFSET 1024u
#define VMX_TX_OFFSET 0u
#define VMX_TXC_OFFSET 1024u
#define VMX_RX_OFFSET 2048u
#define VMX_RX_BODY_OFFSET 3072u
#define VMX_RXC_OFFSET 3584u
#define VMX_REVISION 0u
#define VMX_UPT 8u
#define VMX_DS_LOW 0x10u
#define VMX_DS_HIGH 0x18u
#define VMX_COMMAND 0x20u
#define VMX_MAC_LOW 0x28u
#define VMX_MAC_HIGH 0x30u
#define VMX_EVENT_CAUSE 0x40u
#define VMX_IMR 0u
#define VMX_TX_PROD 0x600u
#define VMX_RX_PROD 0x800u
#define VMX_RX_BODY_PROD 0xa00u
#define VMX_ACTIVATE 0xcafe0000u
#define VMX_QUIESCE 0xcafe0001u
#define VMX_RESET 0xcafe0002u
#define VMX_UPDATE_RX_MODE 0xcafe0003u
#define VMX_GET_LINK 0xf00d0002u
#define VMX_GET_INTR_CONFIG 0xf00d0008u
#define VMX_GEN 0x80000000u

/* Four dwords provide the hardware's 16-byte ring cell without C bitfields. */
struct vmx_desc {
    uint64_t address;
    uint32_t a, b;
};
_Static_assert(sizeof(struct vmx_desc) == 16, "VMXNET3 descriptor size");
_Static_assert(VMX_RXC_OFFSET + VMX_RXC_COUNT * 16 <= VMX_RING_PAGES * 4096, "ring allocation");

struct vmx_device {
    struct vmx_device *next;
    const struct reliefos_driver_kernel_api *api;
    struct reliefos_driver_pci_device pci;
    void *bar[2];
    uint16_t original_command;
    bool command_saved, exposed, active;
    unsigned broken;
    uint32_t ifindex, irq_handle, event_pending;
    unsigned irq_mode;
    bool link_valid, link_up, auto_mask, irq_masked;
    uint8_t mac[6];
    uint64_t shared_pa, rings_pa, tx_data_pa, rx_data_pa;
    uint8_t *shared, *tx_data, *rx_data;
    volatile struct vmx_desc *tx, *txc, *rx, *rxc;
    unsigned tx_next, tx_clean, txc_next, tx_gen, txc_gen;
    unsigned rx_next, rx_fill, rxc_next, rx_gen, rxc_gen;
    struct kernel_spinlock command_lock, tx_lock, rx_lock, irq_lock;
};

/** @brief Read a 32-bit device register from one retained MMIO lease.
 * @param base Mapped BAR. @param offset Aligned register offset. @return Register value.
 */
uint32_t vmx_io_read(void *base, unsigned offset);
/** @brief Write a 32-bit device register through one retained MMIO lease.
 * @param base Mapped BAR. @param offset Aligned register offset. @param value Word to write.
 */
void vmx_io_write(void *base, unsigned offset, uint32_t value);
/** @brief Map, quiesce, reset and negotiate one device.
 * @param d New context with validated services. @return Zero or negative errno; retains partial
 * resources.
 */
int vmx_hardware_start(struct vmx_device *d);
/** @brief Synchronously quiesce and reset a device before DMA leases are freed.
 * @param d Context with callback admission closed. @return Zero or negative errno retaining unsafe
 * resources.
 */
int vmx_hardware_stop(struct vmx_device *d);
/** @brief Read cached carrier in IRQ mode or query it in polling fallback.
 * @param d Live callback-pinned context under network execution.
 * @return Nonzero only while the device reports link up; task context only.
 */
int vmx_link(struct vmx_device *d);
/** @brief Issue a command under the per-device command lock.
 * @param d Mapped retained controller. @param value Command word.
 * @return Synchronous completion word; task context, no sleeping.
 */
uint32_t vmx_command(struct vmx_device *d, uint32_t value);
/** @brief Refresh the task-context carrier cache after activation or a link event.
 * @param d Active retained controller. Command access is serialized.
 */
void vmx_refresh_link(struct vmx_device *d);
/** @brief Prefer one owned MSI-X entry, then MSI, otherwise retain polling.
 * @param d Quiesced controller whose BARs are mapped; init task context.
 */
void vmx_interrupt_prepare(struct vmx_device *d);
/** @brief Mask the device and synchronously drain the owned IRQ callback.
 * @param d Retained controller with callback admission closed; task context.
 */
void vmx_interrupt_stop(struct vmx_device *d);
/** @brief Consume event causes in task context and refresh cached carrier.
 * @param d Retained active controller. @return Zero or negative EIO.
 */
int vmx_interrupt_events(struct vmx_device *d);
/** @brief Rearm only after empty RX/TX service and recheck for a racing completion.
 * @param d Active retained controller; caller holds rx_lock. Task context only.
 */
void vmx_interrupt_complete(struct vmx_device *d);
/** @brief Reclaim validated TX completions under the queue lock.
 * @param d Retained active controller. @return Zero or negative EIO.
 */
int vmx_reclaim_tx(struct vmx_device *d);
/** @brief Allocate and initialize coherent shared memory and descriptor rings.
 * @param d Negotiated device. @return Zero or -ENOMEM, retaining partial leases for cleanup.
 */
int vmx_allocate(struct vmx_device *d);
/** @brief Configure and activate one TX/RX queue without optional offloads.
 * @param d Device with complete DMA allocations. @return Zero or negative activation error.
 */
int vmx_activate(struct vmx_device *d);
/** @brief Release all DMA leases after proven hardware quiescence.
 * @param d Stopped or partially initialized device.
 */
void vmx_free(struct vmx_device *d);
/** @brief Submit one complete Ethernet frame without waiting for the device.
 * @param d Active context. @param frame Borrowed packet. @param length Packet bytes.
 * @return Zero, or negative Linux errno for invalid, down, full or broken queues.
 */
int vmx_send(struct vmx_device *d, const void *frame, uint32_t length);
/** @brief Consume a complete, validated Ethernet frame if one is available.
 * @param d Active context. @param frame Writable output. @param capacity Output bytes.
 * @param length Optional output length. @return One frame, zero if empty, or negative errno.
 */
int vmx_receive(struct vmx_device *d, void *frame, uint32_t capacity, uint32_t *length);
#endif
