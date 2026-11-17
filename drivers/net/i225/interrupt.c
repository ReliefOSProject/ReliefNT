/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/net_device.h>

static void i225_irq(void *context)
{
    struct i225_device *d = context;
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->irq_lock, &flags);
    i225_write(d, I225_IMC, I225_IRQ_MASK);
    i225_flush(d);
    (void)i225_read(d, I225_ICR);
    d->irq_masked = true;
    uint32_t ifindex = d->ifindex;
    bool notify = ifindex != 0;
    kernel_spin_unlock_irqrestore(&d->irq_lock, flags);
    if (notify)
        netdev_rx_notify(ifindex);
}

void i225_interrupt_prepare(struct i225_device *d)
{
    d->irq_masked = true;
    if (d->api->struct_size < I225_PCI_IRQ_API_SIZE ||
        !d->api->request_pci_irq || !d->api->free_pci_irq)
        return;
    uint32_t handle = 0;
    if (!d->api->request_pci_irq(&d->pci, i225_irq, d, &handle))
        d->irq_handle = handle;
}

void i225_interrupt_start(struct i225_device *d, uint32_t ifindex)
{
    if (!d->irq_handle || !ifindex)
        return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->irq_lock, &flags);
    d->ifindex = ifindex;
    i225_write(d, I225_IMC, I225_IRQ_MASK);
    (void)i225_read(d, I225_ICR);
    i225_write(d, I225_IMS, I225_IRQ_MASK);
    i225_flush(d);
    d->irq_masked = false;
    kernel_spin_unlock_irqrestore(&d->irq_lock, flags);
    netdev_rx_notify(ifindex);
}

void i225_interrupt_complete(struct i225_device *d)
{
    if (!d->irq_handle || !d->rx)
        return;
    uint32_t notify_ifindex = 0;
    uint64_t rx_flags, irq_flags;
    kernel_spin_lock_irqsave(&d->rx_lock, &rx_flags);
    kernel_spin_lock_irqsave(&d->irq_lock, &irq_flags);
    if (d->active && d->irq_masked) {
        i225_write(d, I225_IMS, I225_IRQ_MASK);
        i225_flush(d);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        uint32_t cause = i225_read(d, I225_ICR);
        bool packet_ready = d->rx[d->rx_next].writeback.status_error & I225_RX_DD;
        d->irq_masked = false;
        if (packet_ready) {
            i225_write(d, I225_IMC, I225_IRQ_MASK);
            i225_flush(d);
            (void)i225_read(d, I225_ICR);
            d->irq_masked = true;
            notify_ifindex = d->ifindex;
        } else if (cause & I225_IRQ_MASK) {
            notify_ifindex = d->ifindex;
        }
    }
    kernel_spin_unlock_irqrestore(&d->irq_lock, irq_flags);
    kernel_spin_unlock_irqrestore(&d->rx_lock, rx_flags);
    if (notify_ifindex)
        netdev_rx_notify(notify_ifindex);
}

void i225_interrupt_stop(struct i225_device *d)
{
    if (!d->irq_handle)
        return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->irq_lock, &flags);
    d->ifindex = 0;
    i225_write(d, I225_IMC, UINT32_MAX);
    i225_flush(d);
    (void)i225_read(d, I225_ICR);
    d->irq_masked = true;
    uint32_t handle = d->irq_handle;
    d->irq_handle = 0;
    kernel_spin_unlock_irqrestore(&d->irq_lock, flags);
    d->api->free_pci_irq(handle);
}
