/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Convert the PSI mantissa and decimal exponent to advertised bit/s. */
static uint64_t psi_rate(uint32_t psi)
{
    static const uint64_t factor[]={1u,1000u,1000000u,1000000000u};
    return (uint64_t)(psi>>16)*factor[(psi>>4)&3u];
}

/** Only link rates for which this driver implements the endpoint semantics
 * become usable. Unknown PSI IDs stay unavailable, not guessed from PSIV. */
static void accept_speed(struct xhci_protocol *protocol, uint8_t id, uint32_t psi)
{
    uint64_t rate=psi_rate(psi);uint8_t normalized=0xff;
    unsigned link=(psi>>14)&3u;
    bool full_duplex=(psi&(1u<<8))!=0;
    if (protocol->major==2 && !link && !full_duplex) {
        if (rate==12000000u) normalized=USB_SPEED_FULL;
        else if (rate==1500000u) normalized=USB_SPEED_LOW;
        else if (rate==480000000u) normalized=USB_SPEED_HIGH;
    } else if (protocol->major==3 && full_duplex) {
        if (!link && rate==5000000000ull) normalized=USB_SPEED_SUPER;
        else if (link==1 && protocol->minor>=0x10 &&
                 (rate==10000000000ull || (protocol->minor==0x20 && rate==20000000000ull)))
            normalized=USB_SPEED_SUPER_PLUS;
    }
    if (normalized!=0xff) {
        protocol->valid_speeds|=(uint16_t)(1u<<id);
        protocol->speeds[id]=normalized;protocol->rates[id]=rate;
    }
}

int xhci_protocol_parse(const volatile uint32_t *registers, uint32_t bytes,
                        uint32_t first, uint8_t max_ports, struct xhci_protocol *ports)
{
    if (!registers || !ports || !max_ports || !first || (first&3u)) return -EINVAL;
    __builtin_memset(ports,0,(uint32_t)max_ports*sizeof(*ports));
    uint32_t offset=first;
    while (offset) {
        if (offset>bytes || bytes-offset<4) return -EINVAL;
        uint32_t head=registers[offset/4u], next=(head>>8)&255u;
        uint32_t advance=next*4u;
        if (advance && (advance>bytes-offset || advance<4u)) return -EINVAL;
        if ((head&255u)==2u) {
            if (bytes-offset<16u) return -EINVAL;
            uint32_t name=registers[offset/4u+1u], range=registers[offset/4u+2u];
            uint32_t count=range>>28, length=16u+count*4u;
            if (length>bytes-offset || (advance && advance<length)) return -EINVAL;
            unsigned start=range&255u, n=(range>>8)&255u;
            if (!start || !n || start>max_ports || n>max_ports-start+1u) return -EINVAL;
            struct xhci_protocol p={.major=(uint8_t)(head>>24),.minor=(uint8_t)(head>>16),
                .slot_type=(uint8_t)(registers[offset/4u+3u]&31u)};
            if (!p.major) return -EINVAL;
            for (unsigned i=start-1u;i<start-1u+n;++i) if (ports[i].major) return -EINVAL;
            if (name==0x20425355u && (p.major==2 || p.major==3)) {
                if ((p.major==2 && p.minor!=0) || (p.major==3 &&
                    p.minor!=0 && p.minor!=0x10 && p.minor!=0x20)) return -EINVAL;
                if (!count) {
                    if (p.major==2) {
                        accept_speed(&p,1,(12u<<16)|(2u<<4)|1u);
                        accept_speed(&p,2,(1500u<<16)|(1u<<4)|2u);
                        accept_speed(&p,3,(480u<<16)|(2u<<4)|3u);
                    } else {
                        accept_speed(&p,4,(5u<<16)|(1u<<8)|(3u<<4)|4u);
                        if (p.minor>=0x10)
                            accept_speed(&p,5,(10u<<16)|(1u<<14)|(1u<<8)|(3u<<4)|5u);
                        if (p.minor==0x20) {
                            accept_speed(&p,6,(10u<<16)|(1u<<14)|(1u<<8)|(3u<<4)|6u);
                            accept_speed(&p,7,(20u<<16)|(1u<<14)|(1u<<8)|(3u<<4)|7u);
                        }
                    }
                } else {
                    uint16_t seen=0;
                    p.psi_count=(uint8_t)count;
                    for (unsigned i=0;i<count;++i) p.psi[i]=registers[offset/4u+4u+i];
                    for (unsigned i=0;i<count;++i) {
                        /* xHCI 1.2c 5.1: mask RsvdP before evaluation while
                         * preserving the original dword in the capability. */
                        uint32_t psi=p.psi[i]&~(31u<<9);
                        unsigned id=psi&15u, type=(psi>>6)&3u;
                        if (!id || !(psi>>16) || type==1 || type==3 ||
                            (seen&(1u<<id))) return -EINVAL;
                        seen|=(uint16_t)(1u<<id);
                        if (type==2) {
                            if (++i>=count) return -EINVAL;
                            uint32_t tx=p.psi[i]&~(31u<<9);
                            if ((tx&15u)!=id || ((tx>>6)&3u)!=3 || !(tx>>16) ||
                                ((tx^psi)&((3u<<14)|(1u<<8)))) return -EINVAL;
                            /* Asymmetric links are recognized but unsupported. */
                        } else accept_speed(&p,(uint8_t)id,psi);
                    }
                }
            }
            for (unsigned i=start-1u;i<start-1u+n;++i) ports[i]=p;
        }
        offset=advance ? offset+advance : 0;
    }
    return 0;
}
