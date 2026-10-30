/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* Bootstrap only: a conforming BAR contains the port register set. This bound
 * admits the mandatory HCCPARAMS1/DBOFF/RTSOFF dwords so a full geometry can
 * be calculated before walking any extended capability. */
int xhci_firmware_minimum(uint8_t cap_length,uint32_t hcs,uint32_t *limit)
{
    unsigned ports=hcs>>24;
    if(!limit || cap_length<0x20 || (cap_length&3u) || !ports)return -EINVAL;
    uint32_t end=(uint32_t)cap_length+0x400u+16u*ports,size=16u;
    while(size<end)size<<=1;
    *limit=size;return 0;
}

static bool intersects(uint32_t offset,uint32_t length,uint64_t start,uint64_t end)
{return (uint64_t)offset<end && (uint64_t)offset+length>start;}

/* Extended capabilities must not alias controller registers that the driver
 * may read/write in a different role, including a record straddling them. */
static bool overlaps_live(uint32_t offset,uint32_t length,const struct xhci_geometry *geometry)
{
    uint64_t op=geometry->operational;
    return intersects(offset,length,0,0x20u) ||
        intersects(offset,length,op,op+0x0cu) ||
        intersects(offset,length,op+0x14u,op+0x20u) ||
        intersects(offset,length,op+0x30u,op+0x3cu) ||
        intersects(offset,length,op+0x400u,geometry->port_end) ||
        intersects(offset,length,geometry->doorbells,geometry->doorbell_end) ||
        intersects(offset,length,geometry->runtime,geometry->runtime_end);
}

/* Do not touch an advertised capability outside the mandatory BAR minimum or
 * overlapping active register sets before firmware handoff. */
int xhci_firmware_extent(xhci_firmware_reader read, void *context,
                         const struct xhci_geometry *geometry, uint32_t hcc,
                         uint32_t *legacy, uint32_t *extent)
{
    if (!read || !geometry || !legacy || !extent) return -EINVAL;
    uint32_t limit=geometry->limit;
    *legacy=0;*extent=0x20;
    uint32_t offset=(hcc>>16)*4u;
    for (unsigned count=0;offset;++count) {
        if (count==1024 || offset<0x20 || offset>UINT32_MAX-8u) return -EINVAL;
        if(offset>limit-4u)return -EOPNOTSUPP;
        if(overlaps_live(offset,4u,geometry))return -EINVAL;
        uint32_t header;int rc=read(context,offset,&header);if(rc)return rc;
        uint32_t length=4;
        if ((header&255u)==1u) {
            if (*legacy) return -EINVAL;
            /* The adjacent legacy control/status register must also exist. */
            if(offset>limit-8u)return -EOPNOTSUPP;
            if(overlaps_live(offset,8u,geometry))return -EINVAL;
            uint32_t control;rc=read(context,offset+4u,&control);if(rc)return rc;
            *legacy=offset;length=8;
        } else if ((header&255u)==2u) {
            if(offset>limit-16u)return -EOPNOTSUPP;
            if(overlaps_live(offset,16u,geometry))return -EINVAL;
            uint32_t range;rc=read(context,offset+8u,&range);if(rc)return rc;
            length=16u+(range>>28)*4u;
        }
        if (offset>UINT32_MAX-length) return -EINVAL;
        if (length>limit-offset) return -EOPNOTSUPP;
        if (overlaps_live(offset,length,geometry))return -EINVAL;
        if (offset+length>*extent) *extent=offset+length;
        uint32_t next=((header>>8)&255u)*4u;
        if (!next) break;
        if (next<length || offset>UINT32_MAX-next) return -EINVAL;
        offset+=next;
    }
    return 0;
}

int xhci_firmware_handoff(xhci_firmware_reader read,xhci_firmware_writer8 write8,
                          xhci_firmware_writer32 write32,void *context,
                          uint32_t legacy,bool *acquired)
{
    if(!read || !write8 || !write32 || !acquired)return -EINVAL;
    *acquired=false;
    if(!legacy){*acquired=true;return 0;}
    struct xhci_timer timer;int rc=xhci_timer_start(&timer);if(rc)return rc;
    uint32_t value;rc=read(context,legacy,&value);if(rc)return rc;
    uint8_t original=(uint8_t)(value>>24);
    /* The BIOS and OS semaphores are separately writable bytes. */
    rc=write8(context,legacy+3u,original|1u);if(rc)return rc;
    for(unsigned spin=0;;++spin) {
        rc=read(context,legacy,&value);
        if(rc){(void)write8(context,legacy+3u,original);return rc;}
        if((value&(1u<<24)) && !(value&(1u<<16)))break;
        if(xhci_timer_expired(&timer,1000000u) || spin==50000000u) {
            rc=write8(context,legacy+3u,original);
            return rc ? rc : -EBUSY;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    *acquired=true;
    return write32(context,legacy+4u,0xe0000000u);
}
