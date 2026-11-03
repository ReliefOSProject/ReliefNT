/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Wait with a millisecond bound while initialization owns the device.
 * @param d Live MMIO device. @param reg Register offset. @param mask Bits to inspect.
 * @param want Required masked value. @param milliseconds Maximum sleeps.
 * @return Zero for matching bits, -ETIMEDOUT otherwise; task context, no spinlock held.
 */
static int wait_bits(struct e1k_device *d, unsigned reg, uint32_t mask,
                     uint32_t want, unsigned milliseconds)
{
    for (unsigned i=0;;++i) {
        if ((e1k_read(d,reg)&mask)==want) return 0;
        if (i==milliseconds) return -110;
        d->api->sleep_ms(1);
    }
}

/** @brief Discover, map and reset an explicitly supported PCIe controller.
 * @param d Zeroed state with validated services; caller retains partial state.
 * @return Zero, -ENODEV, -EINVAL, -ENOMEM or -ETIMEDOUT; no DMA buffers are allocated.
 */
int e1k_hardware_start(struct e1k_device *d)
{
    static const uint16_t devices[]={0x10d3,0x150c};
    for (unsigned i=0;i<sizeof(devices)/sizeof(devices[0]);++i)
        if (!d->api->pci_find(0x8086,devices[i],&d->pci)) { d->present=true; break; }
    if (!d->present) return -19;
    const struct reliefos_driver_pci_device *p=&d->pci;
    d->original_command=d->api->pci_read16(p->bus,p->slot,p->function,4);
    d->command_saved=true;
    d->api->pci_write16(p->bus,p->slot,p->function,4,d->original_command&~7u);
    uint32_t low=d->api->pci_read32(p->bus,p->slot,p->function,0x10);
    if ((low&1) || ((low&6)!=0 && (low&6)!=4)) return -22;
    uint64_t address=low&~0xfu;
    if ((low&6)==4)
        address|=(uint64_t)d->api->pci_read32(p->bus,p->slot,p->function,0x14)<<32;
    if (!address || (address&4095)) return -22;
    d->registers=d->api->map_mmio(address,E1K_MMIO_BYTES);
    if (!d->registers) return -12;
    d->api->pci_write16(p->bus,p->slot,p->function,4,
                        (d->original_command&~7u)|2u|(1u<<10));
    /* EEPROM may still be loading after PCI D0 entry. All waits are bounded. */
    d->api->sleep_ms(10);
    e1k_write(d,E1K_IMC,0xffffffffu);
    e1k_write(d,E1K_RCTL,0);
    e1k_write(d,E1K_TCTL,0);
    e1k_write(d,E1K_CTRL,e1k_read(d,E1K_CTRL)|E1K_CTRL_GIO_DISABLE);
    e1k_flush(d);
    int rc=wait_bits(d,E1K_STATUS,E1K_STATUS_MASTER,0,100);
    if (rc) return rc;
    e1k_write(d,E1K_CTRL,e1k_read(d,E1K_CTRL)|E1K_CTRL_RST);
    e1k_flush(d);
    d->api->sleep_ms(20);
    rc=wait_bits(d,E1K_CTRL,E1K_CTRL_RST,0,100);
    if (rc) return rc;
    e1k_write(d,E1K_IMC,0xffffffffu);
    (void)e1k_read(d,E1K_ICR);
    rc=wait_bits(d,E1K_EECD,1u<<9,1u<<9,1000);
    if (rc) return rc;
    e1k_write(d,E1K_GCR,e1k_read(d,E1K_GCR)|(1u<<22));
    /* No pause offload is advertised: protocol checksums remain in software. */
    e1k_write(d,E1K_FCAL,0); e1k_write(d,E1K_FCAH,0); e1k_write(d,E1K_FCT,0);
    return e1k_setup_link(d);
}

/** @brief Stop all ring DMA before backing or MMIO can be released.
 * @param d Retained device after callback admission closes and existing callbacks drain.
 * @return Zero if hardware stopped, -ETIMEDOUT if any backing must remain retained.
 */
int e1k_hardware_stop(struct e1k_device *d)
{
    d->active=false;
    if (!d->registers) return 0;
    e1k_write(d,E1K_IMC,0xffffffffu);
    e1k_write(d,E1K_RCTL,0); e1k_write(d,E1K_TCTL,0);
    e1k_write(d,E1K_CTRL_EXT,e1k_read(d,E1K_CTRL_EXT)&~E1K_DRV_LOAD);
    e1k_write(d,E1K_CTRL,e1k_read(d,E1K_CTRL)|E1K_CTRL_GIO_DISABLE);
    e1k_flush(d);
    d->api->sleep_ms(10);
    const struct reliefos_driver_pci_device *p=&d->pci;
    uint16_t command=d->api->pci_read16(p->bus,p->slot,p->function,4);
    d->api->pci_write16(p->bus,p->slot,p->function,4,command&~4u);
    (void)d->api->pci_read16(p->bus,p->slot,p->function,4);
    int rc=wait_bits(d,E1K_STATUS,E1K_STATUS_MASTER,0,100);
    if (rc) return rc;
    e1k_write(d,E1K_CTRL,e1k_read(d,E1K_CTRL)|E1K_CTRL_RST);
    e1k_flush(d);
    d->api->sleep_ms(20);
    return wait_bits(d,E1K_CTRL,E1K_CTRL_RST,0,100);
}
