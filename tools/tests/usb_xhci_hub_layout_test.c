/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/usb/xhci/internal.h"

/** @brief Catch wrong nibble order, root inheritance, and loss of a TT through a FS hub. */
static void routes(unsigned stride)
{
    uint32_t input[528]={0};
    struct xhci_hub hub={.ports=24,.characteristics=0x60};
    struct xhci_controller c={.stride=(uint8_t)stride};
    struct xhci_device root={.controller=&c,.slot_id=7,.hub=&hub,
        .usb={.speed=USB_SPEED_HIGH,.root_port=2}};
    struct xhci_device fs={.controller=&c,.hub=&hub,.usb={.speed=USB_SPEED_FULL}};
    assert(!xhci_topology_child(&fs,&root,20));
    assert(fs.route==15 && fs.depth==1 && fs.usb.root_port==2);
    assert(fs.tt_slot==7 && fs.tt_port==20);
    struct xhci_device low={.controller=&c,.input={.cpu=input},.usb={.speed=USB_SPEED_LOW}};
    assert(!xhci_topology_child(&low,&fs,3));
    assert(low.route==0x3f && low.depth==2 && low.tt_slot==7 && low.tt_port==20);
    input[stride/4u]=1u<<27;
    xhci_context_topology(&low);
    assert(input[stride/4u]==((1u<<27)|0x3f));
    assert(input[stride/4u+2u]==0x1407);
    root.input.cpu=input;input[stride/4u]=1u<<27;
    xhci_context_topology(&root);
    assert(input[stride/4u]==((1u<<27)|(1u<<26)));
    assert(input[stride/4u+1u]>>24==24);
    assert(input[stride/4u+2u]==0x30000); /* TTT, only on a HS Hub. */
    assert(xhci_topology_child(&low,&root,0)==-EINVAL);
    assert(xhci_topology_child(&low,&root,25)==-EINVAL);
    root.depth=5;assert(xhci_topology_child(&low,&root,1)==-EOPNOTSUPP);
    root.depth=0;root.usb.disconnected=true;
    assert(xhci_topology_child(&low,&root,1)==-ENODEV);
}

/** @brief Reject short bitmaps and malformed SS descriptors before programming hardware. */
static void descriptors(void)
{
    struct xhci_hub hub={0};
    struct usb_endpoint endpoint={.address=0x81,.max_packet=2,.interval=12};
    uint8_t usb2[]={11,0x29,8,0x69,0,10,0,0,0,0xff,0xff};
    assert(!xhci_hub_descriptor(&hub,usb2,sizeof(usb2),USB_SPEED_HIGH,&endpoint));
    assert(hub.ports==8 && hub.bitmap_bytes==2 && hub.power_good==10 && !hub.superspeed);
    endpoint.max_packet=1;
    assert(xhci_hub_descriptor(&hub,usb2,sizeof(usb2),USB_SPEED_HIGH,&endpoint)==-EPROTO);
    endpoint.max_packet=2;usb2[0]=9;
    assert(xhci_hub_descriptor(&hub,usb2,sizeof(usb2),USB_SPEED_HIGH,&endpoint)==-EPROTO);
    uint8_t ss[]={12,0x2a,4,9,0,5,0,0,0,0,0,0};
    endpoint.bytes_per_interval=2;
    assert(!xhci_hub_descriptor(&hub,ss,sizeof(ss),USB_SPEED_SUPER,&endpoint));
    assert(hub.superspeed && hub.ports==4 && hub.bitmap_bytes==1);
    ss[2]=16;
    assert(xhci_hub_descriptor(&hub,ss,sizeof(ss),USB_SPEED_SUPER,&endpoint)==-EPROTO);
    ss[2]=4;
    assert(xhci_hub_descriptor(&hub,ss,sizeof(ss),USB_SPEED_LOW,&endpoint)==-EPROTO);
    uint8_t bos[]={5,15,25,0,1,20,16,10,0,1,0,0,0,0,0,0,0,
                   0x34,0,5,0,0xb4,0,5,0};
    assert(!xhci_hub_bos(&hub,bos,sizeof(bos)) && hub.extended_status);
    assert(hub.rx_rates[4]==5000000000ull && hub.tx_rates[4]==5000000000ull);
    bos[21]=0xb5; /* paired speed IDs must agree */
    assert(xhci_hub_bos(&hub,bos,sizeof(bos))==-EPROTO);
    uint8_t many[71]={71,0x29,255,2,0,0,0};
    endpoint.max_packet=32;
    assert(!xhci_hub_descriptor(&hub,many,sizeof(many),USB_SPEED_HIGH,&endpoint));
    assert(hub.ports==255 && hub.bitmap_bytes==32);
}

static unsigned tt_requests;
static int tt_result;
/** @brief Check the wire request targets the actual HS single TT, with literal USB selectors. */
static int tt_control(struct usb_device *usb,const uint8_t setup[8],void *data,
                        uint16_t length,uint32_t *actual)
{
    assert(usb->speed==USB_SPEED_HIGH && !data && !length && setup[0]==0x23 && setup[1]==8);
    assert(setup[4]==1 && !setup[5] && !setup[6] && !setup[7]);
    unsigned value=setup[2]|(unsigned)setup[3]<<8;
    if(!tt_requests)assert(value==0x9051); /* IN bulk endpoint 1, address 5. */
    else assert(value==(tt_requests==1 ? 0x50u : 0x8050u));
    ++tt_requests;*actual=0;return tt_result;
}

/** @brief Catch clearing an immediate FS Hub instead of its upstream translator. */
static void translator_recovery(void)
{
    static const struct usb_device_operations ops={.control=tt_control};
    struct xhci_hub state={0};
    struct xhci_device hs={.slot_id=7,.hub=&state,.usb={.speed=USB_SPEED_HIGH,.operations=&ops}};
    struct xhci_device fs={.parent=&hs,.usb={.speed=USB_SPEED_FULL}};
    struct xhci_device child={.parent=&fs,.tt_slot=7,.tt_port=4,
        .usb={.speed=USB_SPEED_FULL,.address=5}};
    child.endpoints[3].endpoint.address=0x81;
    assert(!xhci_tt_clear(&child,3) && tt_requests==1);
    assert(!xhci_tt_clear(&child,1) && tt_requests==3);
    child.endpoints[3].hid=true;
    assert(!xhci_tt_clear(&child,3) && tt_requests==3);
    child.endpoints[3].hid=false;hs.usb.disconnected=true;
    assert(xhci_tt_clear(&child,3)==-ENODEV && tt_requests==3);
    hs.usb.disconnected=false;tt_requests=0;tt_result=-EIO;
    assert(xhci_tt_clear(&child,3)==-EIO && tt_requests==1);
}

int main(void)
{
    routes(32);routes(64);descriptors();translator_recovery();
    puts("USB xHCI Hub routes, TT contexts, descriptor and bitmap bounds passed");
}
