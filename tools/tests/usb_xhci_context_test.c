/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/usb/xhci/internal.h"

int main(void)
{
    uint32_t words[33*16] __attribute__((aligned(64)));
    for (unsigned stride=32;stride<=64;stride+=32) {
        memset(words,0xa5,sizeof(words));
        assert(!xhci_context_address(words,sizeof(words),stride,4,7,512,0x101000));
        unsigned slot=stride/4, ep=2*slot;
        assert(words[0]==0 && words[1]==3);
        assert(words[slot]==((1u<<27)|(4u<<20)) && words[slot+1]==(7u<<16));
        assert(words[ep]==0 && words[ep+1]==((512u<<16)|(4u<<3)|(3u<<1)));
        assert(words[ep+2]==0x101001 && words[ep+3]==0 && words[ep+4]==8);
        for (unsigned i=0;i<33*stride/4;++i)
            if (i!=1 && i!=slot && i!=slot+1 && i!=ep+1 && i!=ep+2 && i!=ep+4)
                assert(words[i]==0);
    }
    assert(xhci_context_address(words,sizeof(words),48,4,7,512,0x101000)==-EINVAL);
    assert(xhci_context_address(words,sizeof(words),64,4,0,512,0x101000)==-EINVAL);
    assert(xhci_context_address(words,sizeof(words),64,4,7,512,0x101001)==-EINVAL);
    assert(xhci_context_address(words,1024,64,4,7,512,0x101000)==-EINVAL);
    assert(!xhci_context_address(words,sizeof(words),64,4,7,512,0x101000));
    struct usb_endpoint ep={.address=0x81,.max_packet=1024,.max_burst=3};
    assert(!xhci_context_endpoint(words,sizeof(words),64,USB_SPEED_SUPER,&ep,false,0x102000));
    assert(words[1]==0xbu && words[16]==((3u<<27)|(4u<<20)));
    assert(words[64+1]==((1024u<<16)|(3u<<8)|(6u<<3)|(3u<<1)));
    assert(words[64+2]==0x102001 && words[64+4]==1024);
    assert(xhci_context_endpoint(words,sizeof(words),64,USB_SPEED_SUPER,&ep,false,0x102000)==-EINVAL);
    ep=(struct usb_endpoint){.address=0x82,.max_packet=8,.interval=10};
    assert(!xhci_context_endpoint(words,sizeof(words),64,USB_SPEED_FULL,&ep,true,0x103000));
    assert(words[96]==(6u<<16));
    assert(words[97]==((8u<<16)|(7u<<3)|(3u<<1)) && words[100]==((8u<<16)|8u));

    /* SSP uses the raw xHCI slot speed ID and SuperSpeed burst/interval rules. */
    assert(!xhci_context_address(words,sizeof(words),64,7,7,512,0x101000));
    assert(words[16]==((1u<<27)|(7u<<20)));
    ep=(struct usb_endpoint){.address=0x81,.max_packet=64,.max_burst=3,
                             .interval=4,.bytes_per_interval=256};
    assert(!xhci_context_endpoint(words,sizeof(words),64,USB_SPEED_SUPER_PLUS,&ep,true,0x102000));
    assert(words[64]==(3u<<16) && words[65]==((64u<<16)|(3u<<8)|(7u<<3)|(3u<<1)));
    assert(words[68]==((256u<<16)|64u));
    ep.bytes_per_interval=257;
    assert(xhci_context_endpoint(words,sizeof(words),64,USB_SPEED_SUPER_PLUS,&ep,true,0x102000)==-EINVAL);
    assert(xhci_context_endpoint(words,sizeof(words),64,5,&ep,true,0x102000)==-EINVAL);

    uint64_t dcbaa[8]={0}, scratch[512]={0};
    assert(!xhci_scratchpad_layout(dcbaa,scratch,3,0x200000,0x300000,false));
    assert(dcbaa[0]==0x200000 && scratch[0]==0x300000 &&
           scratch[1]==0x301000 && scratch[2]==0x302000);
    dcbaa[0]=0xabc0;
    assert(xhci_scratchpad_layout(dcbaa,scratch,9,0x200fc0,0x300000,false)==-EINVAL);
    assert(dcbaa[0]==0xabc0);
    assert(xhci_scratchpad_layout(dcbaa,scratch,3,0x200000,0xfffff000,false)==-EINVAL);
    assert(dcbaa[0]==0xabc0);
    assert(!xhci_scratchpad_layout(dcbaa,scratch,2,0x100200000ull,0x100300000ull,true));
    assert(dcbaa[0]==0x100200000ull && scratch[1]==0x100301000ull);
    assert(!xhci_scratchpad_layout(dcbaa,NULL,0,0,0,false) && !dcbaa[0]);
    puts("USB xHCI context and scratchpad tests passed");
}
