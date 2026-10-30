/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

static unsigned endpoint_dci(uint8_t address)
{ return (address&15u)*2u+((address&0x80u)!=0); }

/* Reset/stop the actual endpoint, then move its dequeue past the abandoned
 * TD before its memory can be reused. A failed command retains the ring. */
int xhci_endpoint_recover(struct xhci_device *device,unsigned dci,bool halted)
{
    if (!device || !dci || dci>=XHCI_ENDPOINTS)return -EINVAL;
    struct xhci_controller *controller=device->controller;
    struct xhci_endpoint_state *endpoint=&device->endpoints[dci];
    if (!endpoint->active)return -EINVAL;
    struct xhci_trb command={.control=XHCI_TRB_TYPE(halted ? 14u : 15u)|
        ((uint32_t)dci<<16)|((uint32_t)device->slot_id<<24)};
    int rc=xhci_command(controller,command,NULL);
    if (!rc && halted && dci!=1) {
        uint8_t setup[8]={2,1,0,0,endpoint->endpoint.address,0,0,0};
        uint32_t actual=0;
        rc=device->usb.operations->control(&device->usb,setup,NULL,0,&actual);
        if (!rc && actual)rc=-EPROTO;
    }
    if (!rc) {
        uint64_t dequeue=(endpoint->ring.dma+(uint64_t)endpoint->ring.producer*16u)|
            endpoint->ring.cycle;
        command=(struct xhci_trb){.lo=(uint32_t)dequeue,.hi=(uint32_t)(dequeue>>32),
            .control=XHCI_TRB_TYPE(16)|((uint32_t)dci<<16)|((uint32_t)device->slot_id<<24)};
        rc=xhci_command(controller,command,NULL);
    }
    uint64_t flags;kernel_spin_lock_irqsave(&controller->event_lock,&flags);
    if (!rc) {
        rc=xhci_ring_reclaim(&endpoint->ring,endpoint->completion.pending.count,true);
        if (!rc) {endpoint->active=false;endpoint->needs_recovery=false;}
    }
    if (rc) {
        xhci_ring_quarantine(&endpoint->ring);
        __atomic_store_n(&controller->stop.failed,true,__ATOMIC_RELEASE);
    }
    kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
    return rc;
}

static int submit(struct xhci_device *device,unsigned dci,const struct xhci_trb *trbs,
                  unsigned count,uint32_t length,bool control,bool input,uint32_t *actual)
{
    struct xhci_controller *controller=device->controller;
    struct xhci_endpoint_state *endpoint=&device->endpoints[dci];
    if (__atomic_load_n(&device->usb.disconnected,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&controller->stop.failed,__ATOMIC_ACQUIRE))return -ENODEV;
    struct xhci_timer timer;int rc=xhci_timer_start(&timer);if(rc)return rc;
    uint64_t flags;kernel_spin_lock_irqsave(&controller->event_lock,&flags);
    if (endpoint->active || !endpoint->configured) {
        kernel_spin_unlock_irqrestore(&controller->event_lock,flags);return -EBUSY;
    }
    endpoint->completion=(struct xhci_completion){.pending={.length=length,
        .event_type=XHCI_EVENT_TRANSFER,.slot=device->slot_id,.endpoint=(uint8_t)dci},
        .control=control,.input=input};
    rc=xhci_ring_submit(&endpoint->ring,trbs,count,&endpoint->completion.pending);
    if (!rc) {
        endpoint->active=true;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        xhci_write(controller,controller->doorbells+(uint32_t)device->slot_id*4u,dci);
    }
    kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
    if(rc)return rc;
    for (unsigned spin=0;;++spin) {
        xhci_events(controller);
        kernel_spin_lock_irqsave(&controller->event_lock,&flags);
        bool done=endpoint->completion.done;
        if(done) {
            rc=endpoint->completion.result;*actual=endpoint->completion.actual;
            bool recovery=endpoint->completion.recovery;
            if (!recovery) {
                (void)xhci_ring_reclaim(&endpoint->ring,count,false);endpoint->active=false;
            } else {
                endpoint->needs_recovery=true;xhci_ring_quarantine(&endpoint->ring);
            }
            kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
            if (recovery && dci==1 && !device->usb.disconnected) {
                int recover=xhci_endpoint_recover(device,dci,true);if(recover)return recover;
            }
            return rc;
        }
        bool gone=__atomic_load_n(&device->usb.disconnected,__ATOMIC_ACQUIRE);
        bool failed=__atomic_load_n(&controller->stop.failed,__ATOMIC_ACQUIRE);
        bool expired=xhci_timer_expired(&timer,5000000u) || spin==50000000u;
        if (gone || failed || expired) {
            xhci_ring_quarantine(&endpoint->ring);
            kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
            if (!gone && !failed) {
                int recover=xhci_endpoint_recover(device,dci,false);
                if (recover)return recover;
            }
            return gone ? -ENODEV : failed ? -EIO : -ETIMEDOUT;
        }
        kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
        __asm__ volatile("pause" ::: "memory");
    }
}

static int acquire(struct usb_device *usb)
{
    struct xhci_device *device=usb->host_context;struct xhci_controller *controller=device->controller;
    if (__atomic_load_n(&usb->disconnected,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&controller->stop.failed,__ATOMIC_ACQUIRE))return -ENODEV;
    uint32_t expected=0;
    return __atomic_compare_exchange_n(&controller->busy,&expected,1,false,
        __ATOMIC_ACQUIRE,__ATOMIC_RELAXED) ? 0 : -EAGAIN;
}
static void release(struct usb_device *usb)
{
    struct xhci_device *device=usb->host_context;
    __atomic_store_n(&device->controller->busy,0,__ATOMIC_RELEASE);
}
static int control(struct usb_device *usb,const uint8_t setup[8],void *data,
                   uint16_t length,uint32_t *actual)
{
    if (!setup || !actual || length>USB_MAX_CONFIG || (length && !data))return -EINVAL;
    *actual=0;
    if (setup[0]==0 && setup[1]==5)return -EOPNOTSUPP; /* Address Device owns SET_ADDRESS. */
    struct xhci_device *device=usb->host_context;struct xhci_controller *controller=device->controller;
    if (__atomic_load_n(&usb->disconnected,__ATOMIC_ACQUIRE) || controller->stop.failed)return -ENODEV;
    bool input=(setup[0]&0x80u)!=0;
    if (length && !input)__builtin_memcpy(controller->bounce.cpu,data,length);
    struct xhci_trb trbs[3];unsigned count;
    int rc=xhci_build_control(setup,controller->bounce.physical,length,trbs,&count);if(rc)return rc;
    rc=submit(device,1,trbs,count,length,true,input,actual);
    if (!rc && input && *actual)__builtin_memcpy(data,controller->bounce.cpu,*actual);
    return rc;
}
static int bulk(struct usb_device *usb,struct usb_endpoint *endpoint,void *data,
                uint32_t length,uint32_t *actual)
{
    if (!endpoint || !actual || length>XHCI_BOUNCE_BYTES || (length && !data))return -EINVAL;
    *actual=0;struct xhci_device *device=usb->host_context;struct xhci_controller *controller=device->controller;
    unsigned dci=endpoint_dci(endpoint->address);
    if (dci<2 || dci>=XHCI_ENDPOINTS || !device->endpoints[dci].configured)return -EINVAL;
    if (__atomic_load_n(&usb->disconnected,__ATOMIC_ACQUIRE) || controller->stop.failed)return -ENODEV;
    bool input=(endpoint->address&0x80u)!=0;
    if (!input && length)__builtin_memcpy(controller->bounce.cpu,data,length);
    uint32_t offset=0;
    do {
        uint32_t chunk=xhci_boundary_chunk(controller->bounce.physical+offset,length-offset);
        if (length>offset && !chunk)return -EINVAL;
        struct xhci_trb trb;int rc=xhci_build_normal(controller->bounce.physical+offset,chunk,input,&trb);
        if(rc)return rc;
        uint32_t transferred=0;rc=submit(device,dci,&trb,1,chunk,false,input,&transferred);
        if (!rc && input && transferred)__builtin_memcpy((uint8_t *)data+offset,
            (uint8_t *)controller->bounce.cpu+offset,transferred);
        *actual+=transferred;
        if(rc)return rc;
        offset+=transferred;
        if(transferred!=chunk)return input ? 0 : -EIO;
    } while(offset<length);
    return 0;
}

/* BOT reset clears both endpoints, including one which never halted. Reset
 * Endpoint only accepts a Halted endpoint. Replace the idle endpoint context
 * and ring so its host toggle restarts in step with CLEAR_FEATURE. */
static int reset_idle_endpoint(struct xhci_device *device,unsigned dci)
{
    struct xhci_controller *controller=device->controller;
    struct xhci_endpoint_state *state=&device->endpoints[dci];
    if(state->active || state->ring.used || state->replacement.physical)return -EBUSY;
    int rc=xhci_dma_alloc(controller,&state->replacement,1);if(rc)return rc;
    struct xhci_ring replacement;
    rc=xhci_ring_init(&replacement,state->replacement.cpu,state->replacement.physical,XHCI_RING_COUNT);
    uint32_t *input=device->input.cpu,*output=device->output.cpu;
    if(!rc) {
        __builtin_memset(input,0,4096);
        unsigned stride=controller->stride/4u;
        input[0]=1u<<dci;input[1]=1u;
        input[stride]=output[0];input[stride+1]=output[1];input[stride+2]=output[2];
        rc=xhci_context_endpoint(input,4096,controller->stride,device->usb.speed,
                                  &state->endpoint,state->hid,state->replacement.physical);
    }
    if(!rc) {
        struct xhci_trb command={.lo=(uint32_t)device->input.physical,
            .hi=(uint32_t)(device->input.physical>>32),
            .control=XHCI_TRB_TYPE(12)|((uint32_t)device->slot_id<<24)};
        __atomic_thread_fence(__ATOMIC_SEQ_CST);rc=xhci_command(controller,command,NULL);
    }
    if(rc) {
        if(!__atomic_load_n(&controller->stop.failed,__ATOMIC_ACQUIRE))
            xhci_dma_release(&state->replacement);
        return rc;
    }
    uint64_t flags;kernel_spin_lock_irqsave(&controller->event_lock,&flags);
    struct xhci_dma old=state->dma;state->dma=state->replacement;
    state->replacement=(struct xhci_dma){0};state->ring=replacement;
    kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
    xhci_dma_release(&old);
    uint8_t setup[8]={2,1,0,0,state->endpoint.address,0,0,0};uint32_t actual=0;
    rc=device->usb.operations->control(&device->usb,setup,NULL,0,&actual);
    if(!rc && actual)rc=-EPROTO;
    if(rc)__atomic_store_n(&controller->stop.failed,true,__ATOMIC_RELEASE);
    return rc;
}

static int clear_halt(struct usb_device *usb,struct usb_endpoint *endpoint)
{
    if (!endpoint)return -EINVAL;
    struct xhci_device *device=usb->host_context;unsigned dci=endpoint_dci(endpoint->address);
    if (__atomic_load_n(&usb->disconnected,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&device->controller->stop.failed,__ATOMIC_ACQUIRE))return -ENODEV;
    if (dci<2 || dci>=XHCI_ENDPOINTS)return -EINVAL;
    struct xhci_endpoint_state *state=&device->endpoints[dci];
    if (!state->configured)return -EINVAL;
    if (!state->needs_recovery)return reset_idle_endpoint(device,dci);
    return xhci_endpoint_recover(device,dci,true);
}
const struct usb_device_operations xhci_device_operations={
    .acquire=acquire,.release=release,.control=control,.bulk=bulk,.clear_halt=clear_halt,
};
