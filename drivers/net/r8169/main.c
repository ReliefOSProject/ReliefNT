/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>
#include <reliefnt/net_device.h>

static struct rtl_device *devices;

/** @brief Observe this controller's copper link without MDIO waits.
 * @param context Pinned device. @return Nonzero when carrier is up.
 */
static int ready(void *context)
{
    struct rtl_device *d=context;
    return d->active && (rtl_read8(d, 0x6c) & 2);
}
/** @brief Borrow this controller's immutable station address.
 * @param context Pinned device. @return Six device-owned bytes.
 */
static const uint8_t *mac(void *context) { return ((struct rtl_device *)context)->mac; }
/** @brief Submit a frame to one controller.
 * @param context Pinned device. @param frame Borrowed frame bytes.
 * @param length Number of bytes. @return Zero or negative errno.
 */
static int send(void *context, const void *frame, uint32_t length)
{ return rtl_send(context,frame,length); }
/** @brief Consume a descriptor from one controller.
 * @param context Pinned device. @param frame Writable buffer.
 * @param capacity Buffer bytes. @param length Optional length output.
 * @return One frame, zero or negative errno.
 */
static int receive(void *context, void *frame, uint32_t capacity, uint32_t *length)
{ return rtl_receive(context,frame,capacity,length); }
/** @brief Copy this controller's PCI identity, link and station address.
 * @param context Pinned device. @param out Nullable caller-owned output.
 */
static void info(void *context, struct e1000_info *out)
{
    struct rtl_device *d=context;
    if (!out) return;
    *out=(struct e1000_info){.present=d->present,.active=ready(d)!=0,
        .vendor_id=d->pci.vendor_id,.device_id=d->pci.device_id,
        .bus=d->pci.bus,.slot=d->pci.slot,.function=d->pci.function};
    for (unsigned i=0;i<6;++i) out->mac[i]=d->mac[i];
}
static const struct reliefnt_netdev_ops ops={
    RELIEFNT_NETDEV_OPS_VERSION,sizeof(ops),ready,mac,send,receive,info
};

/** @brief Stop one complete or partial controller before releasing its resources.
 * @param d Exclusively owned device with callbacks drained.
 * @return Zero on safe release, negative errno retaining resources for retry.
 */
static int stop_device(struct rtl_device *d)
{
    int rc=rtl_hardware_stop(d);
    if (rc) return rc;
    rtl_free_rings(d);
    if (d->registers) d->api->unmap_mmio((void *)d->registers,d->mmio_bytes);
    if (d->command_saved) {
        const struct reliefos_driver_pci_device *p=&d->pci;
        d->api->pci_write16(p->bus,p->slot,p->function,4,d->original_command & ~4u);
    }
    return 0;
}

/** @brief Enumerate every supported Realtek function and publish separate contexts.
 * @param api Kernel services retained for driver lifetime.
 * @return Zero for at least one success, or first probe errno/-ENODEV.
 */
static int start(const struct reliefos_driver_kernel_api *api)
{
    if (!api || api->abi_version != RELIEFOS_DRIVER_ABI_VERSION ||
        api->struct_size < RELIEFOS_DRIVER_PHYS_MAP_API_SIZE ||
        !api->pci_enumerate || !api->pci_read16 || !api->pci_write16 || !api->pci_read32 ||
        !api->map_mmio || !api->unmap_mmio || !api->alloc_dma || !api->free_dma ||
        !api->phys_to_virt || !api->sleep_ms || !api->console_log || !api->report_teardown_failure)
        return -22;
    int first_error=0; unsigned registered=0;
    for (uint32_t index=0;;++index) {
        struct reliefos_driver_pci_device pci;
        int rc=api->pci_enumerate(index,&pci);
        if (rc==-19) break;
        if (rc) return rc;
        if (pci.vendor_id!=0x10ec || (pci.device_id!=0x8168 && pci.device_id!=0x8125) ||
            pci.class_code!=2 || pci.subclass!=0) continue;
        struct rtl_device *d=kernel_malloc(sizeof(*d));
        if (!d) {
            api->console_log(CONSOLE_LOG_WARN,"[r8169] %02x:%02x.%u: no device state memory\n",
                             pci.bus,pci.slot,pci.function);
            if (!first_error) first_error=-12;
            break;
        }
        *d=(struct rtl_device){.api=api,.pci=pci,.next=devices}; devices=d;
        rc=rtl_hardware_probe(d);
        if (!rc) rc=rtl_hardware_stop(d);
        if (!rc) rc=rtl_allocate_rings(d);
        if (!rc) {
            uint16_t command=api->pci_read16(pci.bus,pci.slot,pci.function,4);
            api->pci_write16(pci.bus,pci.slot,pci.function,4,command|4u);
            if (!(api->pci_read16(pci.bus,pci.slot,pci.function,4)&4)) rc=-5;
        }
        if (!rc) rc=d->family==RTL_FAMILY_8125 ? rtl8125_start(d) : rtl8168_start(d);
        if (!rc) {
            d->active=true; uint32_t ifindex;
            rc=driver_register_netdev(&ops,d,&ifindex);
        }
        if (rc) {
            api->console_log(CONSOLE_LOG_WARN,"[r8169] %02x:%02x.%u initialization failed: %d\n",
                             pci.bus,pci.slot,pci.function,rc);
            if (!first_error) first_error=rc;
            if (stop_device(d)) return rc; /* manager retries mandatory fini */
            devices=d->next; kernel_free(d); continue;
        }
        ++registered;
        api->console_log(CONSOLE_LOG_INFO,
            "[r8169] %02x:%02x.%u %s %04x:%04x XID=%08x MAC=%02x:%02x:%02x:%02x:%02x:%02x; link=%s\n",
            pci.bus,pci.slot,pci.function,d->chip_name,pci.vendor_id,pci.device_id,d->xid,
            d->mac[0],d->mac[1],d->mac[2],d->mac[3],d->mac[4],d->mac[5],
            ready(d)?"up":"negotiating/down");
    }
    return registered ? 0 : first_error ? first_error : -19;
}

/** @brief Stop each controller; failed resets retain their resources for retry.
 * Called after callback admission closes and outstanding users drain.
 */
static void stop(void)
{
    struct rtl_device **link=&devices;
    while (*link) {
        struct rtl_device *d=*link;
        int rc=stop_device(d);
        if (rc) { d->api->report_teardown_failure(rc); link=&d->next; continue; }
        *link=d->next; kernel_free(d);
    }
}

const struct reliefos_driver_module r8169_driver_module={
    .magic=RELIEFOS_DRIVER_MODULE_MAGIC,.abi_version=RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size=sizeof(struct reliefos_driver_module),.kind=RELIEFOS_DRIVER_KIND_NETWORK,
    .name="r8169",.version=1,.init=start,.fini=stop,
};
