/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/console.h>
#include <reliefnt/framebuffer.h>
#include "../../drivers/usb/xhci/internal.h"

#define CCS 1u
#define PED (1u<<1)
#define PR (1u<<4)
#define WPR (1u<<31)
#define PP (1u<<9)
#define CSC (1u<<17)
#define WRC (1u<<19)
#define PRC (1u<<21)
#define PLC (1u<<22)
#define CHANGES (0x7fu<<17)

static struct xhci_controller controller;
static struct xhci_device working;
static struct xhci_trb event_ring[XHCI_EVENT_COUNT];
static uint64_t dcbaa[5],now_ticks,next_dma=0x100000;
static uint32_t ports[2],reset_speed=1;
static unsigned tail,resets,enable_commands,disable_commands,allocations,warnings,service_requests;
static unsigned hid_releases;
static bool delayed_event,replacement,transient_once,inject_debounce,inject_reset,inject_attach;
static bool inject_ack_window,seen_reset_snapshot;
static bool inject_service_snapshot;
static bool fail_disable;
static unsigned service_snapshot_reads;
static unsigned transient_left;
static int transient_error=-EOPNOTSUPP;
static int reset_wait_error;
static uint8_t interface_class;
static bool oversized_config;
static char last_warning[256];

uint64_t time_ticks(void){return now_ticks;}
void *kernel_malloc(size_t bytes){void *p=calloc(1,bytes);if(p)++allocations;return p;}
void kernel_free(void *p){if(p){assert(allocations);--allocations;free(p);}}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{assert(!lock->state);lock->state=1;*flags=0;}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{assert(lock->state && !flags);lock->state=0;}
int xhci_dma_alloc(struct xhci_controller *c,struct xhci_dma *dma,uint32_t pages)
{
    (void)c;assert(pages==1 && !dma->physical);dma->cpu=kernel_malloc(4096);assert(dma->cpu);
    dma->pages=1;dma->physical=next_dma;next_dma+=4096;return 0;
}
void xhci_dma_release(struct xhci_dma *dma)
{kernel_free(dma->cpu);*dma=(struct xhci_dma){0};}
void usb_host_request_service(struct usb_host *host){(void)host;++service_requests;}
void usb_hid_boot_release(struct usb_hid_state *state){(void)state;++hid_releases;}
void usb_hid_boot_keyboard(struct usb_hid_state *s,const uint8_t *r,uint32_t n){(void)s;(void)r;(void)n;}
void usb_hid_boot_mouse(struct usb_hid_state *s,const uint8_t *r,uint32_t n){(void)s;(void)r;(void)n;}
const struct framebuffer *framebuffer_get(void){static const struct framebuffer fb={0};return &fb;}
int usb_storage_probe(struct usb_device *d,uint8_t i,struct usb_endpoint in,struct usb_endpoint out,
                      struct usb_storage **s)
{(void)d;(void)i;(void)in;(void)out;(void)s;assert(0);return -EIO;}
int usb_storage_disconnect(struct usb_storage *storage){(void)storage;return 0;}
void usb_storage_release(struct usb_storage *storage){(void)storage;}
int xhci_endpoint_recover(struct xhci_device *d,unsigned e,bool halt)
{(void)d;(void)e;(void)halt;return 0;}
void console_log(enum console_log_level level,const char *format,...)
{
    if(level==CONSOLE_LOG_WARN) {
        ++warnings;va_list ap;va_start(ap,format);
        vsnprintf(last_warning,sizeof(last_warning),format,ap);va_end(ap);
    }
}
void xhci_delay(unsigned ms)
{(void)ms;if(inject_debounce){inject_debounce=false;ports[0]|=CSC;}}
int xhci_timer_start(struct xhci_timer *timer){*timer=(struct xhci_timer){0};return 0;}
bool xhci_timer_expired(struct xhci_timer *timer,uint64_t us){(void)timer;(void)us;return false;}

uint32_t xhci_read(struct xhci_controller *c,uint32_t offset)
{
    if(offset==c->operational+4u)return 0;
    assert(offset>=c->operational+0x400u && offset<c->operational+0x420u);
    unsigned port=(offset-c->operational-0x400u)/16u;
    if(!port && inject_service_snapshot && ++service_snapshot_reads==2) {
        ports[0]|=CSC;inject_service_snapshot=false;
    }
    if(!port && inject_ack_window && (ports[0]&PRC)) {
        if(seen_reset_snapshot){ports[0]|=CSC;inject_ack_window=false;}
        else seen_reset_snapshot=true;
    }
    return ports[port];
}
void xhci_write(struct xhci_controller *c,uint32_t offset,uint32_t value)
{
    if(offset>=c->doorbells && offset<c->doorbells+20u)return;
    assert(offset>=c->operational+0x400u && offset<c->operational+0x420u);
    unsigned port=(offset-c->operational-0x400u)/16u;
    ports[port]&=~(value&CHANGES); /* exact W1CS snapshot */
    if(value&(PR|WPR)){ports[port]|=value&(PR|WPR);++resets;}
}
void xhci_write64(struct xhci_controller *c,uint32_t offset,uint64_t value)
{assert(offset==c->runtime+0x38u);(void)value;}
int xhci_wait(struct xhci_controller *c,uint32_t offset,uint32_t mask,uint32_t value,unsigned ms)
{
    (void)c;(void)offset;(void)value;(void)ms;assert((mask==PR || mask==WPR) && (ports[0]&mask));
    ports[0]=(ports[0]&~(mask|(15u<<10)))|PED|PRC|(reset_speed<<10);
    if(mask==WPR)ports[0]|=WRC;
    if(inject_reset){inject_reset=false;ports[0]|=CSC;}
    delayed_event=true;int rc=reset_wait_error;reset_wait_error=0;return rc;
}
static void publish_reset_event(void)
{
    assert(delayed_event && !(ports[0]&(PRC|WRC))); /* event arrives after W1CS */
    assert(tail<XHCI_EVENT_COUNT);
    event_ring[tail++]=(struct xhci_trb){.lo=1u<<24,.control=(34u<<10)|1u};
    delayed_event=false;xhci_events(&controller);
}
int __wrap_xhci_command(struct xhci_controller *c,struct xhci_trb trb,uint8_t *slot)
{
    unsigned type=(trb.control>>10)&63u;
    if(__atomic_load_n(&c->stop.failed,__ATOMIC_ACQUIRE))return -EIO;
    if(type==9) {
        ++enable_commands;if(delayed_event)publish_reset_event();assert(slot);*slot=1;return 0;
    }
    struct xhci_device *device=c->devices;assert(device && device!=&working);
    if(type==11)((uint32_t *)device->output.cpu)[3]=5;
    else if(type==10){++disable_commands;if(fail_disable)return -EIO;}
    else assert(type==12);
    return 0;
}
static const uint8_t hub_descriptor[]={18,1,0,2,9,0,0,8,0x0f,0x0e,2,0,0,1,0,0,0,1};
static const uint8_t keyboard_descriptor[]={18,1,0,2,0,0,0,8,0x34,0x12,0x78,0x56,0,1,0,0,0,1};
static const uint8_t keyboard_config[]={9,2,25,0,1,1,0,0x80,50,9,4,0,0,1,3,1,1,0,7,5,0x81,3,8,0,10};
static int control(struct usb_device *d,const uint8_t setup[8],void *data,uint16_t length,uint32_t *actual)
{
    if(transient_once){transient_once=false;return -EOPNOTSUPP;}
    if(transient_left){--transient_left;return transient_error;}
    if(setup[1]==6) {
        const uint8_t *src=setup[3]==1 ? (replacement ? keyboard_descriptor : hub_descriptor) : keyboard_config;
        unsigned n=setup[3]==1 ? sizeof(hub_descriptor) : sizeof(keyboard_config);
        if(inject_attach && setup[3]==1 && length==18){inject_attach=false;ports[0]|=CSC;}
        assert(length<=n && data);memcpy(data,src,length);
        if(setup[3]==1 && length>=8 && d->speed>=USB_SPEED_SUPER)((uint8_t *)data)[7]=9;
        if(setup[3]==2 && length>=9 && oversized_config) {
            ((uint8_t *)data)[2]=0xff;((uint8_t *)data)[3]=0xff;
        }
        if(setup[3]==2 && length>14 && interface_class)((uint8_t *)data)[14]=interface_class;
        *actual=length;return 0;
    }
    assert(!length && !data && (setup[1]==9 || setup[1]==10 || setup[1]==11));*actual=0;return 0;
}
const struct usb_device_operations xhci_device_operations={.control=control};

static void setup(void)
{
    controller=(struct xhci_controller){.ports=2,.slots=4,.stride=32,.operational=0x40,
        .doorbells=0x3000,.runtime=0x2000,.events={.cpu=event_ring,.physical=0x200000},
        .event_cycle=1,.dcbaa={.cpu=dcbaa}};
    working=(struct xhci_device){.controller=&controller,.usb={.root_port=1}};
    controller.devices=&working;
    for(unsigned p=0;p<2;++p) {
        controller.protocol[p].major=2;controller.protocol[p].valid_speeds=1u<<1;
        controller.protocol[p].speeds[1]=USB_SPEED_FULL;
    }
    ports[0]=CCS|PP|CSC;ports[1]=CCS|PED|PP|(1u<<10);
    controller.port_work[0]=1;
}
static void settle(unsigned passes)
{
    for(unsigned i=0;i<passes;++i){xhci_poll(&controller);if(controller.port_work[0])xhci_service(&controller);}
}
static void storm(void)
{
    setup();xhci_service(&controller);
    assert(resets==1 && enable_commands==1 && disable_commands==1 && warnings==1);
    assert(strstr(last_warning,"stage=device reason=hub action=unsupported"));
    assert(!allocations && controller.devices==&working && !working.usb.disconnected);
    assert(!dcbaa[1]);ports[0]|=PLC;ports[1]|=PLC; /* neither link change is a reconnect */
    settle(5);
    assert(service_requests==1); /* one stale-event service, then idle */
    assert(resets==1 && enable_commands==1 && disable_commands==1 && warnings==1);
    assert(!controller.port_work[0] && !allocations && controller.devices==&working);
}
static void speed(void)
{
    setup();reset_speed=5;xhci_service(&controller);
    assert(resets==1 && !enable_commands && warnings==1);
    assert(strstr(last_warning,"stage=reset reason=speed action=unsupported"));
    assert(!(ports[0]&PRC));
    publish_reset_event();
    settle(5);assert(resets==1 && warnings==1 && !allocations);
}
static void transient(void)
{
    setup();transient_once=true;xhci_service(&controller);
    assert(resets==1 && enable_commands==1 && disable_commands==1 && warnings==1);
    assert(strstr(last_warning,"stage=device reason=retryable action=backoff"));
    settle(5);assert(resets==1 && enable_commands==1 && warnings==1);
    now_ticks=100;settle(5); /* a bounded timed retry reaches the actual hub rejection */
    assert(resets==2 && enable_commands==2 && disable_commands==2 && warnings==2);
    now_ticks=1000;settle(5);
    assert(resets==2 && enable_commands==2 && warnings==2 && !allocations);
}
static void check_keyboard(void);
static void retry_exhaustion(void)
{
    setup();transient_left=20;transient_error=-EAGAIN;xhci_service(&controller);
    settle(5);assert(resets==1 && enable_commands==1 && warnings==1);
    now_ticks=24;settle(2);assert(resets==1);
    now_ticks=25;settle(3);assert(resets==2 && warnings==2);
    now_ticks=74;settle(2);assert(resets==2);
    now_ticks=75;settle(3);assert(resets==3 && warnings==3);
    assert(strstr(last_warning,"stage=device reason=retryable action=exhausted"));
    now_ticks=1000;settle(5);assert(resets==3 && warnings==3);
    transient_left=0;replacement=true;ports[0]|=CSC;settle(5);
    assert(resets==4 && enable_commands==4 && warnings==3);check_keyboard();
}
static void check_keyboard(void)
{
    settle(5);
    struct xhci_device *device=controller.devices;
    assert(device && device!=&working && device->usb.root_port==0 && !device->usb.disconnected);
    assert(device->endpoints[3].hid && !working.usb.disconnected);
    xhci_device_disconnect(device);xhci_devices_collect(&controller);
    assert(controller.devices==&working && !allocations);
}
static void warm_speed(void)
{
    setup();controller.protocol[0].major=3;controller.protocol[0].valid_speeds=1u<<4;
    controller.protocol[0].speeds[4]=USB_SPEED_SUPER;reset_speed=5;
    xhci_service(&controller);
    assert(resets==1 && !enable_commands && warnings==1 && !(ports[0]&(PRC|WRC)));
    publish_reset_event();settle(5);assert(resets==1 && warnings==1);
}
static void warm_reconnect(void)
{
    setup();controller.protocol[0].major=3;controller.protocol[0].valid_speeds=1u<<4;
    controller.protocol[0].speeds[4]=USB_SPEED_SUPER;reset_speed=4;inject_reset=true;
    xhci_service(&controller);
    assert((ports[0]&CSC) && !(ports[0]&(PRC|WRC)) && !enable_commands);
    settle(5);assert(resets==1 && enable_commands==1 && disable_commands==1 && warnings==1);
    assert(!allocations && controller.devices==&working);
}
static void reset_transient(void)
{
    setup();reset_wait_error=-EOPNOTSUPP;xhci_service(&controller);
    assert(resets==1 && !enable_commands && warnings==1 && !(ports[0]&PRC));
    assert(strstr(last_warning,"stage=reset reason=retryable action=backoff"));
    publish_reset_event();settle(5);assert(resets==1);
    now_ticks=25;settle(5);assert(resets==2 && enable_commands==1 && warnings==2);
    assert(strstr(last_warning,"stage=device reason=hub action=unsupported"));
}
static void undefined_speed(void)
{
    setup();reset_speed=0;xhci_service(&controller);
    assert(resets==1 && !enable_commands && warnings==1 && !(ports[0]&PRC));
    assert(strstr(last_warning,"stage=reset reason=retryable action=backoff"));
    publish_reset_event();settle(5);reset_speed=1;now_ticks=25;settle(5);
    assert(resets==2 && enable_commands==1 && warnings==2);
}
static void rejected_class(const char *kind)
{
    setup();replacement=true;
    if(!strcmp(kind,"hub-interface"))interface_class=9;
    else if(!strcmp(kind,"interface"))interface_class=0xff;
    else oversized_config=true;
    xhci_service(&controller);
    const char *reason=interface_class==9 ? "reason=hub action=unsupported" :
        interface_class ? "reason=interface action=unsupported" : "reason=configuration action=unsupported";
    assert(resets==1 && enable_commands==1 && disable_commands==1 && warnings==1);
    assert(strstr(last_warning,reason) && !allocations && !dcbaa[1]);
    settle(5);assert(resets==1 && enable_commands==1 && warnings==1 && controller.devices==&working);
}
static void failed_hub_disable(void)
{
    setup();fail_disable=true;xhci_service(&controller);
    struct xhci_device *device=controller.devices;
    assert(controller.stop.failed && device && device!=&working && device->usb.disconnected);
    assert(working.usb.disconnected && disable_commands==1 && dcbaa[1] && allocations);
    assert(strstr(last_warning,"stage=device reason=hub action=controller-failed"));
    xhci_poll(&controller);
    assert(controller.devices==device && allocations && dcbaa[1]); /* DMA stays owned */
    device->next=NULL;controller.stop.stopped=true;xhci_devices_collect(&controller);
    assert(!controller.devices && !allocations); /* only after stopped proof */
}
static void coalesced_failed_disable(void)
{
    setup();replacement=true;xhci_service(&controller);
    struct xhci_device *old=controller.devices;
    assert(old && old!=&working && !old->usb.disconnected && dcbaa[1]);
    assert(resets==1 && enable_commands==1 && !controller.stop.failed);
    unsigned retained=allocations;uint64_t old_dcbaa=dcbaa[1];
    working.endpoints[3].hid=true;
    ports[0]|=CSC;controller.port_work[0]|=1; /* coalesced replug while CCS remains 1 */
    fail_disable=true;xhci_service(&controller);
    assert(controller.stop.failed && old->usb.disconnected && working.usb.disconnected);
    assert(hid_releases==2); /* revoke both old and unrelated HID state */
    assert(resets==1 && enable_commands==1 && disable_commands==1 && (ports[0]&CSC));
    assert(controller.devices==old && allocations==retained && dcbaa[1]==old_dcbaa);
    xhci_poll(&controller);
    assert(controller.devices==old && allocations==retained && dcbaa[1]==old_dcbaa);
    old->next=NULL;controller.stop.stopped=true;xhci_devices_collect(&controller);
    assert(!controller.devices && !allocations); /* release only after stopped proof */
}
static void reconnect(void)
{
    setup();xhci_service(&controller);settle(5);
    assert(resets==1 && enable_commands==1 && warnings==1);
    replacement=true;ports[0]|=CSC; /* CCS remains set: coalesced unplug/replug */
    settle(5);
    assert(resets==2 && enable_commands==2 && warnings==1);
    check_keyboard();
}
static void disconnect_reconnect(void)
{
    setup();xhci_service(&controller);settle(5);
    ports[0]=(ports[0]&~CCS)|CSC;settle(5);
    assert(resets==1 && enable_commands==1 && warnings==1);
    ports[0]|=CCS|CSC;replacement=true;settle(5);
    assert(resets==2 && enable_commands==2 && warnings==1);check_keyboard();
}
static void snapshot_change(void)
{
    setup();ports[0]&=~CSC;inject_service_snapshot=true;xhci_service(&controller);
    assert((ports[0]&CSC) && !resets && !enable_commands);
    replacement=true;settle(5);check_keyboard();
}
static void successful_attach_change(void)
{
    setup();replacement=true;inject_attach=true;xhci_service(&controller);
    assert((ports[0]&CSC) && enable_commands==1 && disable_commands==1 && !allocations);
    assert(controller.devices==&working && !working.usb.disconnected);
    settle(5);assert(enable_commands==2);check_keyboard();
}
static void changed_during(const char *stage)
{
    setup();
    if(!strcmp(stage,"debounce"))inject_debounce=true;
    else if(!strcmp(stage,"reset"))inject_reset=true;
    else if(!strcmp(stage,"ack-window"))inject_ack_window=true;
    else inject_attach=true;
    xhci_service(&controller);
    assert(ports[0]&CSC); /* reset cleanup must preserve the new connection change */
    if(strcmp(stage,"attach"))assert(!enable_commands);
    replacement=true;settle(5);check_keyboard();
}
int main(int argc,char **argv)
{
    assert(argc==2);
    if(!strcmp(argv[1],"storm"))storm();
    else if(!strcmp(argv[1],"speed"))speed();
    else if(!strcmp(argv[1],"transient"))transient();
    else if(!strcmp(argv[1],"retry-exhaustion"))retry_exhaustion();
    else if(!strcmp(argv[1],"warm-speed"))warm_speed();
    else if(!strcmp(argv[1],"warm-reconnect"))warm_reconnect();
    else if(!strcmp(argv[1],"reset-transient"))reset_transient();
    else if(!strcmp(argv[1],"undefined-speed"))undefined_speed();
    else if(!strcmp(argv[1],"hub-interface") || !strcmp(argv[1],"interface") || !strcmp(argv[1],"config"))
        rejected_class(argv[1]);
    else if(!strcmp(argv[1],"failed-hub-disable"))failed_hub_disable();
    else if(!strcmp(argv[1],"coalesced-failed-disable"))coalesced_failed_disable();
    else if(!strcmp(argv[1],"reconnect"))reconnect();
    else if(!strcmp(argv[1],"disconnect-reconnect"))disconnect_reconnect();
    else if(!strcmp(argv[1],"snapshot-change"))snapshot_change();
    else if(!strcmp(argv[1],"successful-attach-change"))successful_attach_change();
    else if(!strcmp(argv[1],"debounce") || !strcmp(argv[1],"reset") ||
            !strcmp(argv[1],"attach") || !strcmp(argv[1],"ack-window"))
        changed_during(argv[1]);
    else assert(0);
    puts("USB xHCI connection-scoped port lifecycle passed");return 0;
}
