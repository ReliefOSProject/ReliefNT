/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

#define PCI_IO 0x0001u
#define PCI_MASTER 0x0004u
#define PCI_INTX_DISABLE 0x0400u

/** @brief Close PCI DMA admission and verify the same function accepted it.
 * @param d Device retaining its original PCI command.
 * @return Zero or negative EIO; no wait, allocation or resource release.
 */
static int disable_master(struct r8139_device *d)
{
    const struct reliefos_driver_pci_device *p = &d->pci;
    uint16_t command = d->api->pci_read16(p->bus, p->slot, p->function, 4);
    if (command == UINT16_MAX)
        return -5;
    d->api->pci_write16(p->bus, p->slot, p->function, 4,
                        (command & ~PCI_MASTER) | PCI_INTX_DISABLE);
    command = d->api->pci_read16(p->bus, p->slot, p->function, 4);
    return (command == UINT16_MAX || (command & PCI_MASTER)) ? -5 : 0;
}

/** @brief Request D0 through a validated, bounded PCI capability list.
 * @param d Device with bus mastering closed. @return Zero or negative errno.
 * Initialization task only, with no device lock held; D3 settling may sleep.
 */
static int enter_d0(struct r8139_device *d)
{
    const struct reliefos_driver_pci_device *p = &d->pci;
    if (!(d->api->pci_read16(p->bus, p->slot, p->function, 6) & 0x10))
        return 0;
    unsigned next = d->api->pci_read32(p->bus, p->slot, p->function, 0x34) & 0xff;
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
            if (power & 3) {
                /* Do not acknowledge the unrelated PME W1C status. */
                d->api->pci_write16(p->bus, p->slot, p->function, next + 4,
                                    power & ~(0x8000u | 3u));
                d->api->sleep_ms(10);
                if (d->api->pci_read16(p->bus, p->slot, p->function, next + 4) & 3)
                    return -5;
            }
        }
        next = cap >> 8;
    }
    return 0;
}

/** @brief Reset both DMA engines with a bounded completion wait.
 * @param d Retained I/O window, bus mastering disabled.
 * @param sleepable True only for init/fini without a device lock.
 * @return Zero or negative timeout/EIO; a failed reset never releases DMA.
 */
static int reset(struct r8139_device *d, bool sleepable)
{
    r8139_write16(d, R8139_IMR, 0);
    r8139_write8(d, R8139_COMMAND, R8139_CMD_RESET);
    unsigned attempts = sleepable ? 100u : 10000u;
    for (unsigned i = 0; i < attempts; ++i) {
        uint8_t command = r8139_read8(d, R8139_COMMAND);
        if (command == UINT8_MAX)
            return -5;
        if (!(command & R8139_CMD_RESET)) {
            r8139_write16(d, R8139_IMR, 0);
            r8139_write16(d, R8139_ISR, UINT16_MAX);
            return 0;
        }
        if (sleepable)
            d->api->sleep_ms(1);
        else
            __asm__ volatile("pause" : : : "memory");
    }
    return -110;
}

/** @brief Select the exact RTL8139 I/O BAR and leave both engines reset.
 * @param d Fresh device context with validated services.
 * @return Zero or negative errno, retaining partial PCI state for cleanup.
 */
int r8139_probe(struct r8139_device *d)
{
    const struct reliefos_driver_pci_device *p = &d->pci;
    if ((p->header_type & 0x7f) != 0)
        return -19;
    d->original_command = d->api->pci_read16(p->bus, p->slot, p->function, 4);
    if (d->original_command == UINT16_MAX)
        return -19;
    d->command_saved = true;
    int rc = disable_master(d);
    if (rc)
        return rc;
    rc = enter_d0(d);
    if (rc)
        return rc;
    uint32_t bar = d->api->pci_read32(p->bus, p->slot, p->function, 0x10);
    uint32_t port = bar & ~3u;
    if (!(bar & 1) || !port || (port & 0xffu) || port > 0xff00u)
        return -19;
    d->api->pci_write16(p->bus, p->slot, p->function, 4,
                        (d->original_command & ~PCI_MASTER) | PCI_IO | PCI_INTX_DISABLE);
    if (!(d->api->pci_read16(p->bus, p->slot, p->function, 4) & PCI_IO))
        return -5;
    d->io_base = port;
    r8139_write16(d, R8139_IMR, 0);
    r8139_write8(d, R8139_CFG9346, 0xc0);
    r8139_write8(d, R8139_CONFIG1, r8139_read8(d, R8139_CONFIG1) & ~3u);
    r8139_write8(d, R8139_CFG9346, 0);
    rc = reset(d, true);
    if (rc)
        return rc;
    unsigned any = 0;
    for (unsigned i = 0; i < 6; ++i) {
        d->mac[i] = r8139_read8(d, R8139_MAC + i);
        any |= d->mac[i];
    }
    return (!any || (d->mac[0] & 1)) ? -5 : 0;
}

/** @brief Allocate a DMA run and reject truncated or unmappable bus addresses.
 * @param d Quiesced device. @param pages Run length in 4 KiB pages.
 * @param physical Receives the owned physical run, including on mapping failure.
 * @param alias Receives the CPU mapping on success.
 * @return Zero, negative ENOMEM or EIO; the caller owns all returned leases.
 */
static int allocate(struct r8139_device *d, unsigned pages, uint64_t *physical, uint8_t **alias)
{
    *physical = d->api->alloc_dma(pages, UINT32_MAX);
    if (!*physical)
        return -12;
    if ((*physical & 4095u) || *physical > UINT32_MAX - (uint64_t)pages * 4096 + 1)
        return -5;
    *alias = d->api->phys_to_virt(*physical);
    return *alias ? 0 : -12;
}

/** @brief Retain complete DMA32 RX and TX allocations before enabling either engine.
 * @param d Reset controller. @return Zero or negative errno with partial leases retained.
 */
int r8139_allocate(struct r8139_device *d)
{
    int rc = allocate(d, R8139_RX_PAGES, &d->rx_pa, &d->rx);
    return rc ? rc : allocate(d, R8139_TX_PAGES, &d->tx_pa, &d->tx);
}

/** @brief Reinitialize classic-mode registers from complete, quiesced DMA leases.
 * @param d Exclusively owned reset controller. @return Zero or negative EIO.
 * No sleep or allocation; callable during a lock-held recovery.
 */
int r8139_activate(struct r8139_device *d)
{
    const struct reliefos_driver_pci_device *p = &d->pci;
    d->rx_next = d->tx_next = 0;
    for (unsigned i = 0; i < R8139_RX_PAGES * 4096; ++i)
        d->rx[i] = 0;
    for (unsigned i = 0; i < R8139_TX_COUNT; ++i) {
        d->tx_pending[i] = false;
        r8139_write32(d, R8139_TSAD + i * 4, d->tx_pa + i * R8139_TX_BYTES);
    }
    r8139_write32(d, R8139_RBSTART, d->rx_pa);
    r8139_write32(d, R8139_MAR, UINT32_MAX);
    r8139_write32(d, R8139_MAR + 4, UINT32_MAX);
    r8139_write32(d, R8139_MISSED, 0);
    r8139_write16(d, R8139_IMR, 0);
    r8139_write16(d, R8139_ISR, UINT16_MAX);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint16_t command = d->api->pci_read16(p->bus, p->slot, p->function, 4);
    d->api->pci_write16(p->bus, p->slot, p->function, 4,
                        command | PCI_IO | PCI_MASTER | PCI_INTX_DISABLE);
    command = d->api->pci_read16(p->bus, p->slot, p->function, 4);
    if (command == UINT16_MAX || (command & (PCI_IO | PCI_MASTER)) != (PCI_IO | PCI_MASTER))
        return -5;
    /* The receive configuration is writable once RX/TX have been enabled. */
    r8139_write8(d, R8139_COMMAND, R8139_CMD_RX | R8139_CMD_TX);
    r8139_write32(d, R8139_TCR, R8139_TX_CONFIG);
    r8139_write32(d, R8139_RCR, R8139_RX_CONFIG);
    r8139_write16(d, R8139_CAPR, (uint16_t)-16);
    command = r8139_read8(d, R8139_COMMAND);
    return (command == UINT8_MAX ||
            (command & (R8139_CMD_RX | R8139_CMD_TX)) != (R8139_CMD_RX | R8139_CMD_TX)) ? -5 : 0;
}

/** @brief Close packet admission and DMA without freeing unproven resources.
 * @param d Callback-pinned controller under its device lock.
 */
void r8139_fault(struct r8139_device *d)
{
    d->fault = true;
    r8139_write16(d, R8139_IMR, 0);
    r8139_write8(d, R8139_COMMAND, 0);
    (void)disable_master(d);
}

/** @brief Recover lost progress by resetting both engines before DMA reuse.
 * @param d Callback-pinned device under its lock. @return Zero or negative errno.
 */
int r8139_recover(struct r8139_device *d)
{
    int rc = disable_master(d);
    if (!rc)
        rc = reset(d, false);
    if (!rc)
        rc = r8139_activate(d);
    if (rc)
        r8139_fault(d);
    else
        ++d->recoveries;
    return rc;
}

/** @brief Prove both controller reset and bus-master disable before teardown.
 * @param d Context after callback drain, including partial initialization.
 * @return Zero on safe STOP; negative errno retains device and all DMA leases.
 */
int r8139_stop(struct r8139_device *d)
{
    d->active = false;
    int rc = d->command_saved ? disable_master(d) : 0;
    if (d->io_base && !rc) {
        int reset_rc = reset(d, true);
        if (!rc)
            rc = reset_rc;
    }
    if (!rc && d->command_saved)
        rc = disable_master(d);
    return rc;
}

/** @brief Release the exact DMA leases only after the caller proved STOP.
 * @param d Stopped device; fields are cleared to make partial cleanup idempotent.
 */
void r8139_free(struct r8139_device *d)
{
    if (d->rx_pa)
        d->api->free_dma(d->rx_pa, R8139_RX_PAGES);
    if (d->tx_pa)
        d->api->free_dma(d->tx_pa, R8139_TX_PAGES);
    d->rx_pa = d->tx_pa = 0;
    d->rx = d->tx = NULL;
}
