/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/console.h>
#include <reliefnt/blkdev.h>
#include <reliefnt/framebuffer.h>
#include "../../drivers/usb/xhci/internal.h"
#include "../../drivers/usb/storage/internal.h"

static unsigned allocations, dma_releases, key_down, key_up, mouse_up;
uint64_t time_ticks(void){return 0;}
void *kernel_malloc(size_t n){void *p=calloc(1,n);if(p)++allocations;return p;}
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
void xhci_dma_release(struct xhci_dma *dma){if(dma->physical)++dma_releases;*dma=(struct xhci_dma){0};}
uint32_t xhci_read(struct xhci_controller *controller,uint32_t offset)
{assert(offset==controller->operational+4u);return 0;}
void xhci_write(struct xhci_controller *c,uint32_t o,uint32_t v){(void)c;(void)o;(void)v;}
void xhci_write64(struct xhci_controller *c,uint32_t o,uint64_t v){(void)c;(void)o;(void)v;}
void xhci_delay(unsigned milliseconds){(void)milliseconds;}
int xhci_wait(struct xhci_controller *c,uint32_t o,uint32_t m,uint32_t v,unsigned ms)
{(void)c;(void)o;(void)m;(void)v;(void)ms;return 0;}
int xhci_timer_start(struct xhci_timer *timer){*timer=(struct xhci_timer){0};return 0;}
bool xhci_timer_expired(struct xhci_timer *timer,uint64_t us){(void)timer;(void)us;return true;}
int __wrap_xhci_device_attach(struct xhci_controller *c,uint8_t p,uint8_t s,enum xhci_port_rejection *r)
{(void)c;(void)p;(void)s;*r=XHCI_REJECT_NONE;return 0;}
int __wrap_xhci_hid_arm(struct xhci_device *d,unsigned e){(void)d;(void)e;return 0;}
void __wrap_xhci_hid_event(struct xhci_device *d,unsigned e){(void)d;(void)e;assert(0);}
int xhci_endpoint_recover(struct xhci_device *d,unsigned e,bool h)
{(void)d;(void)e;(void)h;return 0;}
void console_log(enum console_log_level level,const char *fmt,...){(void)level;(void)fmt;}
void input_handle_scancode(uint8_t keycode,uint8_t pressed)
{assert(keycode==0x1e);if(pressed)++key_down;else ++key_up;}
void input_push_mouse(int32_t x,int32_t y,int32_t dx,int32_t dy,uint8_t buttons)
{(void)x;(void)y;(void)dx;(void)dy;if(!buttons)++mouse_up;}
void input_push_mouse_wheel(int32_t x,int32_t y,int32_t wheel,uint8_t buttons)
{(void)x;(void)y;(void)wheel;(void)buttons;}
const struct framebuffer *framebuffer_get(void){static const struct framebuffer fb={0};return &fb;}
static int read_blocks(void *context,uint64_t first,uint32_t count,void *output)
{(void)context;(void)first;(void)count;(void)output;assert(0);return 0;}

static void exercise(bool controller_event)
{
    static const struct block_device_operations ops={.read=read_blocks};
    struct xhci_trb event_ring[XHCI_EVENT_COUNT]={0},command_ring[4]={0};
    struct xhci_controller controller={.operational=0x40,.events={.cpu=event_ring,.physical=0x200000},
        .event_cycle=1,.commands={.cpu=command_ring,.physical=0x300000}};
    struct xhci_device *device=kernel_malloc(sizeof(*device));assert(device);
    struct usb_storage *storage=kernel_malloc(sizeof(*storage));assert(storage);
    device->controller=&controller;device->slot_id=1;device->storage[0]=storage;
    device->storage_count=1;device->input.physical=0x1000;
    device->endpoints[3].hid=true;controller.devices=device;
    storage->device=&device->usb;storage->luns[0].registered=true;
    storage->luns[0].registration=(struct block_device_registration){.info={.device=0x10300,
        .blocks=64,.block_size=512,.max_blocks=4,.flags=BLOCK_DEVICE_READ_ONLY},.operations=&ops};
    assert(!register_block_device(&storage->luns[0].registration));
    struct block_device_ref held={0},later={0};uint8_t bytes[512];
    assert(!block_device_get(0x10300,&held));
    assert(!usb_host_register_service(xhci_poll,xhci_service,&controller,NULL,&controller.host));
    uint8_t key[8]={0,0,4};uint8_t mouse[3]={1,0,0};
    unsigned before_key=key_up,before_mouse=mouse_up;
    usb_hid_boot_keyboard(&device->endpoints[3].hid_state,key,sizeof(key));
    usb_hid_boot_mouse(&device->endpoints[3].hid_state,mouse,sizeof(mouse));
    if(controller_event) {
        event_ring[0]=(struct xhci_trb){.control=(37u<<10)|1u,.status=1u<<24};
        usb_poll();
    } else {
        assert(!xhci_ring_init(&controller.command_ring,command_ring,0x300000,4));
        assert(xhci_command(&controller,(struct xhci_trb){.control=9u<<10},NULL)==-ETIMEDOUT);
        assert(controller.command_ring.quarantined && controller.command_active);
        usb_poll();
    }
    assert(controller.stop.failed && device->usb.disconnected);
    assert(key_up==before_key+1 && mouse_up==before_mouse+1);
    assert(usb_service_pending());usb_service_run();
    assert(block_device_read(&held,0,1,bytes)==-ENODEV);
    assert(block_device_get(0x10300,&later)==-ENODEV);
    assert(controller.devices==device && !dma_releases);
    block_device_put(&held);
    for(unsigned i=0;i<400;++i){usb_poll();if(usb_service_pending())usb_service_run();}
    assert(unregister_block_device(&storage->luns[0].registration)==-ENOENT);
    assert(controller.devices==device && !dma_releases);
    assert(!usb_host_unregister(controller.host));controller.stop.stopped=true;
    xhci_devices_release(&controller);
    assert(dma_releases==1 && !allocations);dma_releases=0;
}
int main(void)
{
    exercise(true);exercise(false);
    assert(key_down==2 && key_up==2 && mouse_up==2);
    puts("USB xHCI controller failure releases HID and revokes retained disks while keeping DMA");
}
