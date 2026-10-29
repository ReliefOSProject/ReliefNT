/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* USB 2.0 sections 5.7.4/5.11.3/11.18: reserve <=80% of a
 * high-speed microframe; use worst-case bit stuffing, a 10us controller
 * allowance per transaction, and the maximum 32 full-speed bit times of hub
 * TT think time. Full-speed transactions sharing a TT frame fit in 125us;
 * low-speed traffic occupies its TT frame alone, bounded to 250us. This avoids
 * FSTNs and gives every split the same SSPLIT/CSPLIT pattern. Treat every
 * high-speed hub as single-TT, including multi-TT hubs in default mode. */
#define HS_PERIODIC_NS 100000u
#define TT_WINDOW_NS 125000u
#define TT_LOW_WINDOW_NS 250000u
#define HOST_DELAY_NS 10000u

/** @brief Ceiling of the worst stuffed payload bit count from USB 2.0 5.11.3. */
static uint32_t stuffed_bits(uint32_t bytes)
{
    return (28u*bytes+2u)/3u;
}

/** @brief Bound one high-speed non-isochronous transaction including host delay. */
static uint32_t hs_ns(uint32_t bytes)
{
    uint32_t bits=55u*8u+4u+stuffed_bits(bytes);
    return (bits*2084u+999u)/1000u+HOST_DELAY_NS;
}

/** @brief Bound all protocol bytes at downstream speed plus TT think time. */
static uint32_t tt_ns(uint8_t speed, uint32_t bytes)
{
    /* USB 2.0 tables 5-6/5-7: at most 19/13 protocol bytes. Charge every
     * byte, including PRE/setup overhead, at the slower downstream rate. */
    uint32_t overhead=speed==USB_SPEED_LOW ? 19u : 13u;
    uint32_t bits=stuffed_bits(overhead+bytes);
    uint32_t wire=speed==USB_SPEED_LOW ? (bits*2000u+2u)/3u : (bits*250u+2u)/3u;
    return wire+HOST_DELAY_NS+2667u;
}

/** @brief Test every due frame's microframe and shared-translator resources. */
static bool fits(const struct ehci_controller *controller, const struct usb_device *device,
                 const struct ehci_bandwidth *proposed)
{
    for(unsigned frame=proposed->phase;frame<EHCI_FRAME_COUNT;frame+=proposed->period) {
        uint32_t usage[8]={0}, tt=proposed->tt_ns;
        unsigned tt_starts=proposed->tt_ns ? 1u : 0u;
        for(unsigned i=0;i<EHCI_MAX_HID;++i) {
            const struct ehci_hid *hid=&controller->hid[i];
            if(!hid->device || __atomic_load_n(&hid->device->usb.disconnected,__ATOMIC_ACQUIRE) ||
               (frame & (hid->period-1u))!=hid->phase)continue;
            if(proposed->tt_ns && hid->tt_ns &&
               device->hub_address==hid->device->usb.hub_address) {
                if(device->speed==USB_SPEED_LOW || hid->device->usb.speed==USB_SPEED_LOW)return false;
                tt+=hid->tt_ns;++tt_starts;
            }
            for(unsigned micro=0;micro<8;++micro) {
                if(hid->smask&(1u<<micro))usage[micro]+=hid->start_ns;
                if(hid->cmask&(1u<<micro))usage[micro]+=hid->complete_ns;
            }
        }
        uint32_t limit=device->speed==USB_SPEED_LOW ? TT_LOW_WINDOW_NS : TT_WINDOW_NS;
        if(tt>limit || tt_starts>16u)return false;
        for(unsigned micro=0;micro<8;++micro) {
            if(proposed->smask&(1u<<micro))usage[micro]+=proposed->start_ns;
            if(proposed->cmask&(1u<<micro))usage[micro]+=proposed->complete_ns;
            if(usage[micro]>HS_PERIODIC_NS)return false;
        }
    }
    return true;
}

/** @brief Choose a frame/microframe phase without publishing an overbooked QH. */
int ehci_bandwidth_admit(const struct ehci_controller *controller,
                         const struct usb_device *device, const struct usb_endpoint *endpoint,
                         struct ehci_bandwidth *result)
{
    if(!controller || !device || !endpoint || !result || device->speed>USB_SPEED_HIGH ||
       !(endpoint->address&0x80u) || !(endpoint->address&15u) ||
       (endpoint->address&0x70u) || !endpoint->interval || !endpoint->max_packet)
        return -EINVAL;
    struct ehci_bandwidth proposed={.period=1};
    unsigned microperiod=8;
    if(device->speed==USB_SPEED_HIGH) {
        if(endpoint->interval>16 || endpoint->max_packet>1024)return -EOPNOTSUPP;
        microperiod=1u<<(endpoint->interval-1u);
        if(microperiod>=8) {
            unsigned frames=microperiod/8;
            proposed.period=frames>EHCI_FRAME_COUNT ? EHCI_FRAME_COUNT : frames;
            microperiod=8;
        }
        proposed.start_ns=hs_ns(endpoint->max_packet);
    } else {
        if(!device->hub_address || !device->hub_port ||
           endpoint->max_packet>(device->speed==USB_SPEED_LOW ? 8u : 64u))return -EOPNOTSUPP;
        while(proposed.period*2u<=endpoint->interval)proposed.period*=2;
        proposed.tt_ns=tt_ns(device->speed,endpoint->max_packet);
        if(proposed.tt_ns>(device->speed==USB_SPEED_LOW ? TT_LOW_WINDOW_NS : TT_WINDOW_NS))
            return -EOPNOTSUPP;
        /* SSPLIT has no payload; CSPLIT includes the data and split overhead. */
        proposed.start_ns=2u*hs_ns(0);
        proposed.complete_ns=hs_ns(0)+hs_ns(endpoint->max_packet);
        proposed.smask=1; proposed.cmask=0x1cu;
    }
    for(unsigned phase=0;phase<proposed.period;++phase) {
        proposed.phase=(uint16_t)phase;
        unsigned choices=device->speed==USB_SPEED_HIGH ? microperiod : 1;
        for(unsigned u=0;u<choices;++u) {
            if(device->speed==USB_SPEED_HIGH) {
                proposed.smask=0;
                for(unsigned i=u;i<8;i+=microperiod)proposed.smask|=1u<<i;
            }
            if(fits(controller,device,&proposed)) { *result=proposed;return 0; }
        }
    }
    return -ENOSPC;
}
