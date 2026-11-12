/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

static struct rtl_device device;

/** @brief Observe the hardware's current link indication without MDIO waits.
 * @return Nonzero only for an active bound controller with a copper link.
 */
static int ready(void)
{
    return device.active && (rtl_read8(&device, 0x6c) & 2);
}

/** @brief Borrow the immutable station address while the caller holds its owner pin.
 * @return Device-owned six-byte station address, retained until callback drain.
 */
static const uint8_t *mac(void)
{
    return device.mac;
}

/** @brief Submit a frame through the retained single-interface network service.
 * @param frame Borrowed Ethernet bytes. @param length Size excluding FCS.
 * @return Zero on admission or negative Linux errno without waiting.
 */
static int send(const void *frame, uint32_t length)
{
    return rtl_send(&device, frame, length);
}

/** @brief Consume one descriptor through the retained network service.
 * @param frame Writable output. @param capacity Buffer capacity. @param length Optional size output.
 * @return One for a frame, zero if empty, or negative Linux errno.
 */
static int receive(void *frame, uint32_t capacity, uint32_t *length)
{
    return rtl_receive(&device, frame, capacity, length);
}

/** @brief Report the actual PCI identity, link state and station address.
 * @param out Nullable caller-owned output; no private pointer crosses the interface.
 */
static void info(struct reliefos_driver_e1000_info *out)
{
    if (!out)
        return;
    *out = (struct reliefos_driver_e1000_info){
        .present = device.present, .active = ready() != 0,
        .vendor_id = device.pci.vendor_id, .device_id = device.pci.device_id,
        .bus = device.pci.bus, .slot = device.pci.slot, .function = device.pci.function
    };
    for (unsigned i = 0; i < 6; ++i)
        out->mac[i] = device.mac[i];
}

/** @brief Initialize a supported Realtek MAC and publish its owner-pinned callbacks.
 * @param api Size/version-qualified services borrowed for the binding's lifetime.
 * @return Zero or negative Linux errno; partial resources remain for mandatory fini.
 */
static int start(const struct reliefos_driver_kernel_api *api)
{
    static const struct reliefos_driver_e1000_ops ops = {ready, mac, send, receive, info};
    if (!api || api->abi_version != RELIEFOS_DRIVER_ABI_VERSION ||
        api->struct_size < RELIEFOS_DRIVER_PHYS_MAP_API_SIZE ||
        !api->pci_find || !api->pci_read16 || !api->pci_write16 || !api->pci_read32 ||
        !api->map_mmio || !api->unmap_mmio || !api->alloc_dma || !api->free_dma ||
        !api->phys_to_virt || !api->sleep_ms || !api->register_e1000 ||
        !api->console_log || !api->report_teardown_failure)
        return -22;
    device = (struct rtl_device){.api = api};
    int rc = rtl_hardware_probe(&device);
    if (rc)
        return rc;
    rc = rtl_hardware_stop(&device);
    if (rc)
        return rc;
    rc = rtl_allocate_rings(&device);
    if (rc)
        return rc;
    const struct reliefos_driver_pci_device *p = &device.pci;
    uint16_t command = api->pci_read16(p->bus, p->slot, p->function, 4);
    api->pci_write16(p->bus, p->slot, p->function, 4, command | 4u);
    if (!(api->pci_read16(p->bus, p->slot, p->function, 4) & 4))
        return -5;
    rc = device.family == RTL_FAMILY_8125 ? rtl8125_start(&device) : rtl8168_start(&device);
    if (rc)
        return rc;
    device.active = true;
    rc = api->register_e1000(&ops);
    if (rc)
        return rc;
    api->console_log(CONSOLE_LOG_INFO,
        "[r8169] %02x:%02x.%u %s %04x:%04x XID=%08x MAC=%02x:%02x:%02x:%02x:%02x:%02x; link=%s\n",
        p->bus, p->slot, p->function, device.chip_name, p->vendor_id, p->device_id, device.xid,
        device.mac[0], device.mac[1], device.mac[2], device.mac[3], device.mac[4], device.mac[5],
        ready() ? "up" : "negotiating/down");
    return 0;
}

/** @brief Stop and drain hardware before relinquishing owned MMIO and DMA backing.
 * A failed reset retains the resources and module through the manager's retry contract.
 * Called only after callback admission closes and all outstanding users drain.
 */
static void stop(void)
{
    if (!device.api)
        return;
    int rc = rtl_hardware_stop(&device);
    if (rc) {
        device.api->report_teardown_failure(rc);
        return;
    }
    rtl_free_rings(&device);
    if (device.registers)
        device.api->unmap_mmio((void *)device.registers, device.mmio_bytes);
    if (device.command_saved) {
        const struct reliefos_driver_pci_device *p = &device.pci;
        device.api->pci_write16(p->bus, p->slot, p->function, 4,
                                device.original_command & ~4u);
    }
    device = (struct rtl_device){0};
}

const struct reliefos_driver_module r8169_driver_module = {
    .magic = RELIEFOS_DRIVER_MODULE_MAGIC,
    .abi_version = RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size = sizeof(struct reliefos_driver_module),
    .kind = RELIEFOS_DRIVER_KIND_NETWORK,
    .name = "r8169", .version = 1, .init = start, .fini = stop,
};
