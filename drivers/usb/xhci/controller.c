/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/console.h>
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>

#define XHCI_CMD 0x00u
#define XHCI_STATUS 0x04u
#define XHCI_PAGESIZE 0x08u
#define XHCI_HALTED 1u
#define XHCI_HCRST 2u
#define XHCI_CNR (1u<<11)

static struct xhci_controller *controllers;
static const struct reliefos_driver_kernel_api *services;

uint32_t xhci_read(struct xhci_controller *controller,uint32_t offset)
{ return *(volatile uint32_t *)(controller->mapping+offset); }
void xhci_write(struct xhci_controller *controller,uint32_t offset,uint32_t value)
{
    *(volatile uint32_t *)(controller->mapping+offset)=value;
    (void)*(volatile uint32_t *)(controller->mapping+offset);
}
void xhci_write64(struct xhci_controller *controller,uint32_t offset,uint64_t value)
{
    if(xhci_mmio_address(controller->mapping,offset,value,controller->dma_mask==UINT64_MAX))
        __atomic_store_n(&controller->stop.failed,true,__ATOMIC_RELEASE);
}
int xhci_wait(struct xhci_controller *controller,uint32_t offset,uint32_t mask,
              uint32_t value,unsigned milliseconds)
{
    struct xhci_timer timer;int rc=xhci_timer_start(&timer);if(rc)return rc;
    for (unsigned spin=0;;++spin) {
        if ((xhci_read(controller,offset)&mask)==value)return 0;
        if (xhci_timer_expired(&timer,(uint64_t)milliseconds*1000u) || spin==50000000u)
            return -ETIMEDOUT;
        __asm__ volatile("pause" ::: "memory");
    }
}

int xhci_dma_alloc(struct xhci_controller *controller,struct xhci_dma *dma,uint32_t pages)
{
    if (!controller || !dma || !pages || dma->physical)return -EINVAL;
    uint64_t physical=mm_alloc_pages_below(pages,controller->dma_mask);
    if (!physical)return -ENOMEM;
    void *cpu=mm_phys_to_virt(physical);
    if (!cpu || (physical&4095u) || ((uintptr_t)cpu&4095u) ||
        physical>controller->dma_mask-(uint64_t)pages*4096u+1u) {
        mm_free_pages(physical,pages);return -EIO;
    }
    *dma=(struct xhci_dma){.physical=physical,.pages=pages,.cpu=cpu};
    __builtin_memset(cpu,0,(size_t)pages*4096u);
    return 0;
}
void xhci_dma_release(struct xhci_dma *dma)
{
    if (dma->physical)mm_free_pages(dma->physical,dma->pages);
    *dma=(struct xhci_dma){0};
}

struct firmware_io { const struct pci_device *pci;uint64_t physical;uint32_t limit; };
static int firmware_read(void *context,uint32_t offset,uint32_t *value)
{
    struct firmware_io *io=context;
    if(!value || offset>io->limit-4u)return -EOPNOTSUPP;
    void *page=pci_map_xhci_firmware_cap_page(io->pci,io->physical,offset,io->limit);
    if (!page)return -EIO;
    *value=*(volatile uint32_t *)((uint8_t *)page+(offset&4095u));
    pci_unmap_mmio(page,4096);
    return 0;
}
static int firmware_write32(void *context,uint32_t offset,uint32_t value)
{
    struct firmware_io *io=context;
    if(offset>io->limit-4u)return -EOPNOTSUPP;
    void *page=pci_map_xhci_firmware_cap_page(io->pci,io->physical,offset,io->limit);
    if (!page)return -EIO;
    volatile uint32_t *reg=(volatile uint32_t *)((uint8_t *)page+(offset&4095u));
    *reg=value;(void)*reg;
    pci_unmap_mmio(page,4096);
    return 0;
}
static int firmware_write8(void *context,uint32_t offset,uint8_t value)
{
    struct firmware_io *io=context;
    if(offset>=io->limit)return -EOPNOTSUPP;
    void *page=pci_map_xhci_firmware_cap_page(io->pci,io->physical,offset&~3u,io->limit);
    if(!page)return -EIO;
    volatile uint8_t *reg=(volatile uint8_t *)page+(offset&4095u);
    *reg=value;(void)*reg;
    pci_unmap_mmio(page,4096);
    return 0;
}

static int stop_unregister(void *context)
{
    struct xhci_controller *controller=context;
    int rc=usb_host_unregister(controller->host);
    if (!rc)controller->host=NULL;
    return rc;
}
static void stop_request(void *context)
{
    struct xhci_controller *controller=context;
    xhci_write(controller,controller->operational+XHCI_CMD,0);
}
static int stop_wait(void *context)
{
    struct xhci_controller *controller=context;
    return xhci_wait(controller,controller->operational+XHCI_STATUS,XHCI_HALTED,XHCI_HALTED,1000);
}
static int stop_master(void *context)
{
    struct xhci_controller *controller=context;const struct pci_device *pci=&controller->pci;
    uint16_t cmd=pci_config_read16(pci->bus,pci->slot,pci->function,4);
    pci_config_write16(pci->bus,pci->slot,pci->function,4,cmd&~4u);
    return pci_config_read16(pci->bus,pci->slot,pci->function,4)&4u ? -EIO : 0;
}
static int controller_stop(void *context)
{
    struct xhci_controller *controller=context;
    if (!controller->owned) {controller->stop.stopped=true;return 0;}
    static const struct xhci_stop_ops operations={stop_unregister,stop_request,stop_wait,stop_master};
    return xhci_quiesce(&controller->stop,&operations,controller);
}
static void controller_release(void *context)
{
    struct xhci_controller *controller=context;
    xhci_devices_release(controller);
    xhci_dma_release(&controller->scratch_pages);xhci_dma_release(&controller->scratch_array);
    xhci_dma_release(&controller->bounce);xhci_dma_release(&controller->erst);
    xhci_dma_release(&controller->events);xhci_dma_release(&controller->commands);
    xhci_dma_release(&controller->dcbaa);
    if (controller->mapping)pci_unmap_mmio((void *)controller->mapping,controller->mapping_bytes);
    struct xhci_controller **at=&controllers;
    while (*at && *at!=controller)at=&(*at)->next;
    if (*at)*at=controller->next;
    kernel_free(controller);
}

static int probe(const struct pci_device *pci)
{
    struct xhci_timer timer;int rc=xhci_timer_start(&timer);if(rc)return rc;
    uint32_t bar=pci_config_read32(pci->bus,pci->slot,pci->function,0x10);
    if (!bar || (bar&1u) || ((bar>>1&3u)!=0 && (bar>>1&3u)!=2))return -EOPNOTSUPP;
    uint64_t physical=bar&~15u;
    if ((bar&6u)==4u)physical|=(uint64_t)pci_config_read32(pci->bus,pci->slot,pci->function,0x14)<<32;
    if (!physical || (physical&4095u))return -EOPNOTSUPP;
    uint16_t pci_command=pci_config_read16(pci->bus,pci->slot,pci->function,4);
    pci_config_write16(pci->bus,pci->slot,pci->function,4,pci_command|2u);
    volatile uint8_t *initial=pci_map_firmware_bar0(pci,physical);if(!initial)return -ENOMEM;
    /* CAPLENGTH and HCIVERSION share an architected 32-bit MMIO register.
     * Some controllers do not implement the corresponding sub-dword reads. */
    uint32_t header=*(volatile uint32_t *)initial;
    uint32_t cap_length=header&0xffu,version=header>>16;
    uint32_t hcs=*(volatile uint32_t *)(initial+4),hcs2=*(volatile uint32_t *)(initial+8);
    if (cap_length<0x20 || (cap_length&3u) || version<0x100 || !(hcs&255u) ||
        !((hcs>>8)&0x7ffu) || !(hcs>>24)) {pci_unmap_mmio((void *)initial,4096);return -EINVAL;}
    struct xhci_controller *controller=kernel_malloc(sizeof(*controller));
    if (!controller) {pci_unmap_mmio((void *)initial,4096);return -ENOMEM;}
    /* The port protocol table is large; initialize the owned allocation
     * directly rather than creating a controller-sized boot-stack temporary. */
    __builtin_memset(controller,0,sizeof(*controller));
    controller->pci=*pci;
    controller->bar_phys=physical;controller->mapping=initial;controller->mapping_bytes=4096;
    controller->operational=cap_length;controller->ports=(uint8_t)(hcs>>24);
    controller->slots=(uint8_t)hcs;controller->next=controllers;
    kernel_spin_init(&controller->event_lock);
    rc=module_add_resource(THIS_MODULE,controller_stop,controller_release,controller);
    if(rc){pci_unmap_mmio((void *)initial,4096);kernel_free(controller);return rc;}
    controllers=controller;
    uint32_t firmware_limit;
    rc=xhci_firmware_minimum((uint8_t)cap_length,hcs,&firmware_limit);if(rc)return rc;
    struct firmware_io io={pci,physical,firmware_limit};uint32_t hcc,db,rt,legacy,cap_extent;
    if ((rc=firmware_read(&io,0x10,&hcc)) || (rc=firmware_read(&io,0x14,&db)) ||
        (rc=firmware_read(&io,0x18,&rt)))return rc;
    struct xhci_geometry geometry;
    rc=xhci_geometry_init(cap_length,hcs,db,rt,physical,&geometry);
    if(rc)return rc;
    io.limit=geometry.limit;
    rc=xhci_firmware_extent(firmware_read,&io,&geometry,hcc,&legacy,&cap_extent);
    if(rc){console_warn("[usb] xHCI firmware capability walk error=%d safe_limit=%u\n",rc,geometry.limit);return rc;}
    rc=xhci_firmware_handoff(firmware_read,firmware_write8,firmware_write32,
                             &io,legacy,&controller->owned);if(rc)return rc;
    rc=controller_stop(controller);if(rc)return rc;
    uint32_t bytes;
    rc=xhci_mmio_layout(&geometry,cap_extent,&bytes);
    if(rc)return rc;
    if (!pci_device_bar_contains(pci,0,physical,bytes))return -EOPNOTSUPP;
    void *mapping=pci_map_mmio(physical,bytes);if(!mapping)return -ENOMEM;
    pci_unmap_mmio((void *)initial,4096);
    controller->mapping=mapping;controller->mapping_bytes=bytes;
    controller->doorbells=geometry.doorbells;controller->runtime=geometry.runtime;
    controller->stride=(hcc&(1u<<2)) ? 64 : 32;
    controller->dma_mask=(hcc&1u) ? UINT64_MAX : UINT32_MAX;
    controller->per_port_power=(hcc&(1u<<3))!=0;
    rc=xhci_protocol_parse((volatile uint32_t *)mapping,bytes,(hcc>>16)*4u,
                           controller->ports,controller->protocol);if(rc)return rc;
    xhci_write(controller,cap_length+XHCI_CMD,XHCI_HCRST);
    rc=xhci_wait(controller,cap_length+XHCI_CMD,XHCI_HCRST,0,1000);if(rc)return rc;
    rc=xhci_wait(controller,cap_length+XHCI_STATUS,XHCI_CNR,0,1000);if(rc)return rc;
    if (!(xhci_read(controller,cap_length+XHCI_PAGESIZE)&1u))return -EOPNOTSUPP;
    unsigned scratch=((hcs2>>27)&31u)|(((hcs2>>21)&31u)<<5);
    if (scratch>512)return -EOPNOTSUPP;
    if ((rc=xhci_dma_alloc(controller,&controller->dcbaa,1)) ||
        (rc=xhci_dma_alloc(controller,&controller->commands,1)) ||
        (rc=xhci_dma_alloc(controller,&controller->events,1)) ||
        (rc=xhci_dma_alloc(controller,&controller->erst,1)) ||
        (rc=xhci_dma_alloc(controller,&controller->bounce,16)))return rc;
    if (scratch && ((rc=xhci_dma_alloc(controller,&controller->scratch_array,1)) ||
                    (rc=xhci_dma_alloc(controller,&controller->scratch_pages,scratch))))return rc;
    rc=xhci_scratchpad_layout(controller->dcbaa.cpu,controller->scratch_array.cpu,scratch,
                              controller->scratch_array.physical,controller->scratch_pages.physical,
                              controller->dma_mask==UINT64_MAX);if(rc)return rc;
    rc=xhci_ring_init(&controller->command_ring,controller->commands.cpu,
                      controller->commands.physical,256);if(rc)return rc;
    uint32_t *erst=controller->erst.cpu;
    erst[0]=(uint32_t)controller->events.physical;erst[1]=(uint32_t)(controller->events.physical>>32);
    erst[2]=XHCI_EVENT_COUNT;controller->event_cycle=1;
    rc=xhci_start_rings(controller,pci_command);if(rc)return rc;
    rc=usb_host_register_service(xhci_poll,xhci_service,controller,THIS_MODULE,&controller->host);
    if(rc)return rc;
    controller->stop.registered=true;
    for(unsigned port=0;port<controller->ports;++port)
        if(controller->protocol[port].major==2 || controller->protocol[port].major==3)
            controller->port_work[port/32]|=1u<<(port%32);
    usb_host_request_service(controller->host);
    console_info("[usb] xHCI pci=%u:%u.%u ports=%u slots=%u context=%u ac64=%u scratchpads=%u\n",
        pci->bus,pci->slot,pci->function,controller->ports,controller->slots,controller->stride,
        controller->dma_mask==UINT64_MAX,scratch);
    return 0;
}

static int initialize(const struct reliefos_driver_kernel_api *api)
{
    services=api;
    for(uint32_t index=0;;++index) {
        struct reliefos_driver_pci_device found;int rc=api->pci_enumerate(index,&found);
        if(rc==-ENODEV)break;
        if(rc)return rc;
        if(found.class_code!=0x0c || found.subclass!=3 || found.prog_if!=0x30)continue;
        struct pci_device pci;if(pci_read_device(found.bus,found.slot,found.function,&pci))return -ENODEV;
        rc=probe(&pci);
        if(rc){console_error("[usb] xHCI pci=%u:%u.%u unavailable error=%d\n",pci.bus,pci.slot,pci.function,rc);return rc;}
    }
    return 0;
}
static void finish(void)
{
    int error=0;
    for(struct xhci_controller *controller=controllers;controller;controller=controller->next) {
        int rc=controller_stop(controller);if(rc && !error)error=rc;
    }
    if(error){services->report_teardown_failure(error);return;}
    if(!THIS_MODULE)while(controllers)controller_release(controllers);
}
static const struct reliefos_driver_module xhci_driver={
    .magic=RELIEFOS_DRIVER_MODULE_MAGIC,.abi_version=RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size=sizeof(struct reliefos_driver_module),.kind=RELIEFOS_DRIVER_KIND_INPUT,
    .name="xhci",.version=1,.init=initialize,.fini=finish,
};
static int xhci_register(void){return driver_module_register(&xhci_driver,THIS_MODULE);}
static void xhci_exit(void){}
block_initcall(xhci_register);
module_exit(xhci_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT xHCI USB host");
MODULE_ALIAS("pci:v*d*sv*sd*bc0Csc03i30*");
