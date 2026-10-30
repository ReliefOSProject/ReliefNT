/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../../drivers/usb/xhci/internal.h"

int main(void)
{
    const uint8_t in_setup[8]={0x80,6,0,1,0,0,18,0};
    struct xhci_trb trbs[3];unsigned count;
    assert(!xhci_build_control(in_setup,0x200000,18,trbs,&count) && count==3);
    assert(trbs[0].lo==0x01000680 && trbs[0].hi==0x00120000 && trbs[0].status==8);
    assert(trbs[0].control==((2u<<10)|(1u<<6)|(3u<<16)));
    assert(trbs[1].lo==0x200000 && trbs[1].status==18);
    assert(trbs[1].control==((3u<<10)|(1u<<16)|(1u<<2)));
    assert(trbs[2].control==((4u<<10)|(1u<<5)));
    const uint8_t no_data[8]={0,9,1,0,0,0,0,0};
    assert(!xhci_build_control(no_data,0,0,trbs,&count) && count==2);
    assert(trbs[1].control==((4u<<10)|(1u<<16)|(1u<<5)));
    assert(xhci_build_control(in_setup,0x20fff0,18,trbs,&count)==-EINVAL);
    assert(xhci_build_control(in_setup,0x200000,17,trbs,&count)==-EINVAL);
    assert(xhci_boundary_chunk(0x20fff0,65536)==16);
    assert(xhci_boundary_chunk(0x210000,65536)==65536);
    assert(!xhci_build_normal(0x20fff0,16,true,&trbs[0]));
    assert(trbs[0].status==16 && trbs[0].control==((1u<<10)|(1u<<5)|(1u<<2)));
    assert(xhci_build_normal(0x20fff0,17,true,&trbs[0])==-EINVAL);
    assert(!xhci_build_normal(0,0,false,&trbs[0]));
    puts("USB xHCI control and bulk TRB tests passed");
}
