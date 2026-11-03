/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>
#include <reliefnt/console.h>
#include <reliefnt/time.h>

/** @brief Read a complete BOS; SSP port speed IDs belong to the Hub, not the xHC. */
static int hub_bos(struct xhci_device *device)
{
    uint8_t header[5];uint32_t actual;
    int rc=xhci_request(&device->usb,0x80,6,15u<<8,0,header,sizeof(header),&actual);
    if(rc || actual!=sizeof(header) || header[0]!=5 || header[1]!=15)return rc ? rc : -EPROTO;
    unsigned length=header[2]|(unsigned)header[3]<<8;
    if(length<5 || length>USB_MAX_CONFIG)return -EOPNOTSUPP;
    uint8_t *bytes=kernel_malloc(length);if(!bytes)return -ENOMEM;
    rc=xhci_request(&device->usb,0x80,6,15u<<8,0,bytes,(uint16_t)length,&actual);
    if(!rc)rc=actual==length ? xhci_hub_bos(device->hub,bytes,length) : -EPROTO;
    kernel_free(bytes);return rc;
}

/** @brief Discover all slot Hub fields before the first Configure Endpoint command. */
int xhci_hub_prepare(struct xhci_device *device,const struct usb_config_interface *interface,
                     const uint8_t descriptor[18])
{
    bool ss=device->usb.speed>=USB_SPEED_SUPER;
    if(device->hub || device->depth>=5 || descriptor[4]!=9 || descriptor[5] ||
       (ss ? descriptor[6]!=3 || interface->protocol!=3 :
        device->usb.speed==USB_SPEED_HIGH ?
            (descriptor[6]!=1 && descriptor[6]!=2) || interface->protocol!=1 :
            descriptor[6] || interface->protocol))return -EOPNOTSUPP;
    struct xhci_hub *hub=kernel_malloc(sizeof(*hub));if(!hub)return -ENOMEM;
    *hub=(struct xhci_hub){0};device->hub=hub;
    uint8_t bytes[7u+2u*((XHCI_HUB_MAX_PORTS+8u)/8u)];uint32_t actual;
    uint16_t type=(uint16_t)(ss ? 0x2a : 0x29)<<8;
    int rc=xhci_request(&device->usb,0xa0,6,type,0,bytes,7,&actual);
    if(rc || actual!=7 || bytes[0]>sizeof(bytes) || bytes[0]<7)return rc ? rc : -EPROTO;
    uint8_t length=bytes[0];
    rc=xhci_request(&device->usb,0xa0,6,type,0,bytes,length,&actual);
    if(!rc)rc=actual==length ? xhci_hub_descriptor(hub,bytes,length,device->usb.speed,
                                                &interface->interrupt) : -EPROTO;
    if(!rc && ss)rc=hub_bos(device);
    if(rc)return rc;
    hub->dci=(interface->interrupt.address&15u)*2u+1u;
    xhci_context_topology(device);
    return 0;
}

/** @brief Power downstream ports and arm the status endpoint; no child is published early. */
int xhci_hub_start(struct xhci_device *device)
{
    struct xhci_hub *hub=device->hub;uint32_t actual;int rc=0;
    if(hub->superspeed) {
        rc=xhci_request(&device->usb,0x20,12,device->depth,0,NULL,0,&actual);
        if(rc || actual)return rc ? rc : -EPROTO;
    }
    for(unsigned port=1;port<=hub->ports;++port) {
        if(hub->superspeed || (hub->characteristics&3u)<2u) {
            rc=xhci_request(&device->usb,0x23,3,8,(uint16_t)port,NULL,0,&actual);
            if(rc || actual)return rc ? rc : -EPROTO;
        }
    }
    xhci_delay((unsigned)hub->power_good*2u+20u);
    for(unsigned port=0;port<=hub->ports;++port)
        __atomic_fetch_or(&hub->pending[port/32u],1u<<(port%32u),__ATOMIC_RELEASE);
    struct xhci_controller *controller=device->controller;
    uint64_t flags;kernel_spin_lock_irqsave(&controller->event_lock,&flags);
    device->endpoints[hub->dci].hub_status=true;
    rc=xhci_interrupt_arm(device,hub->dci);
    kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
    if(!rc) {
        console_info("[usb] xHCI hub slot=%u ports=%u depth=%u route=%05x speed=%s\n",
                     device->slot_id,hub->ports,device->depth,device->route,hub->superspeed ? "USB3" : "USB2");
        usb_host_request_service(controller->host);
    }
    return rc;
}

/** @brief Translate a completed status bitmap to service work while holding event_lock. */
void xhci_hub_event(struct xhci_device *device,unsigned dci)
{
    struct xhci_endpoint_state *state=&device->endpoints[dci];
    struct xhci_controller *controller=device->controller;
    if(state->completion.recovery) {
        state->needs_recovery=true;xhci_ring_quarantine(&state->ring);
        usb_host_request_service(controller->host);return;
    }
    if(!state->completion.result && !device->usb.disconnected) {
        const uint8_t *report=(const uint8_t *)state->dma.cpu+1024u;
        for(unsigned bit=0;bit<state->completion.actual*8u && bit<=device->hub->ports;++bit)
            if(report[bit/8u]&(1u<<(bit%8u)))
                __atomic_fetch_or(&device->hub->pending[bit/32u],1u<<(bit%32u),__ATOMIC_RELEASE);
        usb_host_request_service(controller->host);
    }
    (void)xhci_ring_reclaim(&state->ring,1,false);state->active=false;
    if(!device->usb.disconnected && xhci_interrupt_arm(device,dci)) {
        state->needs_recovery=true;usb_host_request_service(controller->host);
    }
}

/** @brief Queue only expired bounded retries; USB requests remain in service context. */
void xhci_hubs_poll(struct xhci_controller *controller)
{
    uint64_t now=time_ticks();bool work=false;
    for(struct xhci_device *device=controller->devices;device;device=device->next) {
        if(!device->hub || device->usb.disconnected)continue;
        for(unsigned port=0;port<=device->hub->ports;++port) {
            const struct xhci_port_state *state=&device->hub->state[port];
            if(state->phase==XHCI_PORT_RETRY && state->attempts &&
               now-state->failed_at>=((RELIEFNT_TICK_HZ/4u)<<(state->attempts-1u))) {
                __atomic_fetch_or(&device->hub->pending[port/32u],1u<<(port%32u),__ATOMIC_RELEASE);
                work=true;
            }
        }
    }
    if(work)usb_host_request_service(controller->host);
}
