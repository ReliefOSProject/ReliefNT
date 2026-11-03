/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Submit one boot-HID/Hub report under event_lock, without sleeping or allocation. */
int xhci_interrupt_arm(struct xhci_device *device,unsigned dci)
{
    struct xhci_controller *controller=device->controller;
    struct xhci_endpoint_state *state=&device->endpoints[dci];
    if(state->active || (!state->hid && !state->hub_status) || device->usb.disconnected)return -EINVAL;
    struct xhci_trb trb;
    int rc=xhci_build_normal(state->dma.physical+1024u,state->endpoint.max_packet,true,&trb);
    if(rc)return rc;
    state->completion=(struct xhci_completion){.pending={.length=state->endpoint.max_packet,
        .event_type=XHCI_EVENT_TRANSFER,.slot=device->slot_id,.endpoint=(uint8_t)dci},.input=true};
    rc=xhci_ring_submit(&state->ring,&trb,1,&state->completion.pending);
    if(!rc) {
        state->active=true;__atomic_thread_fence(__ATOMIC_SEQ_CST);
        xhci_write(controller,controller->doorbells+(uint32_t)device->slot_id*4u,dci);
    }
    return rc;
}
