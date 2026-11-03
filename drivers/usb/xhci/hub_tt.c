/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Clear control/bulk state at the actual HS translator, including through FS Hubs. */
int xhci_tt_clear(struct xhci_device *device,unsigned dci)
{
    if(!device || !dci || dci>=XHCI_ENDPOINTS)return -EINVAL;
    if(!device->tt_slot || (device->usb.speed!=USB_SPEED_LOW && device->usb.speed!=USB_SPEED_FULL))return 0;
    struct xhci_endpoint_state *endpoint=&device->endpoints[dci];
    if(dci!=1 && (endpoint->hid || endpoint->hub_status))return 0;
    struct xhci_device *hub=device->parent;
    while(hub && hub->slot_id!=device->tt_slot)hub=hub->parent;
    if(!hub || !hub->hub || hub->usb.speed!=USB_SPEED_HIGH || hub->usb.disconnected)return -ENODEV;
    uint16_t value=(uint16_t)device->usb.address<<4;
    if(dci!=1)value|=(endpoint->endpoint.address&15u)|(2u<<11)|
                       ((endpoint->endpoint.address&0x80u) ? 0x8000u : 0);
    for(unsigned direction=0;direction<(dci==1 ? 2u : 1u);++direction) {
        uint16_t selector=value|(direction ? 0x8000u : 0);
        /* Hub alternate zero is single-TT: selector index is 1, not the
         * downstream port number used in the xHC's split token context. */
        uint8_t setup[8]={0x23,8,(uint8_t)selector,(uint8_t)(selector>>8),1,0,0,0};
        uint32_t actual=0;
        int rc=hub->usb.operations->control(&hub->usb,setup,NULL,0,&actual);
        if(rc || actual)return rc ? rc : -EPROTO;
    }
    return 0;
}
