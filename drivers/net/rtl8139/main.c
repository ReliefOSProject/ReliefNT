/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>
#include <reliefnt/net_device.h>

static struct r8139_device *devices;

/** @brief Query the exact controller's carrier under its register lock.
 * @param context Callback-pinned device. @return Nonzero only with usable carrier.
 */
static int ready(void *context)
{
    struct r8139_device *d = context;
    uint64_t flags;
    kernel_spin_lock_irqsave(&d->lock, &flags);
    int result = d->active && !d->fault && !(r8139_read8(d, R8139_MEDIA) & R8139_LINK_BAD);
    kernel_spin_unlock_irqrestore(&d->lock, flags);
    return result;
}

/** @brief Borrow this device's immutable station address.
 * @param context Callback-pinned device. @return Six device-owned bytes.
 */
static const uint8_t *mac(void *context)
{
    return ((struct r8139_device *)context)->mac;
}

/** @brief Submit a frame through this device's bounded TX queue.
 * @param context Callback-pinned device. @param frame Borrowed Ethernet frame.
 * @param length Bytes excluding FCS. @return Zero or negative Linux errno.
 */
static int send(void *context, const void *frame, uint32_t length)
{
    return r8139_send(context, frame, length);
}

/** @brief Receive one frame through this device's DMA ring.
 * @param context Callback-pinned device. @param frame Writable output.
 * @param capacity Output bytes. @param length Optional output length.
 * @return One frame, zero or negative Linux errno.
 */
static int receive(void *context, void *frame, uint32_t capacity, uint32_t *length)
{
    return r8139_receive(context, frame, capacity, length);
}

/** @brief Copy this exact PCI function's identity, MAC and carrier state.
 * @param context Callback-pinned device. @param out Nullable caller output.
 */
static void info(void *context, struct e1000_info *out)
{
    struct r8139_device *d = context;
    if (!out)
        return;
    *out = (struct e1000_info){.present = 1, .active = ready(d) != 0,
        .vendor_id = d->pci.vendor_id, .device_id = d->pci.device_id,
        .bus = d->pci.bus, .slot = d->pci.slot, .function = d->pci.function};
    for (unsigned i = 0; i < 6; ++i)
        out->mac[i] = d->mac[i];
}

static const struct reliefnt_netdev_ops ops = {
    RELIEFNT_NETDEV_OPS_VERSION, sizeof(ops), ready, mac, send, receive, info
};

/** @brief Stop one controller before restoring PCI decode and releasing DMA.
 * @param d Exclusively owned controller after provider callback drain.
 * @return Zero on release, or negative errno while retaining every unsafe lease.
 */
static int stop_device(struct r8139_device *d)
{
    int rc = r8139_stop(d);
    if (rc)
        return rc;
    r8139_free(d);
    if (d->command_saved) {
        const struct reliefos_driver_pci_device *p = &d->pci;
        /* Do not restore firmware bus mastering after releasing its buffers. */
        d->api->pci_write16(p->bus, p->slot, p->function, 4, d->original_command & ~4u);
    }
    return 0;
}

/** @brief Probe all matching PCI functions and publish independent network devices.
 * @param api Size-versioned services retained through the complete driver lifetime.
 * @return Zero with a working device, otherwise first probe errno or ENODEV.
 */
static int start(const struct reliefos_driver_kernel_api *api)
{
    if (!api || api->abi_version != RELIEFOS_DRIVER_ABI_VERSION ||
        api->struct_size < RELIEFOS_DRIVER_PHYS_MAP_API_SIZE || !api->pci_enumerate ||
        !api->pci_read16 || !api->pci_write16 || !api->pci_read32 || !api->inb ||
        !api->outb || !api->inl || !api->outl || !api->alloc_dma || !api->free_dma ||
        !api->phys_to_virt || !api->ticks || !api->sleep_ms || !api->console_log ||
        !api->report_teardown_failure)
        return -22;
    int first_error = 0;
    unsigned registered = 0;
    for (uint32_t index = 0;; ++index) {
        struct reliefos_driver_pci_device pci;
        int rc = api->pci_enumerate(index, &pci);
        if (rc == -19)
            break;
        if (rc) {
            if (!first_error)
                first_error = rc;
            break;
        }
        if (pci.vendor_id != 0x10ec || pci.device_id != 0x8139 ||
            pci.class_code != 2 || pci.subclass != 0)
            continue;
        struct r8139_device *d = kernel_malloc(sizeof(*d));
        if (!d) {
            if (!first_error)
                first_error = -12;
            break;
        }
        *d = (struct r8139_device){.api = api, .pci = pci, .next = devices};
        devices = d;
        rc = r8139_probe(d);
        if (!rc)
            rc = r8139_allocate(d);
        if (!rc)
            rc = r8139_activate(d);
        if (!rc) {
            d->active = true;
            rc = driver_register_netdev(&ops, d, &d->ifindex);
        }
        if (rc) {
            api->console_log(CONSOLE_LOG_WARN,
                "[rtl8139] %02x:%02x.%u initialization failed: %d\n",
                pci.bus, pci.slot, pci.function, rc);
            if (!first_error)
                first_error = rc;
            if (stop_device(d))
                return rc;
            devices = d->next;
            kernel_free(d);
            continue;
        }
        ++registered;
        api->console_log(CONSOLE_LOG_INFO,
            "[rtl8139] %02x:%02x.%u io=%04x MAC=%02x:%02x:%02x:%02x:%02x:%02x link=%s irq=poll\n",
            pci.bus, pci.slot, pci.function, d->io_base, d->mac[0], d->mac[1], d->mac[2],
            d->mac[3], d->mac[4], d->mac[5], ready(d) ? "up" : "down");
    }
    return registered ? 0 : first_error ? first_error : -19;
}

/** @brief Drain every controller, retaining any device whose STOP cannot be proven. */
static void stop(void)
{
    struct r8139_device **link = &devices;
    while (*link) {
        struct r8139_device *d = *link;
        int rc = stop_device(d);
        if (rc) {
            d->api->report_teardown_failure(rc);
            link = &d->next;
            continue;
        }
        *link = d->next;
        kernel_free(d);
    }
}

const struct reliefos_driver_module rtl8139_driver_module = {
    .magic = RELIEFOS_DRIVER_MODULE_MAGIC,
    .abi_version = RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size = sizeof(struct reliefos_driver_module),
    .kind = RELIEFOS_DRIVER_KIND_NETWORK,
    .name = "rtl8139",
    .version = 1,
    .init = start,
    .fini = stop,
};
