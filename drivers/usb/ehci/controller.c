/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/console.h>
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>

static struct ehci_controller *controllers;
static const struct reliefos_driver_kernel_api *services;

/** @brief Read one operational register in the controller's retained UC BAR. */
uint32_t ehci_read(struct ehci_controller *controller, unsigned offset)
{ return *(volatile uint32_t *)(controller->operational + offset); }

/** @brief Publish a register write and flush posted MMIO through a readback. */
void ehci_write(struct ehci_controller *controller, unsigned offset, uint32_t value)
{
    *(volatile uint32_t *)(controller->operational + offset) = value;
    (void)*(volatile uint32_t *)(controller->operational + offset);
}

/** @brief Bound a hardware handshake even if a device never changes its register. */
int ehci_wait(struct ehci_controller *controller, unsigned offset, uint32_t mask,
               uint32_t wanted, unsigned milliseconds)
{
    struct ehci_timer timer;
    int rc=ehci_timer_start(&timer);if(rc)return rc;
    for (unsigned spin = 0; ; ++spin) {
        if ((ehci_read(controller, offset) & mask) == wanted) return 0;
        if (ehci_timer_expired(&timer,(uint64_t)milliseconds*1000u) || spin == 10000000u)
            return -ETIMEDOUT;
        __asm__ volatile("pause" ::: "memory");
    }
}

/** @brief Take firmware's standard EHCI ownership semaphore before touching RUN. */
static int take_ownership(const struct pci_device *pci, uint32_t hcc)
{
    uint8_t offset = hcc >> 8; uint64_t visited = 0;
    for (unsigned n = 0; offset && n < 48; ++n) {
        if (offset < 0x40 || (offset & 3u) || (visited & (1ull << (offset / 4)))) return -EINVAL;
        visited |= 1ull << (offset / 4);
        uint32_t cap = pci_config_read32(pci->bus,pci->slot,pci->function,offset);
        if ((cap & 0xffu) == 1) {
            if (offset > 0xf8) return -EINVAL;
            pci_config_write32(pci->bus,pci->slot,pci->function,offset,cap | 1u << 24);
            unsigned waited = 0;
            while (pci_config_read32(pci->bus,pci->slot,pci->function,offset) & (1u << 16)) {
                if (waited++ == 100) return -EBUSY;
                ehci_delay(10);
            }
            /* USBLEGCTLSTS: disable SMI enables, acknowledge only status bits. */
            pci_config_write32(pci->bus,pci->slot,pci->function,offset + 4,0xe0000000u);
        }
        offset = cap >> 8;
    }
    return offset ? -EINVAL : 0;
}

/** @brief Drain polling and prove HALT plus bus-master disable before any release. */
static int controller_stop(void *context)
{
    struct ehci_controller *controller = context;
    if (controller->host) {
        int rc = usb_host_unregister(controller->host); if (rc) return rc;
        controller->host = NULL;
    }
    __atomic_store_n(&controller->failed,true,__ATOMIC_RELEASE);
    ehci_write(controller,EHCI_INTR,0);
    ehci_write(controller,EHCI_CMD,0);
    int halted = ehci_wait(controller,EHCI_STATUS,EHCI_HALTED,EHCI_HALTED,100);
    uint16_t command = pci_config_read16(controller->pci.bus,controller->pci.slot,controller->pci.function,4);
    pci_config_write16(controller->pci.bus,controller->pci.slot,controller->pci.function,4,command & ~4u);
    if (pci_config_read16(controller->pci.bus,controller->pci.slot,controller->pci.function,4) & 4u) return -EIO;
    if (halted) return halted;
    ehci_write(controller,EHCI_CONFIGFLAG,0);
    controller->stopped = true;
    return 0;
}

/** @brief Release CPU metadata, coherent DMA and the exact BAR after complete drain. */
static void controller_release(void *context)
{
    struct ehci_controller *controller = context;
    ehci_devices_release(controller);
    if (controller->dma_phys)
        mm_free_pages(controller->dma_phys,(sizeof(struct ehci_dma) + 4095u) / 4096u);
    pci_unmap_mmio((void *)controller->mapping,4096);
    struct ehci_controller **at = &controllers;
    while (*at && *at != controller) at = &(*at)->next;
    if (*at) *at = controller->next;
    kernel_free(controller);
}

/** @brief Probe a PCI EHCI function with ownership and cleanup established before DMA. */
static int probe(const struct pci_device *pci)
{
    struct ehci_timer timer;
    int clock=ehci_timer_start(&timer);if(clock)return clock;
    uint32_t bar = pci_config_read32(pci->bus,pci->slot,pci->function,0x10);
    if (!bar || (bar & 1u) || ((bar >> 1 & 3u) != 0 && (bar >> 1 & 3u) != 2)) return -EOPNOTSUPP;
    uint64_t physical = bar & ~15u;
    if ((bar & 6u) == 4u)
        physical |= (uint64_t)pci_config_read32(pci->bus,pci->slot,pci->function,0x14) << 32;
    if (!physical || (physical & 4095u)) return -EOPNOTSUPP;
    uint16_t command = pci_config_read16(pci->bus,pci->slot,pci->function,4);
    pci_config_write16(pci->bus,pci->slot,pci->function,4,command | 2u);
    /* Read only the architected capability header from firmware's assigned
     * BAR. Sizing a live BAR would disable decode and overwrite it while BIOS
     * SMI code may still own the host. Defer sizing until handoff and HALT. */
    volatile uint8_t *mapping = pci_map_firmware_bar0(pci,physical); if (!mapping) return -ENOMEM;
    unsigned cap_length = mapping[0];
    uint32_t hcs = *(volatile uint32_t *)(mapping + 4);
    uint32_t hcc = *(volatile uint32_t *)(mapping + 8);
    uint16_t version = *(volatile uint16_t *)(mapping + 2);
    int rc = cap_length < 0x10 || cap_length > 0x80 || version < 0x100 || !(hcs & 15u) ? -EINVAL : 0;
    if (!rc) rc = take_ownership(pci,hcc);
    if (rc) { pci_unmap_mmio((void *)mapping,4096); return rc; }
    struct ehci_controller *controller = kernel_malloc(sizeof(*controller));
    if (!controller) { pci_unmap_mmio((void *)mapping,4096); return -ENOMEM; }
    *controller = (struct ehci_controller){.pci=*pci,.mapping=mapping,.operational=mapping+cap_length,
        .port_count=hcs&15u,.per_port_power=(hcs&(1u<<4))!=0,.next=controllers,.busy=1};
    rc = module_add_resource(THIS_MODULE,controller_stop,controller_release,controller);
    if (rc) { pci_unmap_mmio((void *)mapping,4096);kernel_free(controller);return rc; }
    controllers = controller;
    rc = controller_stop(controller); if (rc) return rc;
    if (!pci_device_bar_contains(pci,0,physical,4096)) return -EOPNOTSUPP;
    ehci_write(controller,EHCI_CMD,EHCI_RESET);
    rc = ehci_wait(controller,EHCI_CMD,EHCI_RESET,0,100); if (rc) return rc;
    uint32_t pages = (sizeof(struct ehci_dma)+4095u)/4096u;
    uint64_t allocation = mm_alloc_pages_below(pages,UINT32_MAX);
    if (!allocation) return -ENOMEM;
    controller->dma_phys=allocation;controller->dma=mm_phys_to_virt(allocation);
    if (!controller->dma || ((uintptr_t)controller->dma & 4095u) || (allocation & 4095u) ||
        allocation > UINT32_MAX-(uint64_t)pages*4096u+1u) return -EIO;
    __builtin_memset(controller->dma,0,sizeof(*controller->dma));
    ehci_schedule_init(controller);
    controller->failed=false;controller->stopped=false;
    pci_config_write16(pci->bus,pci->slot,pci->function,4,command | 6u);
    if (!(pci_config_read16(pci->bus,pci->slot,pci->function,4) & 4u)) return -EIO;
    ehci_write(controller,EHCI_INTR,0);
    ehci_write(controller,EHCI_STATUS,0x3fu);
    ehci_write(controller,EHCI_CMD,EHCI_RUN | 8u << 16);
    rc = ehci_wait(controller,EHCI_STATUS,EHCI_HALTED,0,100); if (rc) return rc;
    ehci_write(controller,EHCI_CONFIGFLAG,1);
    for (unsigned port=0;port<controller->port_count;++port) {
        uint32_t value=ehci_read(controller,EHCI_PORTSC+4*port)&~EHCI_PORT_W1C;
        if (controller->per_port_power) value|=EHCI_PORT_POWER;
        ehci_write(controller,EHCI_PORTSC+4*port,value);
    }
    ehci_delay(20);
    controller->port_work=(1u<<controller->port_count)-1u;
    controller->busy=0;ehci_service(controller);
    if (controller->failed) return -EIO;
    rc = usb_host_register_service(ehci_poll,ehci_service,controller,THIS_MODULE,&controller->host);
    if (!rc) console_info("[usb] EHCI pci=%u:%u.%u ports=%u USB2 highspeed enabled\n",
        pci->bus,pci->slot,pci->function,controller->port_count);
    return rc;
}

/** @brief Discover EHCI functions during builtin block startup or module initialization. */
static int initialize(const struct reliefos_driver_kernel_api *api)
{
    services=api;
    for (uint32_t index=0;;++index) {
        struct reliefos_driver_pci_device found;
        int rc=api->pci_enumerate(index,&found);
        if (rc==-ENODEV) break;
        if (rc) return rc;
        if (found.class_code!=0x0c || found.subclass!=3 || found.prog_if!=0x20) continue;
        struct pci_device pci;
        if (pci_read_device(found.bus,found.slot,found.function,&pci)) return -ENODEV;
        rc=probe(&pci);
        if (rc) { console_error("[usb] EHCI pci=%u:%u.%u unavailable error=%d\n",pci.bus,pci.slot,pci.function,rc);return rc; }
    }
    return 0;
}

/** @brief Stop all owned controllers; failed STOP retains every hardware allocation. */
static void finish(void)
{
    int error=0;
    for (struct ehci_controller *controller=controllers;controller;controller=controller->next) {
        int rc=controller_stop(controller);if (rc && !error) error=rc;
    }
    if (error) { services->report_teardown_failure(error);return; }
    /* Native owner resources consume each controller only in final release.
     * Builtin rollback has no ledger, so reclaim its stopped state here. */
    if (!THIS_MODULE) while (controllers) controller_release(controllers);
}

static const struct reliefos_driver_module ehci_driver={
    .magic=RELIEFOS_DRIVER_MODULE_MAGIC,.abi_version=RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size=sizeof(struct reliefos_driver_module),.kind=RELIEFOS_DRIVER_KIND_INPUT,
    .name="ehci",.version=1,.init=initialize,.fini=finish,
};

/** @brief Register the same waitable USB host implementation in builtin and module mode. */
static int ehci_register(void) { return driver_module_register(&ehci_driver,THIS_MODULE); }
/** @brief Complete exit after the owner has stopped hardware and retired block users. */
static void ehci_exit(void) { }
block_initcall(ehci_register);
module_exit(ehci_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT EHCI USB2 storage and boot HID host");
MODULE_ALIAS("pci:v*d*sv*sd*bc0Csc03i20*");
