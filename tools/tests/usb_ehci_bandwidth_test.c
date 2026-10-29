/* SPDX-License-Identifier: Apache-2.0 */
/* Run the real periodic tree and real transfer descriptor helpers with fake MMIO. */
#include "../../drivers/usb/ehci/internal.h"
#include <reliefnt/console.h>
#include <reliefnt/framebuffer.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t command;
static unsigned requests;
int ehci_timer_start(struct ehci_timer *timer) { (void)timer; return 0; }
bool ehci_timer_expired(struct ehci_timer *timer,uint64_t us) { (void)timer; (void)us; return true; }
uint32_t ehci_read(struct ehci_controller *c,unsigned off) { (void)c; assert(off==EHCI_CMD); return command; }
void ehci_write(struct ehci_controller *c,unsigned off,uint32_t value) { (void)c; assert(off==EHCI_CMD); command=value; }
int ehci_wait(struct ehci_controller *c,unsigned off,uint32_t mask,uint32_t want,unsigned ms)
{ (void)c;(void)off;(void)mask;(void)want;(void)ms;return 0; }
int ehci_request(struct usb_device *d,uint8_t type,uint8_t request,uint16_t value,
                 uint16_t index,void *data,uint16_t length,uint32_t *actual)
{ (void)d;(void)type;(void)request;(void)value;(void)index;(void)data;(void)length;*actual=0;++requests;return 0; }
static struct framebuffer fb;
const struct framebuffer *framebuffer_get(void) { return &fb; }
void console_log(enum console_log_level level,const char *format,...) { (void)level;(void)format; }
void usb_hid_boot_release(struct usb_hid_state *state) { (void)state; }

static struct ehci_controller *make_controller(void)
{
    struct ehci_controller *c=calloc(1,sizeof(*c));assert(c);
    c->dma=aligned_alloc(4096,sizeof(*c->dma));assert(c->dma);
    memset(c->dma,0,sizeof(*c->dma));c->dma_phys=0x400000;
    for(unsigned frame=0;frame<EHCI_FRAME_COUNT;++frame)c->dma->frames[frame]=EHCI_TERMINATE;
    command=requests=0;return c;
}
static void free_controller(struct ehci_controller *c) {free(c->dma);free(c);}
static int add(struct ehci_controller *c,struct ehci_device *d,unsigned speed,
               unsigned hub,unsigned port,unsigned packet,unsigned interval,unsigned protocol)
{
    *d=(struct ehci_device){.usb={.speed=speed,.address=(uint8_t)(1+port),
        .hub_address=hub,.hub_port=port,.host_context=c}};
    struct usb_config_interface i={.subclass=1,.protocol=protocol,.number=(uint8_t)port,
        .interrupt={.address=0x81,.interval=interval,.max_packet=packet}};
    return ehci_hid_add(c,d,&i);
}
static unsigned active(const struct ehci_controller *c)
{unsigned n=0;for(unsigned i=0;i<EHCI_MAX_HID;++i)if(c->hid[i].device)++n;return n;}
/* Traverse all frames exactly as EHCI does, detecting cycles, stray QHs, and
 * any endpoint visited at a period other than its admitted power-of-two rate. */
static void verify_tree(struct ehci_controller *c)
{
    for(unsigned frame=0;frame<EHCI_FRAME_COUNT;++frame) {
        bool seen[EHCI_MAX_HID]={0};uint32_t link=c->dma->frames[frame];unsigned depth=0;
        uint32_t hs[8]={0},tt[128]={0};unsigned starts[128]={0};bool low[128]={0};
        while(!(link&EHCI_TERMINATE)) {
            assert((link&6u)==EHCI_LINK_QH && ++depth<=EHCI_MAX_HID);
            uint32_t address=link&~31u;unsigned slot;
            for(slot=0;slot<EHCI_MAX_HID;++slot)
                if(address==ehci_bus(c,&c->dma->hid_heads[slot]))break;
            assert(slot<EHCI_MAX_HID && c->hid[slot].device && !seen[slot]);
            seen[slot]=true;
            const struct ehci_hid *hid=&c->hid[slot];
            assert((c->dma->hid_heads[slot].capabilities&0xffffu)==
                   (hid->smask|((uint32_t)hid->cmask<<8)));
            for(unsigned micro=0;micro<8;++micro) {
                if(hid->smask&(1u<<micro))hs[micro]+=hid->start_ns;
                if(hid->cmask&(1u<<micro))hs[micro]+=hid->complete_ns;
            }
            if(hid->tt_ns) {
                unsigned hub=hid->device->usb.hub_address;assert(hub<128);
                /* All QHs on this TT have the same start/complete windows, so
                 * the frame's one fixed QH order is also the CSPLIT order. */
                assert(hid->smask==1 && hid->cmask==0x1c);
                tt[hub]+=hid->tt_ns;++starts[hub];
                if(hid->device->usb.speed==USB_SPEED_LOW)low[hub]=true;
            }
            link=c->dma->hid_heads[slot].link;
        }
        for(unsigned i=0;i<EHCI_MAX_HID;++i)if(c->hid[i].device) {
            unsigned period=c->hid[i].period,phase=c->hid[i].phase;
            assert(period && !(period&(period-1u)) && phase<period);
            assert(seen[i]==((frame%period)==phase));
        }
        for(unsigned micro=0;micro<8;++micro)assert(hs[micro]<=100000);
        for(unsigned hub=0;hub<128;++hub) {
            assert(tt[hub]<=(low[hub] ? 250000u : 125000u) && starts[hub]<=16);
            if(low[hub])assert(starts[hub]==1);
        }
    }
}
static void high_speed_limit(void)
{
    struct ehci_controller *c=make_controller();struct ehci_device d[8];unsigned accepted=0;
    for(unsigned i=0;i<8;++i) {
        unsigned before=requests;int rc=add(c,&d[i],USB_SPEED_HIGH,0,i+1,1024,1,1);
        if(!rc)++accepted;else {assert(rc==-ENOSPC && requests==before);break;}
    }
    assert(accepted && accepted<8 && active(c)==accepted);
    /* 55-byte HS overhead, worst stuffed 1024-byte payload and 10us host
     * allowance in USB 2.0 5.11.3 give a lower bound of 30,845ns. */
    assert(c->hid[0].start_ns>=30845);
    verify_tree(c);free_controller(c);
}
static void high_speed_microframe_phases(void)
{
    struct ehci_controller *c=make_controller();struct ehci_device d[5];
    for(unsigned i=0;i<5;++i)assert(!add(c,&d[i],USB_SPEED_HIGH,0,i+1,1024,2,1));
    assert(c->hid[0].smask==0x55 && c->hid[4].smask==0xaa);
    verify_tree(c);free_controller(c);
}
static void shared_and_independent_tt(void)
{
    struct ehci_controller *c=make_controller();struct ehci_device d[12];
    assert(!add(c,&d[0],USB_SPEED_LOW,4,1,8,10,1));
    assert(!add(c,&d[1],USB_SPEED_LOW,4,2,8,10,2));
    assert(!add(c,&d[2],USB_SPEED_LOW,5,1,8,10,1));
    /* The implementation charges even the complete protocol overhead at LS. */
    assert(c->hid[0].tt_ns>=180667);
    verify_tree(c);
    unsigned both=0,independent_overlap=0;
    for(unsigned frame=0;frame<EHCI_FRAME_COUNT;++frame) {
        bool has[3]={0};uint32_t link=c->dma->frames[frame];
        while(!(link&EHCI_TERMINATE)) {
            for(unsigned i=0;i<3;++i)if((link&~31u)==ehci_bus(c,&c->dma->hid_heads[i]))has[i]=true;
            unsigned j;for(j=0;j<EHCI_MAX_HID;++j)if((link&~31u)==ehci_bus(c,&c->dma->hid_heads[j]))break;
            assert(j<EHCI_MAX_HID);link=c->dma->hid_heads[j].link;
        }
        both+=has[0]&&has[1];independent_overlap+=has[0]&&has[2];
    }
    assert(!both && independent_overlap);
    for(unsigned i=3;i<9;++i)assert(!add(c,&d[i],USB_SPEED_LOW,4,i,8,10,1));
    unsigned before=requests;
    assert(add(c,&d[9],USB_SPEED_LOW,4,9,8,10,1)==-ENOSPC && requests==before);
    assert(active(c)==9);verify_tree(c);
    d[0].usb.disconnected=true;assert(!ehci_hid_collect(c));
    assert(!add(c,&d[9],USB_SPEED_LOW,4,9,8,10,1));verify_tree(c);
    free_controller(c);
}
static void fullspeed_shared_one_ms(void)
{
    struct ehci_controller *c=make_controller();struct ehci_device d[2];
    assert(!add(c,&d[0],USB_SPEED_FULL,6,1,8,1,1));
    assert(!add(c,&d[1],USB_SPEED_FULL,6,2,8,1,2));
    assert(c->hid[0].tt_ns>=29001);
    verify_tree(c);
    for(unsigned frame=0;frame<EHCI_FRAME_COUNT;++frame)assert(!(c->dma->frames[frame]&EHCI_TERMINATE));
    free_controller(c);
}
static void mixed_frame_periods(void)
{
    struct ehci_controller *c=make_controller();struct ehci_device d[8];
    /* Different power-of-two periods must form one consistent successor tree,
     * even when phases on the same TT differ. */
    assert(!add(c,&d[0],USB_SPEED_LOW,7,1,8,8,1));
    assert(!add(c,&d[1],USB_SPEED_LOW,7,2,8,8,2));
    assert(!add(c,&d[2],USB_SPEED_FULL,7,3,8,4,1));
    assert(!add(c,&d[3],USB_SPEED_FULL,8,1,8,1,2));
    assert(!add(c,&d[4],USB_SPEED_HIGH,0,5,8,7,1));
    assert(!add(c,&d[5],USB_SPEED_HIGH,0,6,8,5,2));
    verify_tree(c);free_controller(c);
}
int main(void)
{
    high_speed_limit();high_speed_microframe_phases();shared_and_independent_tt();fullspeed_shared_one_ms();
    mixed_frame_periods();
    puts("PASS EHCI periodic admission, frame tree, shared and independent TTs");return 0;
}
