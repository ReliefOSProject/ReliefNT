/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/* Event Ring ownership is shared by the timer poller and synchronous task
 * transactions. This lock covers the consumer cycle and all pending IDs. */
void xhci_events(struct xhci_controller *controller)
{
    if (!controller || !controller->events.cpu)return;
    uint64_t flags;kernel_spin_lock_irqsave(&controller->event_lock,&flags);
    volatile struct xhci_trb *ring=controller->events.cpu;
    bool consumed=false;
    for (unsigned n=0;n<XHCI_EVENT_COUNT;++n) {
        volatile struct xhci_trb *entry=&ring[controller->event_index];
        uint32_t control=__atomic_load_n(&entry->control,__ATOMIC_ACQUIRE);
        if ((control&1u)!=controller->event_cycle)break;
        struct xhci_trb event={.lo=entry->lo,.hi=entry->hi,.status=entry->status,.control=control};
        consumed=true;
        if (++controller->event_index==XHCI_EVENT_COUNT) {
            controller->event_index=0;controller->event_cycle^=1u;
        }
        unsigned type=(control>>10)&63u;
        if (type==XHCI_EVENT_COMMAND && controller->command_active &&
            xhci_event_match(&controller->command_pending,&event)) {
            controller->command_event=event;controller->command_done=true;
        } else if (type==XHCI_EVENT_TRANSFER) {
            unsigned slot=control>>24,dci=(control>>16)&31u;
            for (struct xhci_device *device=controller->devices;device;device=device->next) {
                if (device->slot_id!=slot || device->disabled || !dci)continue;
                struct xhci_endpoint_state *endpoint=&device->endpoints[dci];
                if (endpoint->active && xhci_complete_event(&endpoint->completion,&event) &&
                    endpoint->completion.done && endpoint->hid)
                    xhci_hid_event(device,dci);
                break;
            }
        } else if (type==34u) {
            unsigned port=event.lo>>24;
            if (port && port<=controller->ports && controller->protocol[port-1].major)
                __atomic_fetch_or(&controller->port_work[(port-1)/32],1u<<((port-1)%32),__ATOMIC_RELEASE);
        } else if (type==37u) {
            __atomic_store_n(&controller->stop.failed,true,__ATOMIC_RELEASE);
        }
    }
    if (consumed) {
        __atomic_thread_fence(__ATOMIC_RELEASE);
        xhci_write64(controller,controller->runtime+0x38u,
            (controller->events.physical+(uint64_t)controller->event_index*16u)|(1u<<3));
    }
    kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
}

int xhci_command(struct xhci_controller *controller,struct xhci_trb trb,uint8_t *slot)
{
    if (__atomic_load_n(&controller->stop.failed,__ATOMIC_ACQUIRE))return -EIO;
    struct xhci_timer timer;int rc=xhci_timer_start(&timer);if(rc)return rc;
    uint64_t flags;kernel_spin_lock_irqsave(&controller->event_lock,&flags);
    if (controller->command_active) {
        kernel_spin_unlock_irqrestore(&controller->event_lock,flags);return -EBUSY;
    }
    controller->command_pending=(struct xhci_pending){.event_type=XHCI_EVENT_COMMAND,
        .slot=(uint8_t)(trb.control>>24)};
    rc=xhci_ring_submit(&controller->command_ring,&trb,1,&controller->command_pending);
    if (!rc) {
        controller->command_done=false;controller->command_active=true;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        xhci_write(controller,controller->doorbells,0);
    }
    kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
    if(rc)return rc;
    for (unsigned spin=0;;++spin) {
        xhci_events(controller);
        kernel_spin_lock_irqsave(&controller->event_lock,&flags);
        bool done=controller->command_done;
        if(done) {
            struct xhci_trb event=controller->command_event;
            controller->command_active=false;controller->command_done=false;
            (void)xhci_ring_reclaim(&controller->command_ring,1,false);
            kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
            if(slot)*slot=(uint8_t)(event.control>>24);
            return (event.status>>24)==1u ? 0 : -EIO;
        }
        kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
        if (xhci_timer_expired(&timer,5000000u) || spin==50000000u) {
            kernel_spin_lock_irqsave(&controller->event_lock,&flags);
            xhci_ring_quarantine(&controller->command_ring);
            __atomic_store_n(&controller->stop.failed,true,__ATOMIC_RELEASE);
            kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
            return -ETIMEDOUT;
        }
        __asm__ volatile("pause" ::: "memory");
    }
}
