/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>
#include <reliefnt/net_device.h>

static struct vmx_device *devices;
/** @brief Report this controller's carrier. @param context Pinned device. @return Nonzero when
 * ready. */
static int ready(void *context)
{
    return vmx_link(context);
}
/** @brief Return this controller's station address. @param context Pinned device. @return Six
 * bytes. */
static const uint8_t *mac(void *context)
{
    return ((struct vmx_device *)context)->mac;
}
/** @brief Transmit through one controller. @param context Pinned device. @param frame Frame bytes.
 * @param length Byte count. @return Zero or negative errno. */
static int send(void *context, const void *frame, uint32_t length)
{
    return vmx_send(context, frame, length);
}
/** @brief Receive through one controller. @param context Pinned device. @param frame Output. @param
 * capacity Output size. @param length Returned size. @return One, zero or negative errno. */
static int receive(void *context, void *frame, uint32_t capacity, uint32_t *length)
{
    return vmx_receive(context, frame, capacity, length);
}
/** @brief Copy the controller identity. @param context Pinned device. @param out Nullable caller
 * output. */
static void info(void *context, struct e1000_info *out)
{
    struct vmx_device *d = context;
    if (!out)
        return;
    *out = (struct e1000_info){.present = 1,
                               .active = vmx_link(d) != 0,
                               .vendor_id = d->pci.vendor_id,
                               .device_id = d->pci.device_id,
                               .bus = d->pci.bus,
                               .slot = d->pci.slot,
                               .function = d->pci.function};
    for (unsigned i = 0; i < 6; ++i)
        out->mac[i] = d->mac[i];
}
static const struct reliefnt_netdev_ops ops = {
    RELIEFNT_NETDEV_OPS_VERSION, sizeof(ops), ready, mac, send, receive, info};

/** @brief Quiesce and release one controller, retaining it on unsafe STOP.
 * @param d Exclusively owned context. @return Zero or negative errno.
 */
static int stop_device(struct vmx_device *d)
{
    int rc = vmx_hardware_stop(d);
    if (rc)
        return rc;
    vmx_free(d);
    for (unsigned i = 0; i < 2; ++i)
        if (d->bar[i])
            d->api->unmap_mmio(d->bar[i], VMX_BAR_BYTES);
    if (d->command_saved) {
        const struct reliefos_driver_pci_device *p = &d->pci;
        d->api->pci_write16(p->bus, p->slot, p->function, 4, d->original_command);
    }
    return 0;
}
/** @brief Probe all matching PCI functions and register independent NICs.
 * @param api Validated services retained until fini. @return Zero or negative Linux errno.
 */
static int start(const struct reliefos_driver_kernel_api *api)
{
    if (!api || api->abi_version != RELIEFOS_DRIVER_ABI_VERSION ||
        api->struct_size < RELIEFOS_DRIVER_PHYS_MAP_API_SIZE || !api->pci_enumerate ||
        !api->pci_read16 || !api->pci_write16 || !api->pci_read32 || !api->map_mmio ||
        !api->unmap_mmio || !api->alloc_dma || !api->free_dma || !api->phys_to_virt ||
        !api->console_log || !api->report_teardown_failure)
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
        if (pci.vendor_id != 0x15ad || pci.device_id != 0x07b0 || pci.class_code != 2 ||
            pci.subclass != 0)
            continue;
        struct vmx_device *d = kernel_malloc(sizeof(*d));
        if (!d) {
            if (!first_error)
                first_error = -12;
            break;
        }
        *d = (struct vmx_device){.api = api, .pci = pci, .next = devices};
        devices = d;
        rc = vmx_hardware_start(d);
        if (!rc)
            rc = vmx_allocate(d);
        if (!rc)
            rc = vmx_activate(d);
        if (!rc) {
            d->active = true;
            uint32_t ifindex;
            rc = driver_register_netdev(&ops, d, &ifindex);
        }
        if (rc) {
            api->console_log(CONSOLE_LOG_WARN, "[vmxnet3] %02x:%02x.%u initialization failed: %d\n",
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
                         "[vmxnet3] %02x:%02x.%u MAC=%02x:%02x:%02x:%02x:%02x:%02x link=%s\n",
                         pci.bus, pci.slot, pci.function, d->mac[0], d->mac[1], d->mac[2],
                         d->mac[3], d->mac[4], d->mac[5], ready(d) ? "up" : "down");
    }
    return registered ? 0 : first_error ? first_error : -19;
}
/** @brief Drain controllers; retain any device whose DMA stop failed. */
static void stop(void)
{
    struct vmx_device **link = &devices;
    while (*link) {
        struct vmx_device *d = *link;
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
const struct reliefos_driver_module vmxnet3_driver_module = {
    .magic = RELIEFOS_DRIVER_MODULE_MAGIC,
    .abi_version = RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size = sizeof(struct reliefos_driver_module),
    .kind = RELIEFOS_DRIVER_KIND_NETWORK,
    .name = "vmxnet3",
    .version = 1,
    .init = start,
    .fini = stop,
};
