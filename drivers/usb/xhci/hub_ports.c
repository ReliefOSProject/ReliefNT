/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/console.h>
#include <reliefnt/time.h>

#define HUB_CONNECTED 1u
#define HUB_ENABLED 2u
#define HUB_OVERCURRENT 8u
#define HUB_RESET 16u
#define HUB_C_CONNECTION 1u
#define HUB_C_RESET 16u

struct hub_port_status { uint16_t status, change; uint32_t extended; };

/** @brief Read status with the request length required by USB2/USB3 chapter 11/10. */
static int port_status(struct xhci_device *device,unsigned port,struct hub_port_status *status)
{
    uint8_t bytes[8];uint32_t actual;bool extended=device->hub->extended_status;
    unsigned length=extended ? 8 : 4;
    int rc=xhci_request(&device->usb,0xa3,0,extended ? 2 : 0,(uint16_t)port,bytes,length,&actual);
    if(rc || actual!=length)return rc ? rc : -EPROTO;
    *status=(struct hub_port_status){.status=bytes[0]|(uint16_t)bytes[1]<<8,
        .change=bytes[2]|(uint16_t)bytes[3]<<8};
    if(extended)status->extended=bytes[4]|(uint32_t)bytes[5]<<8|(uint32_t)bytes[6]<<16|(uint32_t)bytes[7]<<24;
    return 0;
}

/** @brief Issue a class port feature request and reject an unexpected data phase. */
static int port_feature(struct xhci_device *device,unsigned port,bool set,unsigned feature)
{
    uint32_t actual;int rc=xhci_request(&device->usb,0x23,set ? 3 : 1,(uint16_t)feature,
                                       (uint16_t)port,NULL,0,&actual);
    return rc ? rc : actual ? -EPROTO : 0;
}

/** @brief Acknowledge supported change bits without sending USB2 selectors to a SS Hub. */
static int acknowledge(struct xhci_device *device,unsigned port,uint16_t changes)
{
    static const uint8_t usb2[]={16,17,18,19,20};
    static const uint8_t usb3[]={16,0,0,19,20,29,25,26};
    unsigned count=device->hub->superspeed ? sizeof(usb3) : sizeof(usb2);
    const uint8_t *selectors=device->hub->superspeed ? usb3 : usb2;
    for(unsigned bit=0;bit<count;++bit)if((changes&(1u<<bit)) && selectors[bit]) {
        int rc=port_feature(device,port,false,selectors[bit]);if(rc)return rc;
    }
    return 0;
}

/** @brief Find the live identity for this particular Hub port, excluding retained old users. */
static struct xhci_device *child_at(struct xhci_device *parent,unsigned port)
{
    for(struct xhci_device *child=parent->controller->devices;child;child=child->next)
        if(child->parent==parent && child->parent_port==port && !child->usb.disconnected)return child;
    return NULL;
}

/** @brief Queue a fresh snapshot, preserving notifications arriving during a control transfer. */
static void queue_port(struct xhci_device *device,unsigned port)
{
    __atomic_fetch_or(&device->hub->pending[port/32u],1u<<(port%32u),__ATOMIC_RELEASE);
    usb_host_request_service(device->controller->host);
}

/** @brief Revoke all functions below a Hub with local power loss or global overcurrent. */
static int hub_status(struct xhci_device *device)
{
    uint8_t bytes[4];uint32_t actual;
    int rc=xhci_request(&device->usb,0xa0,0,0,0,bytes,sizeof(bytes),&actual);
    if(rc || actual!=sizeof(bytes))return rc ? rc : -EPROTO;
    bool blocked=(bytes[0]&3u)!=0,was_blocked=device->hub->blocked;
    device->hub->blocked=blocked;
    if(blocked) {
        for(struct xhci_device *child=device->controller->devices;child;child=child->next)
            if(child->parent==device)xhci_device_disconnect(child);
        xhci_devices_collect(device->controller);
    }
    for(unsigned bit=0;bit<2;++bit)if(bytes[2]&(1u<<bit)) {
        rc=xhci_request(&device->usb,0x20,1,(uint16_t)bit,0,NULL,0,&actual);
        if(rc || actual)return rc ? rc : -EPROTO;
    }
    if(!blocked && was_blocked) {
        for(unsigned port=1;port<=device->hub->ports;++port) {
            rc=port_feature(device,port,true,8);if(rc)return rc;
            device->hub->state[port]=(struct xhci_port_state){0};queue_port(device,port);
        }
        xhci_delay((unsigned)device->hub->power_good*2u+20u);
    }
    return 0;
}

/** @brief Convert reported downstream link speed into the root protocol's raw PSI namespace. */
static int port_speed(struct xhci_device *device,const struct hub_port_status *status,uint8_t *speed_id)
{
    uint8_t speed;uint64_t rate=0;
    if(device->hub->superspeed) {
        speed=USB_SPEED_SUPER;rate=5000000000ull;
        if(status->status&0x1c00u)return -EOPNOTSUPP;
        if(device->hub->extended_status) {
            uint32_t extended=status->extended;
            unsigned rx=extended&15u,tx=(extended>>4)&15u;
            unsigned receive_lanes=((extended>>8)&15u)+1u,transmit_lanes=((extended>>12)&15u)+1u;
            uint64_t receive=device->hub->rx_rates[rx],transmit=device->hub->tx_rates[tx];
            if(!receive || !transmit || receive_lanes>2 || receive_lanes!=transmit_lanes ||
               receive!=transmit)return -EOPNOTSUPP;
            rate=receive*receive_lanes;
            if(rate==10000000000ull || rate==20000000000ull)speed=USB_SPEED_SUPER_PLUS;
            else if(rate!=5000000000ull)return -EOPNOTSUPP;
        }
    } else {
        if((status->status&0x600u)==0x600u)return -EPROTO;
        speed=(status->status&0x400u) ? USB_SPEED_HIGH :
              (status->status&0x200u) ? USB_SPEED_LOW : USB_SPEED_FULL;
        if(device->usb.speed==USB_SPEED_FULL && speed==USB_SPEED_HIGH)return -EPROTO;
    }
    const struct xhci_protocol *protocol=&device->controller->protocol[device->usb.root_port];
    for(unsigned id=1;id<16;++id)
        if((protocol->valid_speeds&(1u<<id)) && protocol->speeds[id]==speed &&
           (!rate || protocol->rates[id]==rate)){*speed_id=(uint8_t)id;return 0;}
    return -EOPNOTSUPP;
}

/** @brief Reset an attached downstream device with bounded waits and reset-change acknowledgement. */
static int reset_port(struct xhci_device *device,unsigned port,struct hub_port_status *status)
{
    bool warm=device->hub->superspeed && (((status->status>>5)&15u)==6u ||
                                          ((status->status>>5)&15u)==10u);
    int rc=port_feature(device,port,true,warm ? 28 : 4);if(rc)return rc;
    for(unsigned attempt=0;attempt<20;++attempt) {
        xhci_delay(10);rc=port_status(device,port,status);if(rc)return rc;
        if((status->change&HUB_C_CONNECTION) || !(status->status&HUB_CONNECTED))return -EAGAIN;
        if(!(status->status&HUB_RESET) && (status->status&HUB_ENABLED)) {
            rc=acknowledge(device,port,status->change&(HUB_C_RESET|(1u<<5)));
            if(!rc)xhci_delay(10);return rc;
        }
    }
    return -ETIMEDOUT;
}

/** @brief Debounce and bind a single connection; any changed snapshot revokes that identity. */
static int service_port(struct xhci_device *device,unsigned port,enum xhci_port_rejection *rejection)
{
    struct hub_port_status status;int rc=port_status(device,port,&status);if(rc)return rc;
    struct xhci_port_state *state=&device->hub->state[port];
    struct xhci_device *child=child_at(device,port);
    if((status.change&HUB_C_CONNECTION) || !(status.status&HUB_CONNECTED))
        *state=(struct xhci_port_state){0};
    if(child && ((status.change&HUB_C_CONNECTION) || !(status.status&HUB_CONNECTED) ||
                 !(status.status&HUB_ENABLED) || (status.status&HUB_OVERCURRENT))) {
        xhci_device_disconnect(child);child=NULL;xhci_devices_collect(device->controller);
    }
    rc=acknowledge(device,port,status.change);if(rc)return rc;
    if(child || device->hub->blocked || !(status.status&HUB_CONNECTED) ||
       (status.status&HUB_OVERCURRENT) || device->controller->stop.failed) {
        if(child)*state=(struct xhci_port_state){0};
        return 0;
    }
    if(state->phase==XHCI_PORT_REJECTED || state->phase==XHCI_PORT_EXHAUSTED)return 0;
    if(state->phase==XHCI_PORT_RETRY && time_ticks()-state->failed_at<
       ((RELIEFNT_TICK_HZ/4u)<<(state->attempts-1u)))return 0;
    uint16_t power=device->hub->superspeed ? 0x200u : 0x100u;
    if(!(status.status&power)) {
        rc=port_feature(device,port,true,8);if(rc)return rc;
        xhci_delay((unsigned)device->hub->power_good*2u+20u);
    }
    xhci_delay(100);rc=port_status(device,port,&status);if(rc)return rc;
    if((status.change&HUB_C_CONNECTION) || !(status.status&HUB_CONNECTED)) {
        queue_port(device,port);return 0;
    }
    rc=reset_port(device,port,&status);
    uint8_t speed=0;
    if(!rc) {
        rc=port_speed(device,&status,&speed);
        if(rc==-EOPNOTSUPP)*rejection=XHCI_REJECT_SPEED;
    }
    if(!rc)rc=xhci_device_attach_child(device,(uint8_t)port,speed,rejection);
    struct hub_port_status after;int snapshot=port_status(device,port,&after);
    if(snapshot || (after.change&HUB_C_CONNECTION) || !(after.status&HUB_CONNECTED)) {
        child=child_at(device,port);
        if(child){xhci_device_disconnect(child);xhci_devices_collect(device->controller);}
        *state=(struct xhci_port_state){0};
        if(!snapshot){queue_port(device,port);return 0;}
        return snapshot;
    }
    if(!rc)*state=(struct xhci_port_state){0};
    return rc;
}

/** @brief Service status bitmaps in task context, with three attempts per stable connection. */
void xhci_hubs_service(struct xhci_controller *controller)
{
    for(struct xhci_device *device=controller->devices;device && !controller->stop.failed;) {
        if(device->hub && !device->usb.disconnected) {
            for(unsigned group=0;group<XHCI_HUB_BITMAP_WORDS && !controller->stop.failed;++group) {
                uint32_t pending=__atomic_exchange_n(&device->hub->pending[group],0,__ATOMIC_ACQ_REL);
                for(unsigned bit=0;bit<32 && !controller->stop.failed;++bit)if(pending&(1u<<bit)) {
                    unsigned port=group*32u+bit;if(port>device->hub->ports)continue;
                    struct xhci_port_state *state=&device->hub->state[port];
                    enum xhci_port_rejection rejection=XHCI_REJECT_NONE;
                    int rc=port ? service_port(device,port,&rejection) : hub_status(device);
                    if(!rc)continue;
                    if(rejection!=XHCI_REJECT_NONE)state->phase=XHCI_PORT_REJECTED;
                    else if(state->attempts>=2){state->attempts=3;state->phase=XHCI_PORT_EXHAUSTED;}
                    else {++state->attempts;state->phase=XHCI_PORT_RETRY;state->failed_at=time_ticks();}
                    console_warn("[usb] xHCI hub=%u port=%u error=%d action=%s\n",device->slot_id,port,rc,
                        state->phase==XHCI_PORT_REJECTED ? "unsupported" :
                        state->phase==XHCI_PORT_EXHAUSTED ? "exhausted" : "backoff");
                }
            }
        }
        /* Attaching inserts children at the list head. Their initial bitmap is
         * picked up in the next service pass, without recursive enumeration. */
        device=device->next;
    }
}
