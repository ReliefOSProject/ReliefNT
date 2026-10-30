/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* Form an Address Device input context for a directly attached function.
 * Context entries are independently 32 or 64 bytes, selected by CSZ. */
int xhci_context_address(uint32_t *context, uint32_t bytes, unsigned stride,
                         uint8_t speed_id, uint8_t root_port, uint16_t packet,
                         uint64_t endpoint_ring)
{
    if (!context || (stride!=32 && stride!=64) || bytes<33u*stride ||
        !speed_id || speed_id>15 || !root_port || !endpoint_ring ||
        (endpoint_ring&15u) || (packet!=8 && packet!=16 && packet!=32 &&
        packet!=64 && packet!=512)) return -EINVAL;
    __builtin_memset(context,0,33u*stride);
    unsigned index=stride/4u;
    uint32_t *slot=context+index, *ep0=context+2u*index;
    context[1]=3u; /* Add Slot and Endpoint 0, no Drop flags. */
    slot[0]=(1u<<27)|((uint32_t)speed_id<<20);
    slot[1]=(uint32_t)root_port<<16;
    ep0[1]=((uint32_t)packet<<16)|(4u<<3)|(3u<<1);
    ep0[2]=(uint32_t)endpoint_ring|1u;
    ep0[3]=(uint32_t)(endpoint_ring>>32);
    ep0[4]=8u;
    return 0;
}

int xhci_context_endpoint(uint32_t *context, uint32_t bytes, unsigned stride,
                          uint8_t speed, const struct usb_endpoint *endpoint,
                          bool interrupt, uint64_t endpoint_ring)
{
    if (!context || !endpoint || (stride!=32 && stride!=64) || bytes<33u*stride ||
        speed>USB_SPEED_SUPER_PLUS || !endpoint_ring || (endpoint_ring&15u) ||
        !(endpoint->address&15u) || (endpoint->address&0x70u) || !endpoint->max_packet ||
        endpoint->max_packet>1024 || endpoint->max_burst>15 ||
        (speed<USB_SPEED_SUPER && endpoint->max_burst) ||
        (interrupt && (!(endpoint->address&0x80u) || !endpoint->interval)) ||
        (interrupt && speed>=USB_SPEED_HIGH && endpoint->interval>16) ||
        (interrupt && speed>=USB_SPEED_SUPER && (!endpoint->bytes_per_interval ||
            endpoint->bytes_per_interval>(uint32_t)endpoint->max_packet*(endpoint->max_burst+1u))))
        return -EINVAL;
    unsigned dci=(endpoint->address&15u)*2u+((endpoint->address&0x80u)!=0);
    if (context[1]&(1u<<dci)) return -EINVAL;
    unsigned interval=0;
    if (interrupt) {
        if (speed>=USB_SPEED_HIGH) interval=endpoint->interval-1u;
        else {
            unsigned value=endpoint->interval;
            while (value>1u) {value>>=1;++interval;}
            interval+=3u;
        }
    }
    unsigned index=stride/4u;
    uint32_t *slot=context+index, *entry=context+(dci+1u)*index;
    __builtin_memset(entry,0,stride);
    uint32_t type=interrupt ? 7u : (endpoint->address&0x80u ? 6u : 2u);
    uint32_t payload=interrupt ? (speed>=USB_SPEED_SUPER ? endpoint->bytes_per_interval : endpoint->max_packet) : 0;
    entry[0]=interval<<16;
    entry[1]=((uint32_t)endpoint->max_packet<<16)|((uint32_t)endpoint->max_burst<<8)|(type<<3)|(3u<<1);
    entry[2]=(uint32_t)endpoint_ring|1u;entry[3]=(uint32_t)(endpoint_ring>>32);
    entry[4]=(payload<<16)|endpoint->max_packet;
    if ((slot[0]>>27)<dci) slot[0]=(slot[0]&~(31u<<27))|(dci<<27);
    context[1]|=1u<<dci;
    return 0;
}

/* Set DCBAA[0] only after validating and filling all scratchpad pointers.
 * This implementation uses 4 KiB controller pages and a single-page array. */
int xhci_scratchpad_layout(uint64_t *dcbaa, uint64_t *array, uint32_t count,
                           uint64_t array_dma, uint64_t pages_dma, bool ac64)
{
    if (!dcbaa) return -EINVAL;
    if (!count) { dcbaa[0]=0;return 0; }
    if (!array || !array_dma || !pages_dma || count>512 || (array_dma&63u) ||
        (pages_dma&4095u) || (array_dma&4095u)+(uint64_t)count*8u>4096u ||
        array_dma>UINT64_MAX-(uint64_t)count*8u+1u ||
        pages_dma>UINT64_MAX-(uint64_t)count*4096u+1u) return -EINVAL;
    uint64_t mask=ac64 ? UINT64_MAX : UINT32_MAX;
    if (array_dma>mask-(uint64_t)count*8u+1u ||
        pages_dma>mask-(uint64_t)count*4096u+1u) return -EINVAL;
    for (uint32_t i=0;i<count;++i) array[i]=pages_dma+(uint64_t)i*4096u;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    dcbaa[0]=array_dma;
    return 0;
}
