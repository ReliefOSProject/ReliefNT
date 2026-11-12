/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Allocate coherent descriptor pages and fixed packet buffers.
 * @param d Stopped device retaining each successful allocation for rollback.
 * @return Zero or -ENOMEM; no allocation is freed until hardware STOP succeeds.
 */
int rtl_allocate_rings(struct rtl_device *d)
{
    d->rx_physical = d->api->alloc_dma(1, UINT64_MAX);
    d->tx_physical = d->api->alloc_dma(1, UINT64_MAX);
    d->rx_data_physical = d->api->alloc_dma(RTL_RX_PAGES, UINT64_MAX);
    d->tx_data_physical = d->api->alloc_dma(RTL_TX_PAGES, UINT64_MAX);
    if (!d->rx_physical || !d->tx_physical || !d->rx_data_physical || !d->tx_data_physical)
        return -12;
    d->rx = d->api->phys_to_virt(d->rx_physical);
    d->tx = d->api->phys_to_virt(d->tx_physical);
    d->rx_data = d->api->phys_to_virt(d->rx_data_physical);
    d->tx_data = d->api->phys_to_virt(d->tx_data_physical);
    if (!d->rx || !d->tx || !d->rx_data || !d->tx_data)
        return -12;
    for (unsigned i = 0; i < RTL_RX_COUNT; ++i) {
        d->rx[i].address = d->rx_data_physical + (uint64_t)i * RTL_BUFFER_SIZE;
        d->rx[i].options = 0;
        d->rx[i].command = RTL_DESC_OWN | RTL_BUFFER_SIZE |
            (i == RTL_RX_COUNT - 1 ? RTL_DESC_EOR : 0);
    }
    for (unsigned i = 0; i < RTL_TX_COUNT; ++i) {
        d->tx[i].address = d->tx_data_physical + (uint64_t)i * RTL_BUFFER_SIZE;
        d->tx[i].options = 0;
        d->tx[i].command = i == RTL_TX_COUNT - 1 ? RTL_DESC_EOR : 0;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return 0;
}

/** @brief Release coherent storage after hardware and PCI DMA admission stop.
 * @param d Owned stopped device with drained callbacks, including partial allocation.
 */
void rtl_free_rings(struct rtl_device *d)
{
    if (d->rx_physical)
        d->api->free_dma(d->rx_physical, 1);
    if (d->tx_physical)
        d->api->free_dma(d->tx_physical, 1);
    if (d->rx_data_physical)
        d->api->free_dma(d->rx_data_physical, RTL_RX_PAGES);
    if (d->tx_data_physical)
        d->api->free_dma(d->tx_data_physical, RTL_TX_PAGES);
    d->rx_physical = d->tx_physical = d->rx_data_physical = d->tx_data_physical = 0;
    d->rx = d->tx = NULL;
    d->rx_data = d->tx_data = NULL;
}

/** @brief Enqueue one Ethernet frame with ownership published after all bytes.
 * @param d Pinned live device. @param frame Borrowed Ethernet bytes.
 * @param length Frame size excluding FCS, up to RTL_MAX_FRAME.
 * @return Zero, -EINVAL, -EMSGSIZE, -ENETDOWN or -EAGAIN for a full ring.
 * IRQ-safe exclusion only protects the queue; this function never waits or allocates.
 */
int rtl_send(struct rtl_device *d, const void *frame, uint32_t length)
{
    if (!frame || length < 14)
        return -22;
    if (length > RTL_MAX_FRAME)
        return -90;
    if (!d->active || !(rtl_read8(d, 0x6c) & 2))
        return -100;
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->tx_lock, &flags);
    while (d->tx_clean != d->tx_next && !(d->tx[d->tx_clean].command & RTL_DESC_OWN)) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        d->tx_clean = (d->tx_clean + 1) % RTL_TX_COUNT;
    }
    unsigned index = d->tx_next;
    unsigned next = (index + 1) % RTL_TX_COUNT;
    if (next == d->tx_clean) {
        kernel_spin_unlock_irqrestore(&d->tx_lock, flags);
        return -11;
    }
    uint8_t *output = d->tx_data + index * RTL_BUFFER_SIZE;
    const uint8_t *input = frame;
    for (unsigned i = 0; i < length; ++i)
        output[i] = input[i];
    /* Explicit zero padding avoids exposing previously transmitted packet bytes. */
    unsigned wire_length = length < 60 ? 60 : length;
    for (unsigned i = length; i < wire_length; ++i)
        output[i] = 0;
    d->tx[index].options = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d->tx[index].command = RTL_DESC_OWN | RTL_DESC_FIRST | RTL_DESC_LAST | wire_length |
        (index == RTL_TX_COUNT - 1 ? RTL_DESC_EOR : 0);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d->tx_next = next;
    if (d->family == RTL_FAMILY_8125)
        rtl_write16(d, 0x90, 1);
    else
        rtl_write8(d, 0x38, 0x40);
    (void)rtl_read8(d, 0x37);
    kernel_spin_unlock_irqrestore(&d->tx_lock, flags);
    return 0;
}

/** @brief Copy a bounded complete frame and return its descriptor to hardware.
 * @param d Pinned active device. @param frame Writable capacity-byte buffer.
 * @param capacity Buffer capacity. @param length Optional size output, excluding FCS.
 * @return One, zero for no completion, or negative errno; malformed chains are dropped.
 * No allocation or hardware wait occurs while the IRQ-safe RX lock is held.
 */
int rtl_receive(struct rtl_device *d, void *frame, uint32_t capacity, uint32_t *length)
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
    uint32_t status = d->rx[index].command;
    if (status & RTL_DESC_OWN) {
        kernel_spin_unlock_irqrestore(&d->rx_lock, flags);
        return 0;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint32_t wire_length = status & 0x3fffu;
    uint32_t bytes = wire_length >= 4 ? wire_length - 4 : 0;
    uint32_t errors = RTL_RX_ERROR | (1u << 19) | (1u << 20) | (1u << 22);
    int rc = 1;
    if (wire_length > RTL_BUFFER_SIZE || bytes > RTL_MAX_FRAME || bytes > capacity)
        rc = -90;
    else if (wire_length < 18 || (status & errors) || d->dropping_fragments ||
             (status & (RTL_DESC_FIRST | RTL_DESC_LAST)) != (RTL_DESC_FIRST | RTL_DESC_LAST))
        rc = -74;
    d->dropping_fragments = !(status & RTL_DESC_LAST);
    if (rc == 1) {
        const uint8_t *input = d->rx_data + index * RTL_BUFFER_SIZE;
        uint8_t *output = frame;
        for (unsigned i = 0; i < bytes; ++i)
            output[i] = input[i];
        if (length)
            *length = bytes;
    }
    d->rx[index].options = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d->rx[index].command = RTL_DESC_OWN | RTL_BUFFER_SIZE |
        (index == RTL_RX_COUNT - 1 ? RTL_DESC_EOR : 0);
    d->rx_next = (index + 1) % RTL_RX_COUNT;
    kernel_spin_unlock_irqrestore(&d->rx_lock, flags);
    return rc;
}
