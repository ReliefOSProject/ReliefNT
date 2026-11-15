/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Issue a synchronous VMXNET3 command and read its completion.
 * @param d Mapped device. @param command Command word. @return Device's completion word.
 */
uint32_t vmx_command(struct vmx_device *d, uint32_t command)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->command_lock, &flags);
    vmx_io_write(d->bar[1], VMX_COMMAND, command);
    uint32_t result = vmx_io_read(d->bar[1], VMX_COMMAND);
    kernel_spin_unlock_irqrestore(&d->command_lock, flags);
    return result;
}

/** @brief Map, stop and negotiate the revision 1 / UPT 1 protocol.
 * @param d Selected PCI device. @return Zero or negative errno, retaining any partial resources.
 */
int vmx_hardware_start(struct vmx_device *d)
{
    const struct reliefos_driver_pci_device *p = &d->pci;
    d->original_command = d->api->pci_read16(p->bus, p->slot, p->function, 4);
    d->command_saved = true;
    d->api->pci_write16(p->bus, p->slot, p->function, 4, d->original_command & ~7u);
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t raw = d->api->pci_read32(p->bus, p->slot, p->function, (uint8_t)(0x10 + i * 4));
        /* This interface exposes two consecutive 32-bit memory BARs. */
        if ((raw & 7u) || !(raw & ~15u) || ((raw & ~15u) & 4095u))
            return -22;
        d->bar[i] = d->api->map_mmio(raw & ~15u, VMX_BAR_BYTES);
        if (!d->bar[i])
            return -12;
    }
    d->api->pci_write16(p->bus, p->slot, p->function, 4,
                        (d->original_command & ~7u) | 2u | (1u << 10));
    vmx_io_write(d->bar[0], VMX_IMR, 1);
    /* Until quiesced, the function may retain a previous owner's DMA state. */
    d->exposed = true;
    if (vmx_command(d, VMX_QUIESCE) || vmx_command(d, VMX_RESET))
        return -5;
    d->exposed = false;
    uint32_t revision = vmx_io_read(d->bar[1], VMX_REVISION);
    uint32_t upt = vmx_io_read(d->bar[1], VMX_UPT);
    if (revision == UINT32_MAX || upt == UINT32_MAX || !(revision & 1u) || !(upt & 1u))
        return -95;
    vmx_io_write(d->bar[1], VMX_REVISION, 1);
    vmx_io_write(d->bar[1], VMX_UPT, 1);
    uint32_t low = vmx_io_read(d->bar[1], VMX_MAC_LOW), high = vmx_io_read(d->bar[1], VMX_MAC_HIGH);
    for (unsigned i = 0; i < 4; ++i)
        d->mac[i] = (uint8_t)(low >> (8 * i));
    d->mac[4] = (uint8_t)high;
    d->mac[5] = (uint8_t)(high >> 8);
    unsigned any = 0;
    for (unsigned i = 0; i < 6; ++i)
        any |= d->mac[i];
    if (!any || (d->mac[0] & 1))
        return -22;
    return 0;
}

/** @brief Quiesce and reset synchronously, retaining backing on failed STOP.
 * @param d Context whose callbacks have already drained. @return Zero or -EIO.
 */
int vmx_hardware_stop(struct vmx_device *d)
{
    __atomic_store_n(&d->active,false,__ATOMIC_RELEASE);
    vmx_interrupt_stop(d);
    int failed = 0;
    if (d->exposed && d->bar[1]) {
        if (vmx_command(d, VMX_QUIESCE))
            failed = 1;
        if (!failed && vmx_command(d, VMX_RESET))
            failed = 1;
    }
    if (d->command_saved) {
        const struct reliefos_driver_pci_device *p = &d->pci;
        uint16_t value = d->api->pci_read16(p->bus, p->slot, p->function, 4);
        d->api->pci_write16(p->bus, p->slot, p->function, 4, value & ~4u);
        if (d->api->pci_read16(p->bus, p->slot, p->function, 4) & 4u)
            failed = 1;
    }
    if (failed)
        return -5;
    d->exposed = false;
    return 0;
}

/** @brief Read IRQ-cached carrier, or query the device in polling fallback.
 * @param d Device retained by an admitted callback. @return Nonzero if link is up.
 * Task context under network execution; IRQ events may refresh the cache.
 */
int vmx_link(struct vmx_device *d)
{
    if (!d->active || __atomic_load_n(&d->broken, __ATOMIC_RELAXED))
        return 0;
    if (d->irq_handle) {
        if (vmx_interrupt_events(d)) return 0;
        if (!d->link_valid) vmx_refresh_link(d);
    } else {
        vmx_refresh_link(d);
    }
    return d->link_up;
}

/** @brief Refresh task-owned carrier state with one serialized device query.
 * @param d Active device retained through the calling driver callback.
 */
void vmx_refresh_link(struct vmx_device *d)
{
    uint32_t link=vmx_command(d,VMX_GET_LINK);
    d->link_up=link!=UINT32_MAX && (link&1u)!=0;
    d->link_valid=true;
}
