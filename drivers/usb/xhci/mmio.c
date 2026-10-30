/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* xHCI 1.2c 5.1 requires reserved read bits to be masked.  The remaining
 * mandatory geometry, assumed truthful for a conforming controller, proves a
 * PCI power-of-two BAR minimum. It does not use xECP or a mapping granule. */
int xhci_geometry_init(uint32_t cap_length,uint32_t hcs,uint32_t doorbells,
                       uint32_t runtime,uint64_t physical,struct xhci_geometry *geometry)
{
    unsigned ports=hcs>>24,slots=hcs&255u,intrs=(hcs>>8)&0x7ffu;
    doorbells&=~3u;runtime&=~31u;
    if(!geometry || cap_length<0x20 || cap_length>0xff || (cap_length&3u) ||
       !ports || !slots || !intrs || intrs>1024 || !doorbells || !runtime ||
       !physical || (physical&4095u))
        return -EINVAL;
    uint64_t port_end=(uint64_t)cap_length+0x400u+16u*ports;
    uint64_t doorbell_end=(uint64_t)doorbells+4u*(slots+1u);
    uint64_t runtime_end=(uint64_t)runtime+0x20u+32u*intrs;
    uint64_t needed=port_end;
    if(doorbell_end>needed)needed=doorbell_end;
    if(runtime_end>needed)needed=runtime_end;
    if(needed>16u*1024u*1024u)return -EOPNOTSUPP;
    if(doorbells<port_end || runtime<port_end ||
       ((uint64_t)doorbells<runtime_end && (uint64_t)runtime<doorbell_end))return -EINVAL;
    uint32_t limit=0x800u;
    while(limit<needed)limit<<=1;
    if ((physical&(limit-1u)) || physical>UINT64_MAX-limit) return -EOPNOTSUPP;
    *geometry=(struct xhci_geometry){.operational=cap_length,.port_end=(uint32_t)port_end,
        .doorbells=doorbells,.doorbell_end=(uint32_t)doorbell_end,
        .runtime=runtime,.runtime_end=(uint32_t)runtime_end,.limit=limit};
    return 0;
}

/* After HALT and bus-master disable, verify this whole mandatory extent and
 * the discovered capabilities against the actual BAR before mapping it. */
int xhci_mmio_layout(const struct xhci_geometry *geometry,uint32_t cap_extent,uint32_t *bytes)
{
    if(!geometry || !bytes || cap_extent<0x20 || cap_extent>geometry->limit)return -EINVAL;
    uint32_t needed=geometry->port_end;
    if(geometry->doorbell_end>needed)needed=geometry->doorbell_end;
    if(geometry->runtime_end>needed)needed=geometry->runtime_end;
    if(cap_extent>needed)needed=cap_extent;
    *bytes=(needed+4095u)&~4095u;
    return 0;
}

int xhci_mmio_address(volatile uint8_t *mapping,uint32_t offset,uint64_t value,bool ac64)
{
    if(!mapping || (offset&7u) || (!ac64 && value>UINT32_MAX))return -EINVAL;
    if(ac64) {
        volatile uint64_t *reg=(volatile uint64_t *)(mapping+offset);
        *reg=value;(void)*reg;
    } else {
        volatile uint32_t *reg=(volatile uint32_t *)(mapping+offset);
        *reg=(uint32_t)value;(void)*reg;
    }
    return 0;
}
