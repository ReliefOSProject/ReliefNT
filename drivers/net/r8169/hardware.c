/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Enter PCI D0 through the bounded conventional capability chain.
 * @param d PCI device with bus mastering disabled.
 * @return Zero, or -EINVAL for a corrupt chain or unsupported header type.
 */
static int enter_d0(struct rtl_device *d)
{
    const struct reliefos_driver_pci_device *p = &d->pci;
    if ((p->header_type & 0x7f) != 0)
        return -22;
    if (!(d->api->pci_read16(p->bus, p->slot, p->function, 6) & 0x10))
        return 0;
    uint8_t next = d->api->pci_read32(p->bus, p->slot, p->function, 0x34) & 0xff;
    uint64_t visited = 0;
    while (next) {
        if (next < 0x40 || next > 0xfc || (next & 3))
            return -22;
        uint64_t bit = 1ULL << (next / 4);
        if (visited & bit)
            return -22;
        visited |= bit;
        uint16_t cap = d->api->pci_read16(p->bus, p->slot, p->function, next);
        if ((cap & 0xff) == 1) {
            if (next > 0xf8)
                return -22;
            uint16_t power = d->api->pci_read16(p->bus, p->slot, p->function, next + 4);
            /* PME status is W1C. Preserve it while requesting standard D0. */
            d->api->pci_write16(p->bus, p->slot, p->function, next + 4,
                                power & ~(0x8000u | 3u));
            rtl_delay_us(d, 10000);
            if (d->api->pci_read16(p->bus, p->slot, p->function, next + 4) & 3)
                return -5;
            return 0;
        }
        next = cap >> 8;
    }
    return 0;
}

/** @brief Map the preferred register BAR, trying other memory BARs when necessary.
 * @param d Bound PCI function retaining the successful MMIO lease.
 * @return Zero, -EINVAL for no usable memory BAR, or -ENOMEM if all leases fail.
 */
static int map_registers(struct rtl_device *d)
{
    const struct reliefos_driver_pci_device *p = &d->pci;
    uint64_t bases[6] = {0};
    uint16_t io_base = 0;
    for (unsigned index = 0; index < 6; ++index) {
        unsigned bar_index = index;
        unsigned offset = 0x10 + index * 4;
        uint32_t low = d->api->pci_read32(p->bus, p->slot, p->function, offset);
        if (!low || low == UINT32_MAX)
            continue;
        if (low & 1) {
            uint32_t port = low & ~3u;
            if (bar_index == 0 && port && !(port & 0xffu) && port <= 0xff00u)
                io_base = port;
            continue;
        }
        uint64_t base = low & ~0xfu;
        if ((low & 6) == 4) {
            if (index == 5)
                continue;
            base |= (uint64_t)d->api->pci_read32(p->bus, p->slot, p->function,
                                                offset + 4) << 32;
            ++index;
        } else if (low & 6) {
            continue;
        }
        if (!base || (base & 4095u) || base > UINT64_MAX - d->mmio_bytes)
            continue;
        bases[bar_index] = base;
    }
    /* BAR2 is the full PCIe register aperture on modern Realtek controllers. */
    static const unsigned preference[] = {2, 1, 0, 4, 3, 5};
    int rc = -22;
    for (unsigned i = 0; i < sizeof(preference) / sizeof(preference[0]); ++i) {
        uint64_t base = bases[preference[i]];
        if (!base)
            continue;
        d->registers = d->api->map_mmio(base, d->mmio_bytes);
        if (d->registers)
            return 0;
        rc = -12;
    }
    /* Older RTL8168 has a 256-byte control BAR. Its I/O BAR provides the
     * same registers without mapping pages outside the implemented aperture. */
    if (d->family == RTL_FAMILY_8168 && io_base && d->api->inb && d->api->outb &&
        d->api->inl && d->api->outl) {
        d->io_base = io_base;
        d->mmio_bytes = 0x100;
        return 0;
    }
    return rc;
}

/** @brief Discover a supported Realtek function and retain its MMIO/PCI state.
 * @param d Zeroed device containing validated kernel services.
 * @return Zero with identified hardware and valid station address, or negative errno.
 * Bus mastering remains disabled until rings have been fully allocated.
 */
int rtl_hardware_probe(struct rtl_device *d)
{
    static const uint16_t ids[] = {0x8168, 0x8125};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        if (!d->api->pci_find(0x10ec, ids[i], &d->pci)) {
            d->present = true;
            break;
        }
    }
    if (!d->present)
        return -19;
    const struct reliefos_driver_pci_device *p = &d->pci;
    if (p->class_code != 2 || p->subclass != 0)
        return -19;
    d->family = p->device_id == 0x8125 ? RTL_FAMILY_8125 : RTL_FAMILY_8168;
    d->mmio_bytes = d->family == RTL_FAMILY_8125 ? 0x10000 : 0x1000;
    d->original_command = d->api->pci_read16(p->bus, p->slot, p->function, 4);
    d->command_saved = true;
    d->api->pci_write16(p->bus, p->slot, p->function, 4,
                        (d->original_command & ~7u) | (1u << 10));
    int rc = enter_d0(d);
    if (rc)
        return rc;
    rc = map_registers(d);
    if (rc)
        return rc;
    d->api->pci_write16(p->bus, p->slot, p->function, 4,
                        (d->original_command & ~7u) | (d->io_base ? 1u : 2u) | (1u << 10));
    d->xid = rtl_read32(d, 0x40) & 0x7cf00000u;
    rc = d->family == RTL_FAMILY_8125 ? rtl8125_identify(d) : rtl8168_identify(d);
    if (rc) {
        d->api->console_log(CONSOLE_LOG_WARN,
            "[r8169] unsupported Realtek %04x:%04x XID=%08x\n",
            p->vendor_id, p->device_id, d->xid);
        return rc;
    }
    /* The firmware-loaded station address is retained before MAC reset. */
    unsigned mac_offset = d->family == RTL_FAMILY_8125 ? 0x19e0 : 0;
    uint32_t low = rtl_read32(d, mac_offset);
    uint16_t high = d->family == RTL_FAMILY_8125 ? rtl_read16(d, mac_offset + 4) :
        (uint16_t)rtl_read32(d, mac_offset + 4);
    for (unsigned i = 0; i < 4; ++i)
        d->mac[i] = low >> (i * 8);
    d->mac[4] = high;
    d->mac[5] = high >> 8;
    unsigned any = 0;
    for (unsigned i = 0; i < 6; ++i)
        any |= d->mac[i];
    if (!any || (d->mac[0] & 1))
        return -5;
    return 0;
}

/** @brief Confirm controller reset and close PCI bus-master admission.
 * @param d Device with callback users drained; partial probe is accepted.
 * @return Zero if allocations may be released, or negative errno retaining them.
 */
int rtl_hardware_stop(struct rtl_device *d)
{
    d->active = false;
    int rc = 0;
    if ((d->registers || d->io_base) && d->chip_name) {
        /* Clear a previous initialization timeout before attempting recovery. */
        d->io_error = 0;
        rc = d->family == RTL_FAMILY_8125 ? rtl8125_stop(d) : rtl8168_stop(d);
    }
    if (d->command_saved) {
        const struct reliefos_driver_pci_device *p = &d->pci;
        uint16_t command = d->api->pci_read16(p->bus, p->slot, p->function, 4);
        d->api->pci_write16(p->bus, p->slot, p->function, 4, command & ~4u);
        if (d->api->pci_read16(p->bus, p->slot, p->function, 4) & 4)
            rc = -5;
    }
    return rc;
}
