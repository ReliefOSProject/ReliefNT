/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

static struct e1k_device device;

/** @brief Return actual link readiness without waiting or restarting negotiation.
 * @return Nonzero only for an initialized controller whose hardware link is up.
 */
static int ready(void)
{ return device.active && (e1k_read(&device,E1K_STATUS)&E1K_STATUS_LINK); }

/** @brief Borrow the immutable station address during a retained callback.
 * @return Six device-owned bytes, valid until the caller releases its owner pin.
 */
static const uint8_t *mac(void) { return device.mac; }

/** @brief Enqueue one frame through the retained hardware provider.
 * @param frame Borrowed Ethernet bytes. @param length Frame size excluding FCS.
 * @return Zero on admission or a negative Linux errno; no hardware wait.
 */
static int send(const void *frame, uint32_t length)
{ return e1k_send(&device,frame,length); }

/** @brief Consume at most one received Ethernet frame.
 * @param frame Output buffer. @param capacity Buffer size. @param length Optional length output.
 * @return One for a complete frame, zero if empty, or a negative Linux errno.
 */
static int receive(void *frame, uint32_t capacity, uint32_t *length)
{ return e1k_receive(&device,frame,capacity,length); }

/** @brief Return the actual bound PCI identity, MAC and link state.
 * @param out Caller-owned metadata output, nullable; contains no kernel pointers.
 */
static void info(struct reliefos_driver_e1000_info *out)
{
    if (!out) return;
    *out=(struct reliefos_driver_e1000_info){.present=device.present,
        .active=ready()!=0, .vendor_id=device.pci.vendor_id,.device_id=device.pci.device_id,
        .bus=device.pci.bus,.slot=device.pci.slot,.function=device.pci.function};
    for (unsigned i=0;i<6;++i) out->mac[i]=device.mac[i];
}

/** @brief Initialize a supported controller and publish its retained network callbacks.
 * @param api Size-versioned kernel services borrowed for this binding's lifetime.
 * @return Zero or negative Linux errno. Partial resources remain for the manager's
 * mandatory fini rollback; no self-reference or private user ABI is introduced.
 */
static int start(const struct reliefos_driver_kernel_api *api)
{
    static const struct reliefos_driver_e1000_ops ops={ready,mac,send,receive,info};
    if (!api || api->abi_version!=RELIEFOS_DRIVER_ABI_VERSION ||
        api->struct_size<RELIEFOS_DRIVER_PHYS_MAP_API_SIZE ||
        !api->pci_find || !api->pci_read16 || !api->pci_write16 || !api->pci_read32 ||
        !api->map_mmio || !api->unmap_mmio || !api->phys_to_virt ||
        !api->alloc_dma || !api->free_dma || !api->sleep_ms ||
        !api->register_e1000 || !api->console_log || !api->report_teardown_failure) return -22;
    device=(struct e1k_device){.api=api};
    int rc=e1k_hardware_start(&device);
    if (rc) return rc;
    rc=e1k_allocate_rings(&device);
    if (rc) return rc;
    const struct reliefos_driver_pci_device *p=&device.pci;
    uint16_t command=api->pci_read16(p->bus,p->slot,p->function,4);
    api->pci_write16(p->bus,p->slot,p->function,4,command|6u);
    e1k_enable_rings(&device);
    device.active=true;
    rc=api->register_e1000(&ops);
    if (rc) return rc;
    e1k_write(&device,E1K_CTRL_EXT,e1k_read(&device,E1K_CTRL_EXT)|E1K_DRV_LOAD);
    e1k_flush(&device);
    api->console_log(CONSOLE_LOG_INFO,
        "[e1000e] %02x:%02x.%u Intel %04x:%04x MAC=%02x:%02x:%02x:%02x:%02x:%02x legacy DMA rings ready; link=%s\n",
        p->bus,p->slot,p->function,p->vendor_id,p->device_id,
        device.mac[0],device.mac[1],device.mac[2],device.mac[3],device.mac[4],device.mac[5],
        ready()?"up":"negotiating/down");
    return 0;
}

/** @brief Stop hardware before releasing DMA backing and MMIO ownership.
 * Callback admission is closed and pins drained by the manager. Failed STOP
 * reports an error and retains every resource and the module for a safe retry.
 */
static void stop(void)
{
    if (!device.api) return;
    int rc=e1k_hardware_stop(&device);
    if (rc) { device.api->report_teardown_failure(rc); return; }
    e1k_free_rings(&device);
    if (device.registers) device.api->unmap_mmio((void *)device.registers,E1K_MMIO_BYTES);
    if (device.command_saved) {
        const struct reliefos_driver_pci_device *p=&device.pci;
        device.api->pci_write16(p->bus,p->slot,p->function,4,device.original_command);
    }
    device=(struct e1k_device){0};
}

const struct reliefos_driver_module e1000e_driver_module={
    .magic=RELIEFOS_DRIVER_MODULE_MAGIC,.abi_version=RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size=sizeof(struct reliefos_driver_module),.kind=RELIEFOS_DRIVER_KIND_NETWORK,
    .name="e1000e",.version=1,.init=start,.fini=stop,
};
