/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>
#include <reliefnt/net_device.h>

static struct i225_device *devices;

static int ready(void *context)
{
    struct i225_device *d = context;
    return d->active && (i225_read(d, I225_STATUS) & I225_STATUS_LINK);
}

static const uint8_t *mac(void *context)
{ return ((struct i225_device *)context)->mac; }

static int send(void *context, const void *frame, uint32_t length)
{ return i225_send(context, frame, length); }

static int receive(void *context, void *frame, uint32_t capacity, uint32_t *length)
{ return i225_receive(context, frame, capacity, length); }

static void info(void *context, struct e1000_info *out)
{
    struct i225_device *d = context;
    if (!out)
        return;
    *out = (struct e1000_info){.present=d->present,.active=ready(d) != 0,
        .vendor_id=d->pci.vendor_id,.device_id=d->pci.device_id,
        .bus=d->pci.bus,.slot=d->pci.slot,.function=d->pci.function};
    for (unsigned i = 0; i < 6; ++i)
        out->mac[i] = d->mac[i];
}

static const struct reliefnt_netdev_ops ops = {
    RELIEFNT_NETDEV_OPS_VERSION, sizeof(ops), ready, mac, send, receive, info
};

static int stop_device(struct i225_device *d)
{
    i225_interrupt_stop(d);
    int rc = i225_hardware_stop(d);
    if (rc)
        return rc;
    i225_free_rings(d);
    if (d->registers)
        d->api->unmap_mmio((void *)d->registers, I225_MMIO_BYTES);
    if (d->command_saved) {
        const struct reliefos_driver_pci_device *p = &d->pci;
        d->api->pci_write16(p->bus, p->slot, p->function, 4, d->original_command);
    }
    return 0;
}

static int start(const struct reliefos_driver_kernel_api *api)
{
    if (!api || api->abi_version != RELIEFOS_DRIVER_ABI_VERSION ||
        api->struct_size < RELIEFOS_DRIVER_PHYS_MAP_API_SIZE ||
        !api->pci_enumerate || !api->pci_read16 || !api->pci_write16 || !api->pci_read32 ||
        !api->map_mmio || !api->unmap_mmio || !api->phys_to_virt ||
        !api->alloc_dma || !api->free_dma || !api->sleep_ms ||
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
        if (pci.vendor_id != 0x8086 || pci.device_id != 0x15f3 ||
            pci.class_code != 2 || pci.subclass != 0 || (pci.header_type & 0x7f) != 0)
            continue;

        struct i225_device *d = kernel_malloc(sizeof(*d));
        if (!d) {
            if (!first_error)
                first_error = -12;
            break;
        }
        *d = (struct i225_device){.api=api,.pci=pci,.next=devices};
        devices = d;
        rc = i225_hardware_start(d);
        if (!rc)
            i225_interrupt_prepare(d);
        if (!rc)
            rc = i225_allocate_rings(d);
        if (!rc) {
            i225_configure_rings(d);
            rc = i225_activate_busmaster(d);
        }
        if (!rc) {
            i225_enable_rings(d);
            d->active = true;
            uint32_t ifindex = 0;
            rc = driver_register_netdev(&ops, d, &ifindex);
            if (!rc)
                i225_interrupt_start(d, ifindex);
        }
        if (rc) {
            api->console_log(CONSOLE_LOG_WARN,
                "[i225] %02x:%02x.%u initialization failed: %d\n",
                pci.bus, pci.slot, pci.function, rc);
            if (!first_error)
                first_error = rc;
            if (stop_device(d))
                return rc;
            devices = d->next;
            kernel_free(d);
            continue;
        }
        api->console_log(CONSOLE_LOG_INFO,
            "[i225] %02x:%02x.%u Intel I225-V MAC=%02x:%02x:%02x:%02x:%02x:%02x; link=%s irq=%s\n",
            pci.bus, pci.slot, pci.function, d->mac[0], d->mac[1], d->mac[2],
            d->mac[3], d->mac[4], d->mac[5], ready(d) ? "up" : "down",
            d->irq_handle ? "msi" : "poll");
        ++registered;
    }
    return registered ? 0 : first_error ? first_error : -19;
}

static void stop(void)
{
    struct i225_device **link = &devices;
    while (*link) {
        struct i225_device *d = *link;
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

const struct reliefos_driver_module i225_driver_module = {
    .magic=RELIEFOS_DRIVER_MODULE_MAGIC,.abi_version=RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size=sizeof(struct reliefos_driver_module),.kind=RELIEFOS_DRIVER_KIND_NETWORK,
    .name="i225",.version=1,.init=start,.fini=stop,
};
