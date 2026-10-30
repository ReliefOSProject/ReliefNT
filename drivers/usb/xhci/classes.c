/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

int xhci_ep0_packet(uint8_t speed,uint8_t value,uint16_t *packet)
{
    if(!packet || speed>USB_SPEED_SUPER_PLUS)return -EINVAL;
    if(speed>=USB_SPEED_SUPER) {
        if(value!=9)return -EPROTO;
        *packet=512;return 0;
    }
    if((speed==USB_SPEED_LOW && value!=8) || (speed==USB_SPEED_HIGH && value!=64) ||
       (value!=8 && value!=16 && value!=32 && value!=64))return -EPROTO;
    *packet=value;return 0;
}

/* 0 means unsupported interface, 1 BOT, 2 keyboard, 3 relative mouse.
 * Hubs are explicitly rejected because this HCD implements no route strings. */
int xhci_classify_interface(const struct usb_config_interface *interface)
{
    if(!interface)return -EINVAL;
    if(interface->class_code==9)return -EOPNOTSUPP;
    if(interface->unsupported_endpoint)return 0;
    if(interface->class_code==8 && interface->subclass==6 && interface->protocol==0x50 &&
       interface->endpoint_count==2 && interface->in.address && interface->out.address &&
       !interface->interrupt.address)return 1;
    if(interface->class_code==3 && interface->subclass==1 && interface->endpoint_count==1 &&
       interface->interrupt.address && !interface->in.address && !interface->out.address) {
        if(interface->protocol==1 && interface->interrupt.max_packet>=8)return 2;
        if(interface->protocol==2 && interface->interrupt.max_packet>=3)return 3;
    }
    return 0;
}
