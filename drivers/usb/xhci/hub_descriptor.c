/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Validate descriptor length, port bounds and the complete interrupt bitmap. */
int xhci_hub_descriptor(struct xhci_hub *hub,const uint8_t *bytes,unsigned length,
                         uint8_t speed,const struct usb_endpoint *endpoint)
{
    if(!hub || !bytes || !endpoint)return -EINVAL;
    bool ss=speed>=USB_SPEED_SUPER;
    if(speed==USB_SPEED_LOW || speed>USB_SPEED_SUPER_PLUS || length<7 || bytes[0]!=length ||
       bytes[1]!=(ss ? 0x2a : 0x29) || !bytes[2] ||
       bytes[2]>(ss ? 15u : XHCI_HUB_MAX_PORTS))return -EPROTO;
    unsigned bitmap=((unsigned)bytes[2]+8u)/8u;
    if(length!=(ss ? 12u : 7u+2u*bitmap) || endpoint->max_packet<bitmap ||
       (ss && endpoint->bytes_per_interval<bitmap))return -EPROTO;
    uint16_t characteristics=bytes[3]|(uint16_t)bytes[4]<<8;
    if((characteristics&3u)==3u || ((characteristics>>3)&3u)==3u)return -EPROTO;
    hub->ports=bytes[2];hub->characteristics=characteristics;
    hub->power_good=bytes[5];hub->bitmap_bytes=(uint8_t)bitmap;hub->superspeed=ss;
    return 0;
}

/** @brief Read an unaligned little-endian USB descriptor dword. */
static uint32_t descriptor_u32(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}

/** @brief Record only paired symmetric sublinks; asymmetric speeds remain unsupported. */
int xhci_hub_bos(struct xhci_hub *hub,const uint8_t *bytes,unsigned length)
{
    if(!hub || !bytes)return -EINVAL;
    if(length<5 || bytes[0]!=5 || bytes[1]!=15 ||
       (bytes[2]|(unsigned)bytes[3]<<8)!=length)return -EPROTO;
    unsigned count=0;bool found=false;
    for(unsigned offset=5;offset<length;) {
        if(length-offset<3 || bytes[offset]<3 || bytes[offset]>length-offset ||
           bytes[offset+1]!=16)return -EPROTO;
        const uint8_t *cap=bytes+offset;unsigned size=cap[0];++count;
        if(cap[2]==10) {
            if(found || size<12)return -EPROTO;
            found=true;unsigned entries=(descriptor_u32(cap+4)&31u)+1u;
            if((entries&1u) || size!=12u+4u*entries)return -EPROTO;
            uint16_t seen=0;
            for(unsigned i=0;i<entries;i+=2) {
                uint32_t rx=descriptor_u32(cap+12u+4u*i),tx=descriptor_u32(cap+16u+4u*i);
                unsigned id=rx&15u,kind=(rx>>6)&3u,link=(rx>>14)&3u;
                if(kind>1 || ((tx>>6)&3u)!=kind+2u || (tx&15u)!=id ||
                   ((tx>>14)&3u)!=link || link>1 || !(rx>>16) || !(tx>>16) ||
                   (seen&(1u<<id)))return -EPROTO;
                seen|=(uint16_t)(1u<<id);
                static const uint64_t factor[]={1u,1000u,1000000u,1000000000u};
                uint64_t receive=(uint64_t)(rx>>16)*factor[(rx>>4)&3u];
                uint64_t transmit=(uint64_t)(tx>>16)*factor[(tx>>4)&3u];
                if(!kind && receive==transmit) {
                    hub->rx_rates[id]=receive;hub->tx_rates[id]=transmit;
                }
            }
        }
        offset+=size;
    }
    if(count!=bytes[4])return -EPROTO;
    hub->extended_status=found;
    return 0;
}
