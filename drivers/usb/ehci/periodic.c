/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/console.h>
#include <reliefnt/framebuffer.h>

/** @brief Arm an inactive per-interface qTD; publish the QH link last. */
static void arm(struct ehci_controller *controller, unsigned slot)
{
    struct ehci_hid *hid=&controller->hid[slot];
    struct ehci_qtd *td=&controller->dma->hid_descriptors[slot];
    struct ehci_qh *qh=&controller->dma->hid_heads[slot];
    (void)ehci_qtd_prepare(controller,td,EHCI_PID_IN,hid->endpoint.toggle,
        controller->dma->reports[slot],hid->endpoint.max_packet);
    qh->current=0;qh->alternate=EHCI_TERMINATE;qh->token=0;
    for (unsigned i=0;i<5;++i) { qh->buffer[i]=0;qh->high_buffer[i]=0; }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    qh->next=ehci_bus(controller,td);
}

/** @brief Rebuild a power-of-two periodic tree only while PSS is clear. */
int ehci_periodic_rebuild(struct ehci_controller *controller)
{
    int rc=ehci_schedule_enable(controller,true,false);if(rc)return rc;
    /* Link longer periods toward compatible shorter-period phases. Every frame
     * referencing a QH then observes the same successor and no unrelated QH. */
    unsigned order[EHCI_MAX_HID],count=0;
    for (unsigned i=0;i<EHCI_MAX_HID;++i)
        if (controller->hid[i].device && !controller->hid[i].device->usb.disconnected) {
            unsigned at=count;
            while (at && controller->hid[order[at-1]].period<controller->hid[i].period) {
                order[at]=order[at-1];--at;
            }
            order[at]=i;++count;
        }
    for (unsigned i=0;i<count;++i) {
        const struct ehci_hid *current=&controller->hid[order[i]];
        uint32_t link=EHCI_TERMINATE;
        for(unsigned j=i+1;j<count;++j) {
            const struct ehci_hid *next=&controller->hid[order[j]];
            if((current->phase&(next->period-1u))==next->phase) {
                link=ehci_bus(controller,&controller->dma->hid_heads[order[j]])|EHCI_LINK_QH;break;
            }
        }
        controller->dma->hid_heads[order[i]].link=link;
    }
    for (unsigned frame=0;frame<EHCI_FRAME_COUNT;++frame) {
        uint32_t link=EHCI_TERMINATE;
        for (unsigned i=0;i<count;++i) {
            const struct ehci_hid *hid=&controller->hid[order[i]];
            if ((frame&(hid->period-1u))==hid->phase) {
                link=ehci_bus(controller,&controller->dma->hid_heads[order[i]])|EHCI_LINK_QH;break;
            }
        }
        controller->dma->frames[frame]=link;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return count ? ehci_schedule_enable(controller,true,true) : 0;
}

/** @brief Select boot protocol and schedule the interface's interrupt IN endpoint. */
int ehci_hid_add(struct ehci_controller *controller, struct ehci_device *device,
                   const struct usb_config_interface *interface)
{
    if (interface->subclass!=1 || (interface->protocol!=1 && interface->protocol!=2) ||
        !interface->interrupt.address || interface->interrupt.max_packet<(interface->protocol==1 ? 8 : 3))
        return -EOPNOTSUPP;
    unsigned slot=0;
    while (slot<EHCI_MAX_HID && controller->hid[slot].device)++slot;
    if (slot==EHCI_MAX_HID) return -ENOSPC;
    struct ehci_bandwidth bandwidth;
    int rc=ehci_bandwidth_admit(controller,&device->usb,&interface->interrupt,&bandwidth);
    if(rc)return rc;
    uint32_t actual;
    rc=ehci_request(&device->usb,0x21,11,0,interface->number,NULL,0,&actual);
    if (rc)return rc;
    /* SET_IDLE is optional; an unsupported request may STALL endpoint zero. */
    rc=ehci_request(&device->usb,0x21,10,0,interface->number,NULL,0,&actual);
    if (rc && rc!=-EPIPE)return rc;
    rc=ehci_schedule_enable(controller,true,false);if(rc)return rc;
    struct ehci_hid *hid=&controller->hid[slot];
    *hid=(struct ehci_hid){.device=device,.endpoint=interface->interrupt,
        .protocol=interface->protocol,.interface_number=interface->number,
        .period=bandwidth.period,.phase=bandwidth.phase,.smask=bandwidth.smask,.cmask=bandwidth.cmask,
        .start_ns=bandwidth.start_ns,.complete_ns=bandwidth.complete_ns,.tt_ns=bandwidth.tt_ns};
    struct ehci_qh *qh=&controller->dma->hid_heads[slot];
    ehci_qh_prepare(qh,&device->usb,hid->endpoint.address,hid->endpoint.max_packet,false);
    qh->characteristics&=~(15u<<28);
    qh->capabilities|=hid->smask|(uint32_t)hid->cmask<<8;
    const struct framebuffer *fb=framebuffer_get();
    if (fb->available) { hid->input.x=fb->width/2;hid->input.y=fb->height/2; }
    arm(controller,slot);
    rc=ehci_periodic_rebuild(controller);
    if (!rc) console_info("[usb] EHCI HID %s address=%u root_port=%u speed=%s\n",
        hid->protocol==1 ? "keyboard" : "mouse",device->usb.address,device->usb.root_port+1,
        device->usb.speed==USB_SPEED_HIGH ? "high" : device->usb.speed==USB_SPEED_LOW ? "low" : "full");
    return rc;
}

/** @brief Drain completed periodic reports without waiting or allocating in IRQ context. */
void ehci_hid_poll(struct ehci_controller *controller)
{
    for (unsigned slot=0;slot<EHCI_MAX_HID;++slot) {
        struct ehci_hid *hid=&controller->hid[slot];
        if (!hid->device || hid->device->usb.disconnected || hid->stalled)continue;
        uint32_t token=controller->dma->hid_descriptors[slot].token;
        if (token&EHCI_TD_ACTIVE)continue;
        uint32_t remaining=token>>16&0x7fffu;
        if ((token&(EHCI_TD_HALTED|EHCI_TD_ERRORS)) || remaining>hid->endpoint.max_packet) {
            hid->stalled=true;usb_hid_boot_release(&hid->input);
            usb_host_request_service(controller->host);continue;
        }
        uint32_t actual=hid->endpoint.max_packet-remaining;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint8_t *report=controller->dma->reports[slot];
        if (hid->protocol==1)usb_hid_boot_keyboard(&hid->input,report,actual);
        else usb_hid_boot_mouse(&hid->input,report,actual);
        hid->endpoint.toggle=token>>31;
        arm(controller,slot);
    }
}

/** @brief Clear interrupt endpoint halts outside IRQ context, retaining failed states. */
void ehci_hid_service(struct ehci_controller *controller)
{
    for (unsigned slot=0;slot<EHCI_MAX_HID;++slot) {
        struct ehci_hid *hid=&controller->hid[slot];
        if (!hid->device || hid->device->usb.disconnected || !hid->stalled)continue;
        int rc=ehci_schedule_enable(controller,true,false);if(rc)return;
        rc=hid->device->usb.operations->clear_halt(&hid->device->usb,&hid->endpoint);
        if (!rc) { hid->stalled=false;arm(controller,slot); }
        (void)ehci_periodic_rebuild(controller);
    }
}

/** @brief Drop removed interfaces only after hardware has left the periodic schedule. */
int ehci_hid_collect(struct ehci_controller *controller)
{
    bool found=false;
    for (unsigned slot=0;slot<EHCI_MAX_HID;++slot)
        if (controller->hid[slot].device && controller->hid[slot].device->usb.disconnected)found=true;
    if (!found)return 0;
    int rc=ehci_schedule_enable(controller,true,false);if(rc)return rc;
    for (unsigned slot=0;slot<EHCI_MAX_HID;++slot) {
        struct ehci_hid *hid=&controller->hid[slot];
        if (hid->device && hid->device->usb.disconnected) {
            usb_hid_boot_release(&hid->input);
            *hid=(struct ehci_hid){0};
            controller->dma->hid_heads[slot].next=EHCI_TERMINATE;
            controller->dma->hid_heads[slot].token=EHCI_TD_HALTED;
        }
    }
    return ehci_periodic_rebuild(controller);
}
