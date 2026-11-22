/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

#define I226_RETX_CTL            0x0041cu
#define I226_RETX_WATERMARK_MASK 0x0fu
#define I226_GTXOFFSET           0x03310u
#define I226_TXQCTL0             0x03344u
#define I226_DTXMXPKTSZ          0x0355cu
#define I226_DTXMXPKTSZ_DEFAULT  0x98u
#define I226_TQAVCTRL            0x03570u
#define I226_TQAVCTRL_TSN        (1u << 0)
#define I226_TQAVCTRL_ENH_QAV    (1u << 3)
#define I226_TQAVCTRL_FUTSCDDIS  (1u << 7)

int i225_allocate_rings(struct i225_device *d)
{
    d->rx_physical = d->api->alloc_dma(1, UINT64_MAX);
    d->tx_physical = d->api->alloc_dma(1, UINT64_MAX);
    d->rx_data_physical = d->api->alloc_dma(I225_RX_PAGES, UINT64_MAX);
    d->tx_data_physical = d->api->alloc_dma(I225_TX_PAGES, UINT64_MAX);
    if (!d->rx_physical || !d->tx_physical || !d->rx_data_physical || !d->tx_data_physical)
        return -12;
    d->rx = d->api->phys_to_virt(d->rx_physical);
    d->tx = d->api->phys_to_virt(d->tx_physical);
    d->rx_data = d->api->phys_to_virt(d->rx_data_physical);
    d->tx_data = d->api->phys_to_virt(d->tx_data_physical);
    if (!d->rx || !d->tx || !d->rx_data || !d->tx_data)
        return -12;
    for (unsigned i = 0; i < I225_RX_COUNT; ++i) {
        d->rx[i].read.packet = d->rx_data_physical + (uint64_t)i * I225_BUFFER_BYTES;
        d->rx[i].read.header = 0;
    }
    for (unsigned i = 0; i < I225_TX_COUNT; ++i)
        d->tx[i].writeback.status = I225_TX_DONE;
    return 0;
}

/** @brief Confirm descriptor fetching was enabled before publishing a queue.
 * @param d Initializing device; no spinlocks held. @param reg RXDCTL or TXDCTL.
 * @param enable Required enable bit. @return Zero or -ETIMEDOUT after 10 ms.
 */
static int wait_queue(struct i225_device *d, unsigned reg, uint32_t enable)
{
    for (unsigned elapsed = 0;; ++elapsed) {
        if (i225_read(d, reg) & enable)
            return 0;
        if (elapsed == 10)
            return -110;
        d->api->sleep_ms(1);
    }
}

/* Select ordinary Ethernet transmission after firmware may have configured TSN. */
static void i226_restore_legacy_tx(struct i225_device *d)
{
    i225_write(d, I226_GTXOFFSET, 0);
    i225_write(d, I226_DTXMXPKTSZ, I226_DTXMXPKTSZ_DEFAULT);
    /* Keep the normal retry buffer watermark and clear the scheduled threshold. */
    uint32_t retry = i225_read(d, I226_RETX_CTL) & I226_RETX_WATERMARK_MASK;
    i225_write(d, I226_RETX_CTL, retry);
    uint32_t scheduling = i225_read(d, I226_TQAVCTRL);
    scheduling &= ~(I226_TQAVCTRL_TSN | I226_TQAVCTRL_ENH_QAV | I226_TQAVCTRL_FUTSCDDIS);
    i225_write(d, I226_TQAVCTRL, scheduling);
    i225_write(d, I226_TXQCTL0, 0);
}

/** @brief Configure a single standard-MTU queue pair from owned DMA storage.
 * @param d Device with bus mastering disabled and initialized DMA descriptors.
 * @return Zero or -ETIMEDOUT; no interface may be published after failure.
 */
int i225_configure_rings(struct i225_device *d)
{
    i225_write(d, I225_RCTL, 0);
    i225_write(d, I225_TCTL, 0);
    i225_write(d, I225_RXDCTL0, 0);
    i225_write(d, I225_TXDCTL0, 0);
    /* Restore normal packet-buffer partitioning, including after TSN firmware. */
    i225_write(d, I225_RXPBS, 0x000000a2u);
    i225_write(d, I225_TXPBS, 0x04000014u);
    if (d->pci.device_id == I226_V_DEVICE_ID)
        i226_restore_legacy_tx(d);
    i225_write(d, I225_RXCSUM, 0); /* The network stack verifies IP and transport checksums. */
    i225_write(d, I225_MRQC, 0); /* All accepted frames belong to queue zero. */
    /* Disable IPv6 extension-header parsing as required by the hardware erratum. */
    i225_write(d, I225_RFCTL, I225_RFCTL_IPV6_EX_DIS);
    i225_write(d, I225_RLPML, I225_WIRE_FRAME);
    i225_write(d, I225_RDBAL0, (uint32_t)d->rx_physical);
    i225_write(d, I225_RDBAH0, (uint32_t)(d->rx_physical >> 32));
    i225_write(d, I225_RDLEN0, I225_RX_COUNT * sizeof(*d->rx));
    i225_write(d, I225_RDH0, 0);
    i225_write(d, I225_RDT0, 0);
    i225_write(d, I225_SRRCTL0, I225_SRRCTL_2048 | I225_SRRCTL_ADV);
    i225_write(d, I225_TDBAL0, (uint32_t)d->tx_physical);
    i225_write(d, I225_TDBAH0, (uint32_t)(d->tx_physical >> 32));
    i225_write(d, I225_TDLEN0, I225_TX_COUNT * sizeof(*d->tx));
    i225_write(d, I225_TDH0, 0);
    i225_write(d, I225_TDT0, 0);
    /* RAR[15] belongs to manageability; discard stale host filters in 1..14. */
    for (unsigned i = 1; i < 15; ++i) {
        i225_write(d, I225_RAL0 + i * 8, 0);
        i225_write(d, I225_RAH0 + i * 8, 0);
    }
    for (unsigned i = 0; i < 128; ++i)
        i225_write(d, I225_MTA + i * 4, 0);
    d->rx_next = d->tx_next = d->tx_clean = 0;
    d->dropping_fragments = false;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    i225_write(d, I225_TXDCTL0, 8u | (1u << 8) | (16u << 16) | I225_TXDCTL_ENABLE);
    i225_write(d, I225_RXDCTL0, 8u | (8u << 8) | (4u << 16) | I225_RXDCTL_ENABLE);
    i225_flush(d);
    int rc = wait_queue(d, I225_TXDCTL0, I225_TXDCTL_ENABLE);
    if (!rc)
        rc = wait_queue(d, I225_RXDCTL0, I225_RXDCTL_ENABLE);
    return rc;
}

/** @brief Enable the MAC and publish the initial receive descriptor budget.
 * @param d Initialized queue pair with bus mastering enabled, before publication.
 */
void i225_enable_rings(struct i225_device *d)
{
    i225_write(d, I225_TCTL, I225_TCTL_EN | I225_TCTL_PSP | (15u << 4) | (64u << 12));
    i225_write(d, I225_RCTL, I225_RCTL_EN | I225_RCTL_BAM | I225_RCTL_MPE |
               I225_RCTL_DPF | I225_RCTL_SECRC);
    i225_flush(d);
    /* Publish descriptors only after both queue fetching and receive are enabled. */
    i225_write(d, I225_RDT0, I225_RX_COUNT - 1);
    i225_flush(d);
}

void i225_free_rings(struct i225_device *d)
{
    if (d->rx_physical)
        d->api->free_dma(d->rx_physical, 1);
    if (d->tx_physical)
        d->api->free_dma(d->tx_physical, 1);
    if (d->rx_data_physical)
        d->api->free_dma(d->rx_data_physical, I225_RX_PAGES);
    if (d->tx_data_physical)
        d->api->free_dma(d->tx_data_physical, I225_TX_PAGES);
    d->rx_physical = d->tx_physical = d->rx_data_physical = d->tx_data_physical = 0;
    d->rx = NULL;
    d->tx = NULL;
    d->rx_data = d->tx_data = NULL;
}

/** @brief Copy and queue one Ethernet frame, padding short frames with zeros.
 * @param d Published device; callback owner pin keeps DMA storage alive.
 * @param frame Borrowed nonnull Ethernet bytes, excluding FCS.
 * @param length 14..1514 bytes. @return Zero, -EINVAL, -EMSGSIZE, -ENETDOWN
 * or -EAGAIN when the ring is full; serialized by the TX IRQ spinlock.
 */
int i225_send(struct i225_device *d, const void *frame, uint32_t length)
{
    if (!frame || length < 14)
        return -22;
    if (length > I225_MAX_FRAME)
        return -90;
    if (!d->active || !(i225_read(d, I225_STATUS) & I225_STATUS_LINK))
        return -100;
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->tx_lock, &flags);
    while (d->tx_clean != d->tx_next &&
           (d->tx[d->tx_clean].writeback.status & I225_TX_DONE)) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        d->tx_clean = (d->tx_clean + 1) % I225_TX_COUNT;
    }
    unsigned index = d->tx_next;
    unsigned next = (index + 1) % I225_TX_COUNT;
    if (next == d->tx_clean) {
        kernel_spin_unlock_irqrestore(&d->tx_lock, flags);
        return -11;
    }
    uint8_t *output = d->tx_data + index * I225_BUFFER_BYTES;
    const uint8_t *input = frame;
    for (unsigned i = 0; i < length; ++i)
        output[i] = input[i];
    uint32_t wire_length = length < I225_MIN_FRAME ? I225_MIN_FRAME : length;
    for (unsigned i = length; i < wire_length; ++i)
        output[i] = 0;
    d->tx[index].read.address = d->tx_data_physical + (uint64_t)index * I225_BUFFER_BYTES;
    d->tx[index].read.command = I225_TX_DTYP_DATA | I225_TX_DEXT | I225_TX_IFCS |
                                I225_TX_EOP | I225_TX_RS | wire_length;
    d->tx[index].read.offload = wire_length << 14;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d->tx_next = next;
    i225_write(d, I225_TDT0, next);
    i225_flush(d);
    kernel_spin_unlock_irqrestore(&d->tx_lock, flags);
    return 0;
}

int i225_receive(struct i225_device *d, void *frame, uint32_t capacity, uint32_t *length)
{
    if (length)
        *length = 0;
    if (!frame || !capacity)
        return -22;
    if (!d->active)
        return -19;
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->rx_lock, &flags);
    unsigned index = d->rx_next;
    uint32_t status = d->rx[index].writeback.status_error;
    if (!(status & I225_RX_DD)) {
        kernel_spin_unlock_irqrestore(&d->rx_lock, flags);
        i225_interrupt_complete(d);
        return 0;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint32_t bytes = d->rx[index].writeback.length;
    int rc = 1;
    if (bytes > capacity || bytes > I225_MAX_FRAME)
        rc = -90;
    else if (bytes < 14 || d->dropping_fragments || !(status & I225_RX_EOP) ||
             (status & I225_RX_ERR_MASK))
        rc = -74;
    d->dropping_fragments = !(status & I225_RX_EOP);
    if (rc == 1) {
        const uint8_t *input = d->rx_data + index * I225_BUFFER_BYTES;
        uint8_t *output = frame;
        for (unsigned i = 0; i < bytes; ++i)
            output[i] = input[i];
        if (length)
            *length = bytes;
    }
    d->rx[index].read.packet = d->rx_data_physical + (uint64_t)index * I225_BUFFER_BYTES;
    d->rx[index].read.header = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    i225_write(d, I225_RDT0, index);
    d->rx_next = (index + 1) % I225_RX_COUNT;
    kernel_spin_unlock_irqrestore(&d->rx_lock, flags);
    return rc;
}
