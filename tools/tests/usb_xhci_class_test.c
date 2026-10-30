/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../../drivers/usb/xhci/internal.h"


int main(void)
{
    uint16_t packet=0;
    assert(!xhci_ep0_packet(USB_SPEED_SUPER_PLUS,9,&packet) && packet==512);
    assert(xhci_ep0_packet(USB_SPEED_SUPER_PLUS,8,&packet)==-EPROTO);
    assert(xhci_ep0_packet(5,9,&packet)==-EINVAL);
    assert(!xhci_ep0_packet(USB_SPEED_SUPER,9,&packet) && packet==512);
    assert(xhci_ep0_packet(USB_SPEED_SUPER,8,&packet)==-EPROTO);
    assert(!xhci_ep0_packet(USB_SPEED_HIGH,64,&packet) && packet==64);
    assert(xhci_ep0_packet(USB_SPEED_HIGH,32,&packet)==-EPROTO);
    assert(!xhci_ep0_packet(USB_SPEED_FULL,32,&packet) && packet==32);
    assert(xhci_ep0_packet(USB_SPEED_LOW,16,&packet)==-EPROTO);
    struct usb_config_interface i={.class_code=8,.subclass=6,.protocol=0x50,.endpoint_count=2,
        .in={.address=0x81,.max_packet=1024},.out={.address=2,.max_packet=1024}};
    assert(xhci_classify_interface(&i)==1);
    i.protocol=0x62;assert(!xhci_classify_interface(&i)); /* UAS is unsupported */
    i.protocol=0x50;i.endpoint_count=3;assert(!xhci_classify_interface(&i));
    i=(struct usb_config_interface){.class_code=3,.subclass=1,.protocol=1,.endpoint_count=1,
        .interrupt={.address=0x81,.max_packet=8}};
    assert(xhci_classify_interface(&i)==2);
    i.protocol=2;i.interrupt.max_packet=3;assert(xhci_classify_interface(&i)==3);
    i.unsupported_endpoint=true;assert(!xhci_classify_interface(&i));
    i.class_code=9;assert(xhci_classify_interface(&i)==-EOPNOTSUPP);
    puts("USB xHCI device class and EP0 validation tests passed");
}
