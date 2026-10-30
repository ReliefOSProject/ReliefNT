/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/console.h>
#include <reliefnt/framebuffer.h>
#include "../../drivers/usb/xhci/internal.h"

/* Exercise the real protocol-to-device path with only hardware, allocation,
 * and the final storage transport replaced by controlled host boundaries. */
static uint8_t expected_id;
static unsigned allocations, bound;
static uint64_t next_dma=0x100000;
static char last_log[512];

void *kernel_malloc(size_t bytes)
{void *p=calloc(1,bytes);if(p)++allocations;return p;}
void kernel_free(void *p)
{if(p){assert(allocations);--allocations;free(p);}}
int xhci_dma_alloc(struct xhci_controller *c,struct xhci_dma *dma,uint32_t pages)
{
    (void)c;assert(pages==1 && !dma->physical);
    dma->cpu=kernel_malloc(4096);assert(dma->cpu);dma->pages=1;dma->physical=next_dma;
    next_dma+=4096;return 0;
}
void xhci_dma_release(struct xhci_dma *dma)
{kernel_free(dma->cpu);*dma=(struct xhci_dma){0};}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{assert(!lock->state);lock->state=1;*flags=0;}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{assert(lock->state && !flags);lock->state=0;}
void console_log(enum console_log_level level,const char *format,...)
{
    (void)level;va_list ap;va_start(ap,format);
    vsnprintf(last_log,sizeof(last_log),format,ap);va_end(ap);
}
const struct framebuffer *framebuffer_get(void)
{static const struct framebuffer fb={0};return &fb;}
void usb_host_request_service(struct usb_host *host){(void)host;}
void usb_hid_boot_release(struct usb_hid_state *state){(void)state;}
void usb_hid_boot_keyboard(struct usb_hid_state *state,const uint8_t *report,uint32_t length)
{(void)state;(void)report;(void)length;}
void usb_hid_boot_mouse(struct usb_hid_state *state,const uint8_t *report,uint32_t length)
{(void)state;(void)report;(void)length;}
void xhci_write(struct xhci_controller *c,uint32_t offset,uint32_t value)
{(void)c;(void)offset;(void)value;}

static const uint8_t device_descriptor[]={18,1,0x20,0x03,0,0,0,9,0x34,0x12,0x78,0x56,
                                          0,1,0,0,0,1};
static const uint8_t storage_descriptor[]={9,2,44,0,1,1,0,0x80,50,9,4,0,0,2,8,6,0x50,0,
    7,5,0x81,2,0,4,0,6,0x30,3,0,0,0,7,5,2,2,0,4,0,6,0x30,0,0,0,0};
static int control(struct usb_device *device,const uint8_t setup[8],void *data,
                   uint16_t length,uint32_t *actual)
{
    assert(device->speed==USB_SPEED_SUPER_PLUS && device->ep0_max_packet==512);
    if(setup[1]==6) {
        const uint8_t *source=setup[3]==1 ? device_descriptor : storage_descriptor;
        uint32_t size=setup[3]==1 ? sizeof(device_descriptor) : sizeof(storage_descriptor);
        assert(setup[3]==1 || setup[3]==2);assert(length<=size && data);
        memcpy(data,source,length);*actual=length;return 0;
    }
    assert(setup[1]==9 && !length && !data);*actual=0;return 0;
}
const struct usb_device_operations xhci_device_operations={.control=control};

int xhci_command(struct xhci_controller *c,struct xhci_trb trb,uint8_t *slot)
{
    unsigned type=(trb.control>>10)&63u;
    if(type==9){assert(slot);*slot=1;return 0;}
    struct xhci_device *device=c->devices;assert(device && c->stride==64);
    uint32_t *input=device->input.cpu;
    if(type==11) {
        assert(((input[16]>>20)&15u)==expected_id);
        assert((input[33]>>16)==512); /* initial EP0, before descriptor request */
        ((uint32_t *)device->output.cpu)[3]=5;
    } else if(type==12) {
        assert((input[65]>>16)==1024 && ((input[65]>>8)&15u)==3);
        assert((input[81]>>16)==1024);
    } else assert(0);
    return 0;
}
int usb_storage_probe(struct usb_device *device,uint8_t interface,
                      struct usb_endpoint in,struct usb_endpoint out,struct usb_storage **result)
{
    assert(device->speed==USB_SPEED_SUPER_PLUS && device->ep0_max_packet==512 && interface==0);
    assert(in.address==0x81 && in.max_packet==1024 && in.max_burst==3);
    assert(out.address==2 && out.max_packet==1024 && !out.max_burst);
    *result=NULL;++bound;return 0;
}
int usb_storage_disconnect(struct usb_storage *storage){(void)storage;return 0;}
void usb_storage_release(struct usb_storage *storage){(void)storage;}

static void attach_and_check(bool explicit_psi)
{
    uint32_t regs[16]={0};uint64_t dcbaa[2]={0};
    struct xhci_controller c={.ports=1,.slots=1,.stride=64,.dcbaa={.cpu=dcbaa}};
    regs[4]=(3u<<24)|(0x20u<<16)|2u;regs[5]=0x20425355u;
    regs[6]=(1u<<8)|1u|(explicit_psi ? 1u<<28 : 0);
    regs[8]=(20u<<16)|(1u<<14)|(1u<<8)|(3u<<4)|11u;
    expected_id=explicit_psi ? 11 : 7;
    assert(!xhci_protocol_parse(regs,sizeof(regs),16,1,c.protocol));
    enum xhci_port_rejection rejection=XHCI_REJECT_NONE;
    assert(!xhci_device_attach(&c,0,expected_id,&rejection) && rejection==XHCI_REJECT_NONE);
    assert(bound && c.devices && c.devices->usb.speed==USB_SPEED_SUPER_PLUS);
    assert(strstr(last_log,explicit_psi ? "psi=11 speed=SUPER_PLUS bps=20000000000" :
                                          "psi=7 speed=SUPER_PLUS bps=20000000000"));
    assert(strstr(last_log,explicit_psi ? "psi_raw=0014413b psi_source=explicit" :
                                          "psi_raw=00000000 psi_source=implied"));
    xhci_devices_release(&c);assert(!allocations);bound=0;
}

int main(void)
{
    attach_and_check(true);attach_and_check(false);
    puts("USB xHCI SuperSpeedPlus attach retains raw ID, 512-byte EP0, class and advertised PSI log");
}
