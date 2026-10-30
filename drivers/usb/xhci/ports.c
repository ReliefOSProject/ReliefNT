/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/console.h>
#include <reliefnt/time.h>

#define PORT_BASE 0x400u
#define PORT_CONNECT 1u
#define PORT_ENABLED (1u<<1)
#define PORT_RESET (1u<<4)
#define PORT_POWER (1u<<9)
#define PORT_CSC (1u<<17)
#define PORT_WRC (1u<<19)
#define PORT_PRC (1u<<21)
#define PORT_CHANGES (0x7fu<<17)
#define PORT_WARM_RESET (1u<<31)
#define PORT_KEEP (PORT_POWER|(3u<<14)|(7u<<25))
#define PORT_ATTEMPTS 3u
#define PORT_RETRY_TICKS (RELIEFNT_TICK_HZ/4u)

static uint32_t port_offset(struct xhci_controller *controller,unsigned port)
{return controller->operational+PORT_BASE+16u*port;}
static void port_write(struct xhci_controller *controller,unsigned port,uint32_t set,uint32_t ack)
{
    uint32_t offset=port_offset(controller,port);
    uint32_t value=xhci_read(controller,offset)&PORT_KEEP;
    xhci_write(controller,offset,value|set|(ack&PORT_CHANGES));
}
static struct xhci_device *child_at(struct xhci_controller *controller,unsigned port)
{
    for(struct xhci_device *device=controller->devices;device;device=device->next)
        if(device->usb.root_port==port && !device->usb.disconnected)return device;
    return NULL;
}
static int port_reset(struct xhci_controller *controller,unsigned port,uint8_t *speed_id,
                      enum xhci_port_rejection *rejection)
{
    uint32_t offset=port_offset(controller,port),value=xhci_read(controller,offset);
    if(value&PORT_CSC)return -EAGAIN;
    if(!(value&PORT_CONNECT))return -ENODEV;
    if((value&PORT_RESET) || (controller->protocol[port].major==3 && (value&PORT_WARM_RESET)))
        return -EBUSY;
    if(controller->per_port_power && !(value&PORT_POWER)) {
        port_write(controller,port,PORT_POWER,0);xhci_delay(20);
        value=xhci_read(controller,offset);
        if(value&PORT_CSC)return -EAGAIN;
        if(!(value&PORT_CONNECT))return -ENODEV;
    }
    int rc=0;
    if(controller->protocol[port].major==2) {
        port_write(controller,port,PORT_RESET,0);
        rc=xhci_wait(controller,offset,PORT_RESET,0,1000);
    } else {
        value=xhci_read(controller,offset);
        if(value&PORT_CSC)return -EAGAIN;
        if(!(value&PORT_CONNECT))return -ENODEV;
        if(!(value&PORT_ENABLED) || ((value>>5)&15u)!=0) {
            port_write(controller,port,PORT_WARM_RESET,0);
            rc=xhci_wait(controller,offset,PORT_WARM_RESET,0,1000);
        }
    }
    value=xhci_read(controller,offset);
    /* Reset completion is ours to consume, including error exits. Leave
     * connection and other changes for the service's snapshot processing. */
    uint32_t reset_changes=value&PORT_PRC;
    if(controller->protocol[port].major==3)reset_changes|=value&PORT_WRC;
    if(reset_changes)port_write(controller,port,0,reset_changes);
    if(rc)return rc;
    if(value&PORT_CSC)return -EAGAIN;
    if(!(value&PORT_CONNECT))return -ENODEV;
    if(!(value&PORT_ENABLED) || ((value>>5)&15u)!=0)return -EIO;
    *speed_id=(uint8_t)((value>>10)&15u);
    if(!*speed_id)return -EOPNOTSUPP;
    if(!(controller->protocol[port].valid_speeds&(1u<<*speed_id))) {
        *rejection=XHCI_REJECT_SPEED;return -EOPNOTSUPP;
    }
    return 0;
}

/* The host registry serializes poll/service. Reset events do not advance the
 * retry clock; subtraction also handles wrap of the monotonic tick counter. */
static bool retry_due(const struct xhci_port_state *state,uint64_t now)
{
    return state->phase==XHCI_PORT_RETRY && state->attempts &&
        now-state->failed_at >= (PORT_RETRY_TICKS<<(state->attempts-1u));
}
static void requeue_port(struct xhci_controller *controller,unsigned port)
{
    __atomic_fetch_or(&controller->port_work[port/32],1u<<(port%32),__ATOMIC_RELEASE);
    usb_host_request_service(controller->host);
}
static const char *rejection_name(enum xhci_port_rejection rejection)
{
    switch(rejection) {
    case XHCI_REJECT_SPEED:return "speed";
    case XHCI_REJECT_HUB:return "hub";
    case XHCI_REJECT_CONFIG:return "configuration";
    case XHCI_REJECT_INTERFACE:return "interface";
    default:return "retryable";
    }
}

/* Release logical input immediately, then revoke block classes in task context.
 * Failed hardware remains DMA owner until normal controller quiesce succeeds. */
static void failure_disconnect(struct xhci_controller *controller)
{
    for(struct xhci_device *device=controller->devices;device;device=device->next)
        xhci_device_disconnect(device);
}
static void failure_poll(struct xhci_controller *controller)
{
    failure_disconnect(controller);
    bool work=!__atomic_exchange_n(&controller->failure_notified,true,__ATOMIC_ACQ_REL);
    if(__atomic_load_n(&controller->collect_pending,__ATOMIC_ACQUIRE)) {
        if(++controller->collect_ticks>=100u){controller->collect_ticks=0;work=true;}
    } else controller->collect_ticks=0;
    if(work)usb_host_request_service(controller->host);
}

void xhci_poll(void *context)
{
    struct xhci_controller *controller=context;
    if(__atomic_load_n(&controller->stop.failed,__ATOMIC_ACQUIRE)){failure_poll(controller);return;}
    uint32_t status=xhci_read(controller,controller->operational+4u);
    if(status&((1u<<2)|(1u<<12))) {
        __atomic_store_n(&controller->stop.failed,true,__ATOMIC_RELEASE);
        failure_poll(controller);
        return;
    }
    bool work=false;
    uint64_t now=time_ticks();
    for(unsigned port=0;port<controller->ports;++port) {
        if(!controller->protocol[port].valid_speeds)continue;
        uint32_t value=xhci_read(controller,port_offset(controller,port));
        if(value&PORT_CHANGES) {
            __atomic_fetch_or(&controller->port_work[port/32],1u<<(port%32),__ATOMIC_RELEASE);
            if((value&(1u<<17)) || !(value&PORT_CONNECT)) {
                struct xhci_device *device=child_at(controller,port);
                if(device)xhci_device_disconnect(device);
            }
            work=true;
        }
        if(retry_due(&controller->port_state[port],now)) {
            __atomic_fetch_or(&controller->port_work[port/32],1u<<(port%32),__ATOMIC_RELEASE);
            work=true;
        }
    }
    xhci_events(controller);
    if(__atomic_load_n(&controller->stop.failed,__ATOMIC_ACQUIRE)){failure_poll(controller);return;}
    for(unsigned i=0;i<8;++i)if(__atomic_load_n(&controller->port_work[i],__ATOMIC_ACQUIRE))work=true;
    /* A revoked disk can outlive its port event through an open FD/mount.
     * Retry in task context at a bounded polling cadence until users drain. */
    if(__atomic_load_n(&controller->collect_pending,__ATOMIC_ACQUIRE)) {
        if(++controller->collect_ticks>=100u){controller->collect_ticks=0;work=true;}
    } else controller->collect_ticks=0;
    if(work)usb_host_request_service(controller->host);
}

void xhci_service(void *context)
{
    struct xhci_controller *controller=context;uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&controller->busy,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
        usb_host_request_service(controller->host);return;
    }
    if(__atomic_load_n(&controller->stop.failed,__ATOMIC_ACQUIRE)) {
        failure_disconnect(controller);xhci_devices_collect(controller);
        __atomic_store_n(&controller->busy,0,__ATOMIC_RELEASE);return;
    }
    xhci_devices_collect(controller);
    for(unsigned group=0;group<8 && !controller->stop.failed;++group) {
        uint32_t pending=__atomic_exchange_n(&controller->port_work[group],0,__ATOMIC_ACQ_REL);
        for(unsigned bit=0;bit<32 && !controller->stop.failed;++bit)if(pending&(1u<<bit)) {
            unsigned port=group*32u+bit;
            if(port>=controller->ports || !controller->protocol[port].valid_speeds)continue;
            struct xhci_device *old=child_at(controller,port);
            struct xhci_port_state *state=&controller->port_state[port];
            uint32_t value=xhci_read(controller,port_offset(controller,port));
            if((value&PORT_CSC) || !(value&PORT_CONNECT))
                *state=(struct xhci_port_state){0};
            if(old && ((value&PORT_CSC) || !(value&PORT_CONNECT))) {
                xhci_device_disconnect(old);old=NULL;xhci_devices_collect(controller);
                if(__atomic_load_n(&controller->stop.failed,__ATOMIC_ACQUIRE))break;
            }
            port_write(controller,port,0,value);
            if(old || !(value&PORT_CONNECT))continue;
            if(state->phase==XHCI_PORT_REJECTED || state->phase==XHCI_PORT_EXHAUSTED ||
               (state->phase==XHCI_PORT_RETRY && !retry_due(state,time_ticks())))continue;
            xhci_delay(100);
            value=xhci_read(controller,port_offset(controller,port));
            if((value&PORT_CSC) || !(value&PORT_CONNECT)) {requeue_port(controller,port);continue;}
            uint8_t speed_id=0;enum xhci_port_rejection rejection=XHCI_REJECT_NONE;
            const char *stage="reset";
            int rc=port_reset(controller,port,&speed_id,&rejection);
            if(!rc) {
                value=xhci_read(controller,port_offset(controller,port));
                if((value&PORT_CSC) || !(value&PORT_CONNECT))rc=-EAGAIN;
                else {stage="device";rc=xhci_device_attach(controller,(uint8_t)port,speed_id,&rejection);}
            }
            value=xhci_read(controller,port_offset(controller,port));
            bool changed=(value&PORT_CSC) || !(value&PORT_CONNECT);
            if(rc && ((rc!=-EAGAIN && rc!=-ENODEV) || !changed)) {
                const char *action=changed ? "connection-change" : controller->stop.failed ? "controller-failed" :
                    rejection!=XHCI_REJECT_NONE ? "unsupported" :
                    state->attempts+1u>=PORT_ATTEMPTS ? "exhausted" : "backoff";
                console_warn("[usb] xHCI root_port=%u attach error=%d stage=%s reason=%s action=%s\n",
                             port+1u,rc,stage,rejection_name(rejection),action);
            }
            if(changed) {
                old=child_at(controller,port);
                if(old){xhci_device_disconnect(old);xhci_devices_collect(controller);}
                *state=(struct xhci_port_state){0};requeue_port(controller,port);continue;
            }
            if(!rc){*state=(struct xhci_port_state){0};continue;}
            if(controller->stop.failed)continue;
            if(rejection!=XHCI_REJECT_NONE)state->phase=XHCI_PORT_REJECTED;
            else if(++state->attempts>=PORT_ATTEMPTS)state->phase=XHCI_PORT_EXHAUSTED;
            else {state->phase=XHCI_PORT_RETRY;state->failed_at=time_ticks();}
        }
    }
    for(struct xhci_device *device=controller->devices;device && !controller->stop.failed;device=device->next) {
        if(device->usb.disconnected)continue;
        for(unsigned dci=2;dci<XHCI_ENDPOINTS && !controller->stop.failed;++dci) {
            struct xhci_endpoint_state *endpoint=&device->endpoints[dci];
            if(!endpoint->hid || !endpoint->needs_recovery)continue;
            int rc=endpoint->active ? xhci_endpoint_recover(device,dci,true) : 0;
            if(!rc) {
                uint64_t flags;kernel_spin_lock_irqsave(&controller->event_lock,&flags);
                endpoint->needs_recovery=false;rc=xhci_hid_arm(device,dci);
                kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
            }
            if(rc)endpoint->needs_recovery=true;
        }
    }
    if(__atomic_load_n(&controller->stop.failed,__ATOMIC_ACQUIRE))failure_disconnect(controller);
    xhci_devices_collect(controller);
    __atomic_store_n(&controller->busy,0,__ATOMIC_RELEASE);
}
