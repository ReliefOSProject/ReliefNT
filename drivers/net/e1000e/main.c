/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>
#include <reliefnt/net_device.h>

static struct e1k_device *devices;

/** @brief Report carrier of the specific controller.
 * @param context Pinned controller. @return Nonzero when the link is up.
 */
static int ready(void *context)
{
    struct e1k_device *d=context;
    return d->active && (e1k_read(d,E1K_STATUS)&E1K_STATUS_LINK);
}
/** @brief Borrow the selected controller's address while pinned.
 * @param context Pinned controller. @return Six immutable device bytes.
 */
static const uint8_t *mac(void *context) { return ((struct e1k_device *)context)->mac; }
/** @brief Enqueue a frame to the selected controller.
 * @param context Pinned controller. @param frame Borrowed frame.
 * @param length Frame bytes. @return Zero or negative errno.
 */
static int send(void *context, const void *frame, uint32_t length)
{ return e1k_send(context,frame,length); }
/** @brief Receive one frame from the selected controller.
 * @param context Pinned controller. @param frame Output buffer.
 * @param capacity Buffer size. @param length Optional output size.
 * @return One frame, zero or negative errno.
 */
static int receive(void *context, void *frame, uint32_t capacity, uint32_t *length)
{ return e1k_receive(context,frame,capacity,length); }
/** @brief Copy this controller's identity and link state.
 * @param context Pinned controller. @param out Nullable caller-owned output.
 */
static void info(void *context, struct e1000_info *out)
{
    struct e1k_device *d=context;
    if (!out) return;
    *out=(struct e1000_info){.present=d->present,.active=ready(d)!=0,
        .vendor_id=d->pci.vendor_id,.device_id=d->pci.device_id,
        .bus=d->pci.bus,.slot=d->pci.slot,.function=d->pci.function};
    for (unsigned i=0;i<6;++i) out->mac[i]=d->mac[i];
}
static const struct reliefnt_netdev_ops ops={
    RELIEFNT_NETDEV_OPS_VERSION,sizeof(ops),ready,mac,send,receive,info
};

/** @brief Stop and release one complete or partially initialized controller.
 * @param d Exclusively owned controller with callback admission closed.
 * @return Zero after safe release, or negative errno retaining all resources.
 */
static int stop_device(struct e1k_device *d)
{
    int rc=e1k_hardware_stop(d);
    if (rc) return rc;
    e1k_free_rings(d);
    if (d->registers) d->api->unmap_mmio((void *)d->registers,E1K_MMIO_BYTES);
    if (d->command_saved) {
        const struct reliefos_driver_pci_device *p=&d->pci;
        d->api->pci_write16(p->bus,p->slot,p->function,4,d->original_command);
    }
    return 0;
}

/** @brief Probe every supported PCI function and register independent NICs.
 * @param api Validated kernel service table retained for the driver lifetime.
 * @return Zero if at least one registered, or the first probe error/-ENODEV.
 */
static int start(const struct reliefos_driver_kernel_api *api)
{
    if (!api || api->abi_version!=RELIEFOS_DRIVER_ABI_VERSION ||
        api->struct_size<RELIEFOS_DRIVER_PHYS_MAP_API_SIZE ||
        !api->pci_enumerate || !api->pci_read16 || !api->pci_write16 || !api->pci_read32 ||
        !api->map_mmio || !api->unmap_mmio || !api->phys_to_virt ||
        !api->alloc_dma || !api->free_dma || !api->sleep_ms ||
        !api->console_log || !api->report_teardown_failure) return -22;
    int first_error=0; unsigned registered=0;
    for (uint32_t index=0;;++index) {
        struct reliefos_driver_pci_device pci;
        int rc=api->pci_enumerate(index,&pci);
        if (rc==-19) break;
        if (rc) return rc;
        if (pci.vendor_id!=0x8086 || (pci.device_id!=0x10d3 && pci.device_id!=0x150c) ||
            pci.class_code!=2 || pci.subclass!=0) continue;
        struct e1k_device *d=kernel_malloc(sizeof(*d));
        if (!d) {
            api->console_log(CONSOLE_LOG_WARN,"[e1000e] %02x:%02x.%u: no device state memory\n",
                             pci.bus,pci.slot,pci.function);
            if (!first_error) first_error=-12;
            break;
        }
        *d=(struct e1k_device){.api=api,.pci=pci,.next=devices}; devices=d;
        rc=e1k_hardware_start(d);
        if (!rc) rc=e1k_allocate_rings(d);
        if (!rc) {
            uint16_t command=api->pci_read16(pci.bus,pci.slot,pci.function,4);
            api->pci_write16(pci.bus,pci.slot,pci.function,4,command|6u);
            e1k_enable_rings(d); d->active=true;
            uint32_t ifindex;
            rc=driver_register_netdev(&ops,d,&ifindex);
        }
        if (rc) {
            api->console_log(CONSOLE_LOG_WARN,"[e1000e] %02x:%02x.%u initialization failed: %d\n",
                             pci.bus,pci.slot,pci.function,rc);
            if (!first_error) first_error=rc;
            if (stop_device(d)) return rc; /* manager retries mandatory fini */
            devices=d->next; kernel_free(d); continue;
        }
        e1k_write(d,E1K_CTRL_EXT,e1k_read(d,E1K_CTRL_EXT)|E1K_DRV_LOAD);
        e1k_flush(d); ++registered;
        api->console_log(CONSOLE_LOG_INFO,
            "[e1000e] %02x:%02x.%u Intel %04x:%04x MAC=%02x:%02x:%02x:%02x:%02x:%02x; link=%s\n",
            pci.bus,pci.slot,pci.function,pci.vendor_id,pci.device_id,
            d->mac[0],d->mac[1],d->mac[2],d->mac[3],d->mac[4],d->mac[5],
            ready(d)?"up":"negotiating/down");
    }
    return registered ? 0 : first_error ? first_error : -19;
}

/** @brief Stop every owned controller, retaining unsafe ones for retry.
 * Called after callback admission closes and all owner callbacks drain.
 */
static void stop(void)
{
    struct e1k_device **link=&devices;
    while (*link) {
        struct e1k_device *d=*link;
        int rc=stop_device(d);
        if (rc) { d->api->report_teardown_failure(rc); link=&d->next; continue; }
        *link=d->next; kernel_free(d);
    }
}

const struct reliefos_driver_module e1000e_driver_module={
    .magic=RELIEFOS_DRIVER_MODULE_MAGIC,.abi_version=RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size=sizeof(struct reliefos_driver_module),.kind=RELIEFOS_DRIVER_KIND_NETWORK,
    .name="e1000e",.version=1,.init=start,.fini=stop,
};
