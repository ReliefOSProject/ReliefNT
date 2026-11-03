/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/console.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/time.h>
#include "../../drivers/usb/xhci/internal.h"

/* Only physical USB requests, xHC commands, DMA allocation and final class
 * consumers are simulated. Enumeration, context formation, interrupt rings,
 * retry state, topology and subtree retirement are production code. */
struct fixture {
    struct xhci_device *device;
    uint16_t status[4],change[4];
    bool hub,configured,disabled;
};
struct held_disk { struct usb_device *device; bool held; };
static struct fixture nodes[32];
static uint64_t next_dma=0x100000,now;
static unsigned allocations,next_slot,requests,key_releases,storages;
static bool superspeed,fail_storage;
static unsigned lane_count;
static struct held_disk *old_disk;

void *kernel_malloc(size_t bytes)
{void *p=calloc(1,bytes);if(p)++allocations;return p;}
void kernel_free(void *p)
{if(p){assert(allocations);--allocations;free(p);}}
int xhci_dma_alloc(struct xhci_controller *c,struct xhci_dma *dma,uint32_t pages)
{
    (void)c;assert(pages==1 && !dma->physical);dma->cpu=kernel_malloc(4096);
    assert(dma->cpu);dma->pages=1;dma->physical=next_dma;next_dma+=4096;return 0;
}
void xhci_dma_release(struct xhci_dma *dma)
{kernel_free(dma->cpu);*dma=(struct xhci_dma){0};}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{assert(!lock->state);lock->state=1;*flags=0;}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{assert(lock->state && !flags);lock->state=0;}
void console_log(enum console_log_level level,const char *format,...)
{(void)level;(void)format;}
const struct framebuffer *framebuffer_get(void)
{static const struct framebuffer fb={0};return &fb;}
void usb_host_request_service(struct usb_host *host){(void)host;++requests;}
uint64_t time_ticks(void){return now;}
void xhci_delay(unsigned milliseconds){now+=(uint64_t)milliseconds*RELIEFNT_TICK_HZ/1000u;}
void usb_hid_boot_release(struct usb_hid_state *state){(void)state;++key_releases;}
void usb_hid_boot_keyboard(struct usb_hid_state *state,const uint8_t *report,uint32_t length)
{(void)state;(void)report;(void)length;}
void usb_hid_boot_mouse(struct usb_hid_state *state,const uint8_t *report,uint32_t length)
{(void)state;(void)report;(void)length;}
void xhci_write(struct xhci_controller *c,uint32_t offset,uint32_t value)
{assert(c->event_lock.state && offset>=c->doorbells && value);}

/** @brief Model command ownership and reject a Hub slot disabled before any child slot. */
int xhci_command(struct xhci_controller *c,struct xhci_trb trb,uint8_t *slot)
{
    unsigned type=(trb.control>>10)&63u,id=trb.control>>24;
    if(type==9){assert(slot && next_slot<31);*slot=(uint8_t)++next_slot;return 0;}
    struct xhci_device *d=c->devices;
    while(d && d->slot_id!=id)d=d->next;
    assert(d && !c->event_lock.state);
    struct fixture *node=&nodes[id];node->device=d;
    uint32_t *input=d->input.cpu,*output=d->output.cpu;unsigned stride=c->stride/4u;
    if(type==11) {
        assert((input[stride]&0xfffffu)==d->route);
        assert(((input[stride]>>20)&15u)==d->speed_id);
        assert((input[stride+1]>>16)==1);
        assert((input[stride+2]&0xffffu)==(unsigned)(d->tt_slot|(unsigned)d->tt_port<<8));
        memcpy(output,input+stride,12);output[3]=id;
        node->hub=!d->parent || (!superspeed && d->route==2);
        if(node->hub && !d->parent) {
            node->status[1]=superspeed ? 0x201 : 0x301;
            node->status[2]=superspeed ? 0x200 : 0x101;
            node->status[3]=superspeed ? 0x200 : 0x501;
        } else if(node->hub)node->status[3]=0x301;
    } else if(type==12) {
        if(node->hub) {
            assert(node->configured && (input[stride]&(1u<<26)));
            assert(input[stride+1]>>24==3 && !(input[stride]&(1u<<25)));
            assert(((input[stride+2]>>16)&3u)==(d->usb.speed==USB_SPEED_HIGH ? 3u : 0));
        }
    } else if(type==13)assert(input[1]==2);
    else if(type==10) {
        for(struct xhci_device *child=c->devices;child;child=child->next)
            if(child->parent==d)assert(child->disabled);
        node->disabled=true;
    } else assert(0);
    return 0;
}

/** @brief Supply complete descriptors and port feature behavior at the USB wire boundary. */
static int control(struct usb_device *usb,const uint8_t setup[8],void *data,
                     uint16_t length,uint32_t *actual)
{
    struct xhci_device *d=usb->host_context;struct fixture *n=&nodes[d->slot_id];
    assert(!usb->disconnected && !d->controller->event_lock.state);
    *actual=0;
    if(setup[1]==6) {
        uint8_t desc[64]={0};unsigned size=0;
        if(setup[3]==1) {
            if(fail_storage && d->parent && d->parent_port==3 && d->parent->depth==0)return -EIO;
            uint8_t value[]={18,1,0,2,0,0,0,8,0x34,0x12,0x78,0x56,0,1,0,0,0,1};
            memcpy(desc,value,sizeof(value));size=18;
            desc[7]=usb->speed>=USB_SPEED_SUPER ? 9 : usb->speed==USB_SPEED_HIGH ? 64 : 8;
            if(n->hub){desc[4]=9;desc[6]=superspeed ? 3 : usb->speed==USB_SPEED_HIGH ? 2 : 0;}
        } else if(setup[3]==2) {
            uint8_t config[]={9,2,25,0,1,1,0,0x80,50,9,4,0,0,1,3,1,1,0,
                              7,5,0x81,3,8,0,10};
            memcpy(desc,config,sizeof(config));size=25;
            if(n->hub){desc[14]=9;desc[15]=0;desc[16]=superspeed ? 3 : usb->speed==USB_SPEED_HIGH ? 1 : 0;}
            else if(d->usb.speed==USB_SPEED_HIGH || d->usb.speed>=USB_SPEED_SUPER) {
                uint8_t storage[]={9,2,32,0,1,1,0,0x80,50,9,4,0,0,2,8,6,0x50,0,
                                   7,5,0x81,2,0,2,0,7,5,2,2,0,2,0};
                memcpy(desc,storage,sizeof(storage));size=32;
                if(superspeed) {
                    uint8_t ss[]={9,2,44,0,1,1,0,0x80,50,9,4,0,0,2,8,6,0x50,0,
                        7,5,0x81,2,0,4,0,6,0x30,0,0,0,0,7,5,2,2,0,4,0,6,0x30,0,0,0,0};
                    memcpy(desc,ss,sizeof(ss));size=44;
                }
            }
            if(superspeed && n->hub) {
                uint8_t companion[]={6,0x30,0,0,2,0};
                desc[2]=31;desc[22]=2;memcpy(desc+25,companion,6);size=31;
            }
        } else if(setup[3]==0x29 || setup[3]==0x2a) {
            assert(n->hub && n->configured && setup[0]==0xa0);
            uint8_t hub[]={9,0x29,3,0x69,0,5,0,0,0xff};
            memcpy(desc,hub,sizeof(hub));size=9;
            if(superspeed){desc[0]=12;desc[1]=0x2a;desc[3]=9;size=12;}
        } else if(setup[3]==15) {
            uint8_t bos[]={5,15,25,0,1,20,16,10,0,1,0,0,0,0,0,0,0,
                           0x34,0,5,0,0xb4,0,5,0};
            memcpy(desc,bos,sizeof(bos));size=25;
            if(!lane_count) {
                uint8_t legacy[]={5,15,15,0,1,10,16,3,0,8,0,3,10,0xff,7};
                memcpy(desc,legacy,sizeof(legacy));size=15;
            }
        } else assert(0);
        assert(length<=size);memcpy(data,desc,length);*actual=length;return 0;
    }
    if(setup[1]==9){assert(!length);n->configured=true;return 0;}
    if(setup[0]==0x21){assert(!length && (setup[1]==10 || setup[1]==11));return 0;}
    if(setup[1]==12){assert(superspeed && n->configured && setup[2]==d->depth);return 0;}
    if(setup[1]==0) {
        assert(n->hub && n->configured);uint8_t *bytes=data;memset(bytes,0,length);
        if(setup[0]==0xa3) {
            unsigned port=setup[4];assert(port && port<=3);
            assert(length==(superspeed && lane_count ? 8 : 4) && setup[2]==(superspeed && lane_count ? 2 : 0));
            bytes[0]=n->status[port];bytes[1]=n->status[port]>>8;
            bytes[2]=n->change[port];bytes[3]=n->change[port]>>8;
            if(superspeed && lane_count){bytes[4]=0x44;bytes[5]=(lane_count-1u)*0x11u;} /* Hub's SSID 4, not an xHC speed ID. */
        } else assert(setup[0]==0xa0 && length==4);
        *actual=length;return 0;
    }
    assert(n->hub && n->configured && setup[0]==0x23 && !length);
    unsigned port=setup[4],feature=setup[2];assert(port && port<=3);
    if(setup[1]==3) {
        if(feature==8)n->status[port]|=superspeed ? 0x200 : 0x100;
        else {assert(feature==4 || (superspeed && feature==28));n->status[port]|=2;n->change[port]|=16;}
    } else {
        assert(setup[1]==1);
        assert(!superspeed || (feature!=17 && feature!=18));
        unsigned bit=feature==29 ? 5 : feature==25 ? 6 : feature==26 ? 7 : feature-16;
        assert(bit<8);n->change[port]&=~(1u<<bit);
    }
    return 0;
}
const struct usb_device_operations xhci_device_operations={.control=control};
int usb_storage_probe(struct usb_device *device,uint8_t interface,struct usb_endpoint in,
                      struct usb_endpoint out,struct usb_storage **storage)
{
    assert(!interface && in.address==0x81 && out.address==2);
    struct held_disk *disk=kernel_malloc(sizeof(*disk));disk->device=device;disk->held=false;
    *storage=(struct usb_storage *)disk;++storages;if(!old_disk)old_disk=disk;return 0;
}
int usb_storage_disconnect(struct usb_storage *storage)
{struct held_disk *disk=(void *)storage;assert(disk->device->disconnected);return disk->held ? -EBUSY : 0;}
void usb_storage_release(struct usb_storage *storage)
{struct held_disk *disk=(void *)storage;assert(!disk->held);kernel_free(disk);}

/** @brief Complete the genuine armed status ring with one wire-format port bitmap. */
static void notify(struct xhci_device *hub,uint8_t bitmap)
{
    unsigned dci=hub->hub->dci;struct xhci_endpoint_state *ep=&hub->endpoints[dci];
    assert(ep->active);((uint8_t *)ep->dma.cpu)[1024]=bitmap;
    ep->completion=(struct xhci_completion){.done=true,.actual=1};
    uint64_t flags;kernel_spin_lock_irqsave(&hub->controller->event_lock,&flags);
    xhci_hub_event(hub,dci);kernel_spin_unlock_irqrestore(&hub->controller->event_lock,flags);
}

static void exercise(bool ss,bool failing,unsigned lanes)
{
    memset(nodes,0,sizeof(nodes));next_slot=storages=0;old_disk=NULL;superspeed=ss;fail_storage=failing;lane_count=lanes;
    uint64_t dcbaa[32]={0};
    struct xhci_controller c={.ports=1,.slots=31,.stride=64,.doorbells=0x1000,.dcbaa={.cpu=dcbaa}};
    struct xhci_protocol *p=&c.protocol[0];
    p->valid_speeds=(1u<<7)|(1u<<8)|(1u<<9)|(1u<<11)|(1u<<12);
    p->speeds[7]=USB_SPEED_FULL;p->speeds[8]=USB_SPEED_LOW;p->speeds[9]=USB_SPEED_HIGH;
    p->speeds[11]=lanes ? USB_SPEED_SUPER_PLUS : USB_SPEED_SUPER;
    p->speeds[12]=lanes>1 ? USB_SPEED_SUPER_PLUS : USB_SPEED_SUPER;
    p->rates[11]=lanes>1 ? 20000000000ull : lanes ? 10000000000ull : 5000000000ull;
    p->rates[12]=lanes>1 ? 10000000000ull : 5000000000ull;
    enum xhci_port_rejection rejection=XHCI_REJECT_NONE;
    assert(!xhci_device_attach(&c,0,ss ? 11 : 9,&rejection));
    struct xhci_device *root=c.devices;
    for(unsigned i=0;i<3;++i)xhci_hubs_service(&c);
    if(failing) {
        for(unsigned i=0;i<5;++i){now+=RELIEFNT_TICK_HZ;xhci_hubs_poll(&c);xhci_hubs_service(&c);}
        assert(root->hub->state[3].phase==XHCI_PORT_EXHAUSTED && root->hub->state[3].attempts==3);
        assert(next_slot==7 && !storages); /* root + 3 input/hub + exactly 3 attempts */
    } else {
        assert(storages==1 && old_disk);
        struct xhci_device *disk=old_disk->device->host_context;
        if(ss) {
            assert(disk->route==1 && disk->speed_id==(lanes ? 12 : 11));
            assert(disk->tt_slot==(lanes ? root->slot_id : 0) && disk->tt_port==(lanes ? 1 : 0));
            assert(disk->usb.speed==(lanes>1 ? USB_SPEED_SUPER_PLUS : USB_SPEED_SUPER));
        }
        else {
            assert(next_slot==5);
            struct xhci_device *grandchild=c.devices;
            assert(grandchild->route==0x32 && grandchild->usb.speed==USB_SPEED_LOW);
            assert(grandchild->tt_slot==root->slot_id && grandchild->tt_port==2);
        }
        old_disk->held=true;unsigned port=ss ? 1 : 3;
        nodes[root->slot_id].status[port]&=~1u;nodes[root->slot_id].change[port]=1;
        notify(root,(uint8_t)(1u<<port));xhci_hubs_service(&c);
        assert(disk->disabled && disk->usb.disconnected && !root->usb.disconnected);
        nodes[root->slot_id].status[port]|=1;nodes[root->slot_id].change[port]=ss ? 0xe1 : 1;
        notify(root,(uint8_t)(1u<<port));xhci_hubs_service(&c);
        assert(storages==2 && c.devices!=disk && !c.devices->usb.disconnected);
    }
    unsigned released=key_releases;
    xhci_device_disconnect(root);
    for(struct xhci_device *d=c.devices;d;d=d->next)assert(d->usb.disconnected);
    xhci_devices_collect(&c);
    if(!failing) {
        assert(c.devices && !root->disabled);
        assert(old_disk->device->disconnected);old_disk->held=false;
        xhci_devices_collect(&c);
    }
    assert(!c.devices && !allocations);
    assert(ss || key_releases>released);
}

int main(void)
{
    exercise(false,false,1);exercise(true,false,0);exercise(true,false,1);
    exercise(true,false,2);exercise(false,true,1);
    assert(requests);puts("USB xHCI Hub enumeration, SS status, TT tree, hotplug, retry and retained subtree passed");
}
