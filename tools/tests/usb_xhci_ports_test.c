/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <reliefnt/console.h>
#include "../../drivers/usb/xhci/internal.h"

static uint32_t port_value, last_write, status_value;
static unsigned port_reads, requested;
uint64_t time_ticks(void){return 0;}
uint32_t xhci_read(struct xhci_controller *controller,uint32_t offset)
{
    if(offset==controller->operational+4u)return status_value;
    assert(offset==controller->operational+0x400u);
    if(++port_reads==2)port_value|=1u<<19; /* new event after service snapshot */
    return port_value;
}
void xhci_write(struct xhci_controller *controller,uint32_t offset,uint32_t value)
{
    assert(offset==controller->operational+0x400u);last_write=value;
    port_value&=~(value&(0x7fu<<17)); /* emulate W1C change bits */
}
void usb_host_request_service(struct usb_host *host){(void)host;++requested;}
void xhci_events(struct xhci_controller *controller){(void)controller;}
void xhci_delay(unsigned milliseconds){(void)milliseconds;}
int xhci_wait(struct xhci_controller *c,uint32_t o,uint32_t m,uint32_t v,unsigned ms)
{(void)c;(void)o;(void)m;(void)v;(void)ms;return 0;}
int xhci_device_attach(struct xhci_controller *c,uint8_t p,uint8_t s,enum xhci_port_rejection *r)
{(void)c;(void)p;(void)s;*r=XHCI_REJECT_NONE;return 0;}
void xhci_device_disconnect(struct xhci_device *d){d->usb.disconnected=true;}
void xhci_devices_collect(struct xhci_controller *c){(void)c;}
int xhci_endpoint_recover(struct xhci_device *d,unsigned e,bool h)
{(void)d;(void)e;(void)h;return 0;}
int xhci_hid_arm(struct xhci_device *d,unsigned e){(void)d;(void)e;return 0;}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{(void)lock;*flags=0;}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{(void)lock;(void)flags;}
void console_log(enum console_log_level level,const char *fmt,...){(void)level;(void)fmt;}

int main(void)
{
    struct xhci_controller controller={.ports=1,.operational=0x20};
    struct xhci_device device={.controller=&controller,.usb={.root_port=0}};
    controller.devices=&device;controller.protocol[0].valid_speeds=1u<<3;
    controller.port_work[0]=1;
    port_value=1u|(1u<<9)|(1u<<18);
    xhci_service(&controller);
    assert((last_write&(0x7fu<<17))==(1u<<18));
    assert((port_value&(0x7fu<<17))==(1u<<19));
    xhci_poll(&controller);
    assert(requested && controller.port_work[0]==1 && !device.usb.disconnected);
    status_value=1u<<12; /* USBSTS HCE, not just host-system error */
    xhci_poll(&controller);
    assert(controller.stop.failed && device.usb.disconnected);
    puts("USB xHCI port change acknowledgement and fatal status tests passed");
}
