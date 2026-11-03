/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Form the five-nibble route and find the hub isolating a slower USB segment. */
int xhci_topology_child(struct xhci_device *device,struct xhci_device *parent,uint8_t port)
{
    if(!device || !parent || !parent->hub || !port || port>parent->hub->ports ||
       device->controller!=parent->controller)return -EINVAL;
    if(parent->usb.disconnected)return -ENODEV;
    if(parent->depth>=5)return -EOPNOTSUPP;
    uint8_t speed=device->usb.speed,upstream=parent->usb.speed;
    if(speed>USB_SPEED_SUPER_PLUS || upstream==USB_SPEED_LOW ||
       (speed>=USB_SPEED_SUPER)!=(upstream>=USB_SPEED_SUPER) ||
       (upstream==USB_SPEED_FULL && speed==USB_SPEED_HIGH))return -EOPNOTSUPP;
    device->parent=parent;device->parent_port=port;device->depth=parent->depth+1u;
    device->usb.root_port=parent->usb.root_port;
    device->route=parent->route|((uint32_t)(port<15 ? port : 15u)<<(parent->depth*4u));
    device->tt_slot=device->tt_port=0;
    if(speed==USB_SPEED_LOW || speed==USB_SPEED_FULL) {
        if(upstream==USB_SPEED_HIGH){device->tt_slot=parent->slot_id;device->tt_port=port;}
        else {device->tt_slot=parent->tt_slot;device->tt_port=parent->tt_port;}
    } else if(speed>=USB_SPEED_SUPER) {
        const struct xhci_protocol *protocol=&device->controller->protocol[device->usb.root_port];
        uint64_t rate=protocol->rates[device->speed_id],parent_rate=protocol->rates[parent->speed_id];
        if(!rate || rate>parent_rate)return -EOPNOTSUPP;
        if(rate<parent_rate){device->tt_slot=parent->slot_id;device->tt_port=port;}
        else {device->tt_slot=parent->tt_slot;device->tt_port=parent->tt_port;}
    }
    return 0;
}

/** @brief Preserve speed/endpoint entries while adding Address/Configure slot topology. */
void xhci_context_topology(struct xhci_device *device)
{
    uint32_t *slot=(uint32_t *)device->input.cpu+device->controller->stride/4u;
    slot[0]=(slot[0]&~(0xfffffu|(1u<<25)|(1u<<26)))|device->route;
    slot[1]&=0x00ffffffu;
    slot[2]=(slot[2]&~0x3ffffu)|device->tt_slot|((uint32_t)device->tt_port<<8);
    if(device->hub) {
        slot[0]|=1u<<26;
        slot[1]|=(uint32_t)device->hub->ports<<24;
        if(device->usb.speed==USB_SPEED_HIGH)
            slot[2]|=((uint32_t)(device->hub->characteristics>>5)&3u)<<16;
    }
    /* Alternate zero remains single-TT, including on multi-TT capable hubs. */
}
