/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* A single segment cannot cross a 64 KiB physical boundary.  Its last TRB
 * is a Link, permanently reserved for the next trip around the ring. */
int xhci_ring_init(struct xhci_ring *ring, volatile struct xhci_trb *trbs,
                   uint64_t dma, uint32_t count)
{
    if (!ring || !trbs || !dma || (dma & 63u) || count < 4 || count > 4096 ||
        (dma & 65535u) + (uint64_t)count * 16u > 65536u ||
        dma > UINT64_MAX - (uint64_t)count * 16u) return -EINVAL;
    *ring = (struct xhci_ring){.trbs=trbs,.dma=dma,.count=count,.cycle=1};
    for (uint32_t i=0;i<count;++i) trbs[i]=(struct xhci_trb){0};
    trbs[count-1]=(struct xhci_trb){.lo=(uint32_t)dma,.hi=(uint32_t)(dma>>32),
        .control=XHCI_TRB_TYPE(XHCI_TRB_LINK)|2u};
    return 0;
}

/* Stage the entire TD, including a wrapped Link, before publishing its first
 * cycle bit. Hardware cannot observe a partial chain after the doorbell. */
int xhci_ring_submit(struct xhci_ring *ring, const struct xhci_trb *trbs,
                     unsigned count, struct xhci_pending *pending)
{
    if (!ring || !ring->trbs || !trbs || !pending || !count ||
        count > XHCI_TD_MAX_TRBS || ring->count < 4) return -EINVAL;
    if (ring->quarantined) return -EBUSY;
    if (count > ring->count-1u-ring->used) return -ENOSPC;
    unsigned index=ring->producer, cycle=ring->cycle;
    uint32_t first_control=0; unsigned first=index;
    for (unsigned i=0;i<count;++i) {
        volatile struct xhci_trb *destination=&ring->trbs[index];
        uint32_t control=(trbs[i].control & ~XHCI_TRB_CYCLE)|cycle;
        pending->trbs[i]=ring->dma+(uint64_t)index*16u;
        pending->trb_lengths[i]=trbs[i].status&0x1ffffu;
        destination->lo=trbs[i].lo;
        destination->hi=trbs[i].hi;
        destination->status=trbs[i].status;
        if (!i) first_control=control;
        else destination->control=control;
        if (++index == ring->count-1u) {
            volatile struct xhci_trb *link=&ring->trbs[index];
            link->control=XHCI_TRB_TYPE(XHCI_TRB_LINK)|2u|cycle|
                (trbs[i].control & XHCI_TRB_CHAIN);
            index=0; cycle^=1u;
        }
    }
    pending->count=(uint8_t)count;
    ring->producer=index;ring->cycle=(uint8_t)cycle;ring->used+=count;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ring->trbs[first].control=first_control;
    return 0;
}

/* Reuse after a timeout is prohibited until the caller proves hardware has
 * stopped using the ring; an ordinary completion does not provide that proof. */
int xhci_ring_reclaim(struct xhci_ring *ring, unsigned count, bool stopped)
{
    if (!ring || !ring->trbs || !count || count>ring->used) return -EINVAL;
    if (ring->quarantined && !stopped) return -EBUSY;
    ring->consumer=(ring->consumer+count)%(ring->count-1u);
    ring->used-=count;
    if (stopped && !ring->used) ring->quarantined=false;
    return 0;
}

void xhci_ring_quarantine(struct xhci_ring *ring)
{ if (ring) ring->quarantined=true; }

/* Completion belongs to this TD only when type, slot, endpoint, event-data
 * mode, residual and exact physical TRB identity all agree. */
bool xhci_event_match(const struct xhci_pending *pending, const struct xhci_trb *event)
{
    if (!pending || !event || !pending->count || pending->count>XHCI_TD_MAX_TRBS ||
        ((event->control>>10)&63u)!=pending->event_type || !(event->status>>24)) return false;
    uint8_t slot=(uint8_t)(event->control>>24);
    if (pending->slot && slot!=pending->slot) return false;
    if (pending->event_type==XHCI_EVENT_TRANSFER) {
        if (slot!=pending->slot || (event->control & (1u<<2)) ||
            ((event->control>>16)&31u)!=pending->endpoint) return false;
    } else if (pending->event_type==XHCI_EVENT_COMMAND) {
        if (event->control & (0xffu<<16)) return false; /* VF ID is defined */
    } else return false;
    uint64_t address=event->lo|((uint64_t)event->hi<<32);
    /* Command pointer low bits are RsvdZ; a transfer pointer instead names
     * a naturally aligned TRB and ED=1 was rejected above. */
    if (pending->event_type==XHCI_EVENT_COMMAND) address&=~15ull;
    else if (address & 15u) return false;
    for (unsigned i=0;i<pending->count;++i) if (pending->trbs[i]==address)
        return pending->event_type!=XHCI_EVENT_TRANSFER ||
            (event->status&0xffffffu)<=pending->trb_lengths[i];
    return false;
}
