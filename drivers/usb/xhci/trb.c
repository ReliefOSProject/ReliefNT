/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* Normal TRBs may not cross a 64 KiB boundary; callers submit successive
 * single-TRB TDs so a short packet cannot produce a late second IOC event. */
uint32_t xhci_boundary_chunk(uint64_t address,uint32_t remaining)
{
    uint32_t chunk=65536u-(uint32_t)(address&65535u);
    if(chunk>remaining)chunk=remaining;
    if(chunk && address>UINT64_MAX-(uint64_t)chunk+1u)return 0;
    return chunk;
}
int xhci_build_normal(uint64_t data_dma,uint32_t length,bool input,struct xhci_trb *trb)
{
    if(!trb || length>65536u || (length && (!data_dma ||
        xhci_boundary_chunk(data_dma,length)!=length)))return -EINVAL;
    *trb=(struct xhci_trb){.lo=(uint32_t)data_dma,.hi=(uint32_t)(data_dma>>32),
        .status=length,.control=XHCI_TRB_TYPE(1)|(1u<<5)|(input ? 1u<<2 : 0)};
    return 0;
}
int xhci_build_control(const uint8_t *setup,uint64_t data_dma,uint16_t length,
                       struct xhci_trb *trbs,unsigned *count)
{
    if(!setup || !trbs || !count || (setup[6]|(uint16_t)setup[7]<<8)!=length ||
        (length && (!data_dma || xhci_boundary_chunk(data_dma,length)!=length)))return -EINVAL;
    bool input=(setup[0]&0x80u)!=0;
    uint32_t lo=(uint32_t)setup[0]|(uint32_t)setup[1]<<8|(uint32_t)setup[2]<<16|(uint32_t)setup[3]<<24;
    uint32_t hi=(uint32_t)setup[4]|(uint32_t)setup[5]<<8|(uint32_t)setup[6]<<16|(uint32_t)setup[7]<<24;
    trbs[0]=(struct xhci_trb){.lo=lo,.hi=hi,.status=8,
        .control=XHCI_TRB_TYPE(2)|(1u<<6)|((length ? (input ? 3u : 2u) : 0u)<<16)};
    unsigned n=1;
    if(length)trbs[n++]=(struct xhci_trb){.lo=(uint32_t)data_dma,.hi=(uint32_t)(data_dma>>32),
        .status=length,.control=XHCI_TRB_TYPE(3)|(input ? ((1u<<16)|(1u<<2)) : 0)};
    trbs[n++]=(struct xhci_trb){.control=XHCI_TRB_TYPE(4)|(1u<<5)|(!length || !input ? 1u<<16 : 0)};
    *count=n;
    return 0;
}
