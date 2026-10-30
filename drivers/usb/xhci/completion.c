/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* A control data stage and status stage are separate TDs: an IN short
 * establishes the byte count, but only the status event retires the request. */
bool xhci_complete_event(struct xhci_completion *completion, const struct xhci_trb *event)
{
    if (!completion || completion->done || !xhci_event_match(&completion->pending,event)) return false;
    struct xhci_pending *pending=&completion->pending;
    uint64_t address=event->lo|((uint64_t)event->hi<<32);
    unsigned index=0;
    while (index<pending->count && pending->trbs[index]!=address) ++index;
    unsigned code=event->status>>24;
    uint32_t residual=event->status&0xffffffu;
    if (code==13) {
        if (!completion->input || (completion->control && (pending->count!=3 || index!=1))) return false;
        uint64_t actual=0;
        if (!completion->control)
            for (unsigned i=0;i<index;++i) actual+=pending->trb_lengths[i];
        actual+=pending->trb_lengths[index]-residual;
        if (actual>pending->length) return false;
        completion->actual=(uint32_t)actual;
        completion->short_seen=true;
        if (!completion->control) completion->done=true;
    } else if (code==1) {
        if (index!=pending->count-1u || residual) return false;
        if (!completion->short_seen) completion->actual=pending->length;
        completion->done=true;
    } else {
        completion->result=code==6 ? -EPIPE : -EIO;
        completion->done=true;completion->recovery=true;
    }
    return true;
}
