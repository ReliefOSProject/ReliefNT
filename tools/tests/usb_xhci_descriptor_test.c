/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/usb/xhci/internal.h"

/* Mutating endpoint geometry or dropping a SuperSpeed companion must fail.
 * The USB2 speed values remain the queue-head encodings used by EHCI. */
int main(void)
{
    assert(USB_SPEED_FULL == 0 && USB_SPEED_LOW == 1 && USB_SPEED_HIGH == 2 &&
           USB_SPEED_SUPER == 3 && USB_SPEED_SUPER_PLUS == 4);
    assert(sizeof(((struct usb_device *)0)->ep0_max_packet) >= 2);
    const uint8_t storage[] = {
        9,2,44,0,1,1,0,0x80,50,
        9,4,0,0,2,8,6,0x50,0,
        7,5,0x81,2,0,4,0, 6,0x30,3,0,0,0,
        7,5,0x02,2,0,4,0, 6,0x30,0,0,0,0,
    };
    struct usb_config result;
    assert(!usb_parse_configuration(storage,sizeof(storage),3,&result));
    assert(!usb_parse_configuration(storage,sizeof(storage),USB_SPEED_SUPER_PLUS,&result));
    assert(result.count == 1 && result.interfaces[0].in.max_packet == 1024);
    assert(result.interfaces[0].in.address == 0x81 && result.interfaces[0].out.address == 2);
    assert(xhci_classify_interface(&result.interfaces[0])==1);
    uint8_t bad[sizeof(storage)];
    memcpy(bad,storage,sizeof(bad));bad[25]=7; /* missing companion */
    assert(usb_parse_configuration(bad,sizeof(bad),3,&result)<0);
    assert(usb_parse_configuration(bad,sizeof(bad),USB_SPEED_SUPER_PLUS,&result)<0);
    memcpy(bad,storage,sizeof(bad));bad[28]=1; /* streams unsupported */
    assert(usb_parse_configuration(bad,sizeof(bad),3,&result)<0);
    assert(usb_parse_configuration(bad,sizeof(bad),USB_SPEED_SUPER_PLUS,&result)<0);
    memcpy(bad,storage,sizeof(bad));bad[29]=1; /* bulk bytes-per-interval reserved */
    assert(usb_parse_configuration(bad,sizeof(bad),3,&result)<0);
    memcpy(bad,storage,sizeof(bad));bad[27]=16; /* bMaxBurst only four bits */
    assert(usb_parse_configuration(bad,sizeof(bad),3,&result)<0);
    memcpy(bad,storage,sizeof(bad));bad[23]=2; /* SS bulk max packet must be 1024 */
    assert(usb_parse_configuration(bad,sizeof(bad),3,&result)<0);
    const uint8_t hid[]={9,2,31,0,1,1,0,0x80,50,9,4,1,0,1,3,1,1,0,
        7,5,0x81,3,8,0,4,6,0x30,0,0,8,0};
    assert(!usb_parse_configuration(hid,sizeof(hid),3,&result));
    assert(!usb_parse_configuration(hid,sizeof(hid),USB_SPEED_SUPER_PLUS,&result));
    assert(result.interfaces[0].interrupt.max_packet==8);
    assert(xhci_classify_interface(&result.interfaces[0])==2);
    uint8_t bad_hid[sizeof(hid)];memcpy(bad_hid,hid,sizeof(hid));bad_hid[29]=9;
    assert(usb_parse_configuration(bad_hid,sizeof(bad_hid),3,&result)<0);
    assert(usb_parse_configuration(bad_hid,sizeof(bad_hid),USB_SPEED_SUPER_PLUS,&result)<0);
    assert(usb_parse_configuration(hid,sizeof(hid),5,&result)<0);
    puts("USB SuperSpeed descriptor tests passed");
}
