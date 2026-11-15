/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/net_device.h>
#include <reliefnt/pci_irq.h>

/** @brief Mask one message source and coalesce task work without parsing packets.
 * @param context IRQ-pinned device; no allocation or protocol callback is allowed.
 */
static void interrupt(void *context)
{
    struct vmx_device *d=context;
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->irq_lock,&flags);
    vmx_io_write(d->bar[0],VMX_IMR,1);
    (void)vmx_io_read(d->bar[0],VMX_IMR);
    d->irq_masked=true;
    __atomic_store_n(&d->event_pending,1,__ATOMIC_RELEASE);
    bool notify=__atomic_load_n(&d->active,__ATOMIC_ACQUIRE) && d->ifindex;
    kernel_spin_unlock_irqrestore(&d->irq_lock,flags);
    if (notify) netdev_rx_notify(d->ifindex);
}

/** @brief Negotiate a single MSI-X/MSI message, preserving a poll-only fallback.
 * @param d Quiesced device with complete BAR mappings. Init task context only.
 */
void vmx_interrupt_prepare(struct vmx_device *d)
{
    d->irq_masked=true;
    if (!d->api->free_pci_irq) return;
    uint32_t config=vmx_command(d,VMX_GET_INTR_CONFIG), type=config&3u;
    d->auto_mask=((config>>2)&3u)!=1u;
    if (config==UINT32_MAX || type==1u) return; /* No speculative shared INTx. */
    int result=-95;
    if (type!=2u) {
        result=driver_request_pci_msix_irq(&d->pci,interrupt,d,&d->irq_handle);
        if (!result) d->irq_mode=1;
    }
    if (result && d->api->request_pci_irq) {
        result=d->api->request_pci_irq(&d->pci,interrupt,d,&d->irq_handle);
        if (!result) d->irq_mode=2;
    }
    if (result) d->irq_handle=0;
}

/** @brief Stop message production and wait for all retained IRQ callbacks.
 * @param d Context with network callback admission drained, or partial init.
 * Task context; no spinlock spans the provider's callback synchronization.
 */
void vmx_interrupt_stop(struct vmx_device *d)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->irq_lock,&flags);
    if (d->bar[0]) {
        vmx_io_write(d->bar[0],VMX_IMR,1);
        (void)vmx_io_read(d->bar[0],VMX_IMR);
    }
    uint32_t handle=d->irq_handle;
    d->irq_handle=0;
    d->irq_masked=true;
    kernel_spin_unlock_irqrestore(&d->irq_lock,flags);
    if (handle) d->api->free_pci_irq(handle);
}

/** @brief Acknowledge events in task context, updating carrier or closing bad queues.
 * @param d Active, callback-pinned device. @return Zero or negative EIO.
 * A new IRQ may set a successor hint while this one is consumed. Queue errors
 * remain masked and stop data admission instead of reusing untrusted buffers.
 */
int vmx_interrupt_events(struct vmx_device *d)
{
    if (d->irq_handle && __atomic_exchange_n(&d->event_pending,0,__ATOMIC_ACQ_REL)) {
        uint32_t events=vmx_io_read(d->bar[1],VMX_EVENT_CAUSE);
        if (events && events!=UINT32_MAX) {
            vmx_io_write(d->bar[1],VMX_EVENT_CAUSE,events);
            if (events&4u) vmx_refresh_link(d);
        }
        if (events==UINT32_MAX || (events&0x0bu)) {
            __atomic_store_n(&d->broken,1,__ATOMIC_RELAXED);
            d->api->console_log(CONSOLE_LOG_WARN,"[vmxnet3] %02x:%02x.%u queue/device event=%08x; queues stopped\n",
                d->pci.bus,d->pci.slot,d->pci.function,events);
        }
    }
    return __atomic_load_n(&d->broken,__ATOMIC_RELAXED) ? -5 : 0;
}

/** @brief Rearm an empty queue, then close the empty-check/unmask race.
 * @param d Active retained device; caller holds rx_lock and has serviced TX.
 * IRQ lock serializes IMR writes with ISR masking. Coherent generation reads
 * after the posted unmask ensure a raced completion receives another turn.
 */
void vmx_interrupt_complete(struct vmx_device *d)
{
    if (!d->irq_handle || __atomic_load_n(&d->broken,__ATOMIC_RELAXED)) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->irq_lock,&flags);
    bool pending=false;
    if (d->active && d->irq_masked) {
        vmx_io_write(d->bar[0],VMX_IMR,0);
        (void)vmx_io_read(d->bar[0],VMX_IMR);
        d->irq_masked=false;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        pending=(d->rxc[d->rxc_next].b>>31)==d->rxc_gen;
        if (pending) {
            vmx_io_write(d->bar[0],VMX_IMR,1);
            (void)vmx_io_read(d->bar[0],VMX_IMR);
            d->irq_masked=true;
        }
    }
    kernel_spin_unlock_irqrestore(&d->irq_lock,flags);
    if (pending) netdev_rx_notify(d->ifindex);
}
