/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/time.h>

/** @brief Service status and completed TX DMA without waiting for queue progress.
 * @param d Live controller under its device lock.
 * @return Zero, one after a successful queue reset, or negative hardware errno.
 */
static int service(struct r8139_device *d)
{
    uint16_t status = r8139_read16(d, R8139_ISR);
    if (status == UINT16_MAX || (status & R8139_PCI_ERROR)) {
        r8139_fault(d);
        return -5;
    }
    if (status & R8139_RX_OVERFLOW) {
        int rc = r8139_recover(d);
        return rc ? rc : 1;
    }
    if (status)
        r8139_write16(d, R8139_ISR, status);
    uint64_t now = d->api->ticks();
    for (unsigned i = 0; i < R8139_TX_COUNT; ++i) {
        if (!d->tx_pending[i])
            continue;
        uint32_t state = r8139_read32(d, R8139_TSD + i * 4);
        if (state == UINT32_MAX) {
            r8139_fault(d);
            return -5;
        }
        if ((state & R8139_TX_HOST) &&
            (state & (R8139_TX_OK | R8139_TX_UNDERRUN | R8139_TX_ABORT))) {
            if (state & (R8139_TX_UNDERRUN | R8139_TX_ABORT))
                ++d->tx_errors;
            if (state & R8139_TX_ABORT)
                r8139_write32(d, R8139_TCR, R8139_TX_CONFIG | 1u);
            d->tx_pending[i] = false;
        } else if (now - d->tx_submitted[i] >= 5u * RELIEFNT_TICK_HZ) {
            int rc = r8139_recover(d);
            return rc ? rc : 1;
        }
    }
    return 0;
}

/** @brief Submit one Ethernet frame to a hardware-owned TX buffer.
 * @param d Callback-pinned device. @param frame Borrowed bytes excluding FCS.
 * @param length Byte count. @return Zero or negative errno; never waits for TX.
 */
int r8139_send(struct r8139_device *d, const void *frame, uint32_t length)
{
    if (!frame || length < 14 || length > R8139_FRAME_MAX)
        return -22;
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->lock, &flags);
    int rc = -100;
    if (!d->active || d->fault || (r8139_read8(d, R8139_MEDIA) & R8139_LINK_BAD))
        goto done;
    rc = service(d);
    if (rc) {
        if (rc > 0)
            rc = -11;
        goto done;
    }
    unsigned index = d->tx_next;
    if (d->tx_pending[index]) {
        rc = -11;
        goto done;
    }
    uint8_t *buffer = d->tx + index * R8139_TX_BYTES;
    const uint8_t *source = frame;
    unsigned wire_length = length < 60 ? 60 : length;
    for (unsigned i = 0; i < wire_length; ++i)
        buffer[i] = i < length ? source[i] : 0;
    d->tx_pending[index] = true;
    d->tx_submitted[index] = d->api->ticks();
    __atomic_thread_fence(__ATOMIC_RELEASE);
    /* Wait for the complete frame in the FIFO before transmission; hardware
     * appends FCS. The TSD write transfers this buffer to the device. */
    r8139_write32(d, R8139_TSD + index * 4, (63u << 16) | wire_length);
    d->tx_next = (index + 1) % R8139_TX_COUNT;
    rc = 0;
done:
    kernel_spin_unlock_irqrestore(&d->lock, flags);
    return rc;
}

/** @brief Receive a validated classic-ring record and return its storage via CAPR.
 * @param d Callback-pinned device. @param frame Writable output.
 * @param capacity Output bytes. @param length Optional returned frame length.
 * @return One frame, zero if empty/in-flight, or negative errno after a drop/error.
 */
int r8139_receive(struct r8139_device *d, void *frame, uint32_t capacity, uint32_t *length)
{
    if (length)
        *length = 0;
    if (!frame || !capacity)
        return -22;
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->lock, &flags);
    int rc = -100;
    if (!d->active || d->fault)
        goto done;
    rc = service(d);
    if (rc) {
        if (rc > 0)
            rc = -74;
        goto done;
    }
    uint8_t command = r8139_read8(d, R8139_COMMAND);
    if (command == UINT8_MAX) {
        r8139_fault(d);
        rc = -5;
        goto done;
    }
    rc = 0;
    if (command & R8139_CMD_EMPTY)
        goto done;
    /* rx_next is dword-aligned, so the four-byte header never straddles the
     * physical ring end. The packet body can wrap and is copied modulo size. */
    uint32_t header = *(volatile uint32_t *)(d->rx + d->rx_next);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    unsigned wire_length = header >> 16;
    if (wire_length == 0xfff0u)
        goto done; /* Hardware has not finished this DMA record. */
    if (wire_length < 64 || wire_length > R8139_FRAME_MAX + 4) {
        rc = r8139_recover(d);
        if (!rc)
            rc = -74;
        goto done;
    }
    unsigned packet_length = wire_length - 4; /* Strip the received FCS. */
    if (!(header & R8139_RX_OK) || (header & R8139_RX_ERRORS)) {
        rc = -74;
    } else if (packet_length > capacity) {
        rc = -90;
    } else {
        uint8_t *output = frame;
        for (unsigned i = 0; i < packet_length; ++i)
            output[i] = d->rx[(d->rx_next + 4 + i) & (R8139_RX_BYTES - 1)];
        if (length)
            *length = packet_length;
        rc = 1;
    }
    d->rx_next = ((d->rx_next + wire_length + 7u) & ~3u) & (R8139_RX_BYTES - 1);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r8139_write16(d, R8139_CAPR, (uint16_t)(d->rx_next - 16));
done:
    kernel_spin_unlock_irqrestore(&d->lock, flags);
    return rc;
}
