/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <reliefnt/console.h>
#include <reliefnt/blkdev.h>
#include "../../drivers/usb/xhci/internal.h"
#include "../../drivers/usb/storage/internal.h"

static unsigned allocations, commands, releases;
uint64_t time_ticks(void){return 0;}
void *kernel_malloc(size_t bytes){void *p=calloc(1,bytes);if(p)++allocations;return p;}
void kernel_free(void *p){if(p){assert(allocations);--allocations;free(p);}}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{assert(!lock->state);lock->state=1;*flags=0;}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{assert(lock->state && !flags);lock->state=0;}
bool try_module_get(struct module *owner){(void)owner;return true;}
void module_put(struct module *owner){(void)owner;}
bool module_execution_get(struct module *owner){(void)owner;return true;}
void module_execution_put(struct module *owner){(void)owner;}
int module_add_resource(struct module *owner,int (*stop)(void *),void (*release)(void *),void *ctx)
{(void)owner;(void)stop;(void)release;(void)ctx;return 0;}
void usb_hid_boot_release(struct usb_hid_state *state){(void)state;}
void xhci_dma_release(struct xhci_dma *dma){if(dma->physical)++releases;*dma=(struct xhci_dma){0};}
int xhci_command(struct xhci_controller *controller,struct xhci_trb trb,uint8_t *slot)
{(void)controller;(void)slot;assert(((trb.control>>10)&63u)==10);++commands;return 0;}
uint32_t xhci_read(struct xhci_controller *controller,uint32_t offset)
{assert(offset==controller->operational+4u);return 0;}
void xhci_write(struct xhci_controller *c,uint32_t o,uint32_t v){(void)c;(void)o;(void)v;assert(0);}
void xhci_events(struct xhci_controller *controller){(void)controller;}
void xhci_delay(unsigned milliseconds){(void)milliseconds;}
int xhci_wait(struct xhci_controller *c,uint32_t o,uint32_t m,uint32_t v,unsigned ms)
{(void)c;(void)o;(void)m;(void)v;(void)ms;return 0;}
int __wrap_xhci_device_attach(struct xhci_controller *c,uint8_t p,uint8_t s,enum xhci_port_rejection *r)
{(void)c;(void)p;(void)s;*r=XHCI_REJECT_NONE;return 0;}
int xhci_endpoint_recover(struct xhci_device *d,unsigned e,bool h)
{(void)d;(void)e;(void)h;return 0;}
int __wrap_xhci_hid_arm(struct xhci_device *d,unsigned e){(void)d;(void)e;return 0;}
void console_log(enum console_log_level level,const char *fmt,...){(void)level;(void)fmt;}
static int read_blocks(void *context,uint64_t first,uint32_t count,void *output)
{(void)context;(void)first;(void)count;(void)output;assert(0);return 0;}

int main(void)
{
    static const struct block_device_operations ops={.read=read_blocks};
    uint64_t dcbaa[2]={0,0x123000};
    struct xhci_controller controller={.dcbaa={.cpu=dcbaa},.operational=0x40};
    struct xhci_device *device=kernel_malloc(sizeof(*device));assert(device);
    struct usb_storage *storage=kernel_malloc(sizeof(*storage));assert(storage);
    device->controller=&controller;device->slot_id=1;device->storage[0]=storage;
    device->storage_count=1;device->input.physical=0x1000;
    storage->device=&device->usb;storage->luns[0].registered=true;
    storage->luns[0].registration=(struct block_device_registration){.info={.device=0x10300,
        .blocks=64,.block_size=512,.max_blocks=4,.flags=BLOCK_DEVICE_READ_ONLY},.operations=&ops};
    controller.devices=device;
    assert(!register_block_device(&storage->luns[0].registration));
    struct block_device_ref held={0},later={0};uint8_t bytes[512];
    assert(!block_device_get(0x10300,&held));
    assert(!usb_host_register_service(xhci_poll,xhci_service,&controller,NULL,&controller.host));
    xhci_device_disconnect(device);usb_host_request_service(controller.host);usb_service_run();
    assert(controller.devices==device && device->disabled && !dcbaa[1] && commands==1 && !releases);
    assert(block_device_read(&held,0,1,bytes)==-ENODEV);
    assert(block_device_get(0x10300,&later)==-ENODEV);
    unsigned service_turns=0;
    for(unsigned i=0;i<400;++i){usb_poll();if(usb_service_pending()){++service_turns;usb_service_run();}}
    assert(controller.devices==device && commands==1 && !releases && service_turns<=8);
    block_device_put(&held);
    for(unsigned i=0;i<400 && controller.devices;++i){usb_poll();if(usb_service_pending())usb_service_run();}
    assert(!controller.devices && releases==1);
    assert(block_device_get(0x10300,&later)==-ENODEV);
    assert(!usb_host_unregister(controller.host));
    assert(!allocations);
    puts("USB xHCI retained disk retires after last close without a new port event");
}
