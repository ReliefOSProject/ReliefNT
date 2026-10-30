/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

#define XHCI_STATUS_FATAL ((1u<<2)|(1u<<12))

/* Unlike a generic register wait, start-up must reject fatal status even if
 * HCH has already cleared, or while it remains set waiting for RUN. */
static int wait_running(struct xhci_controller *controller,uint32_t status_offset)
{
    struct xhci_timer timer;int rc=xhci_timer_start(&timer);if(rc)return rc;
    for(unsigned spin=0;;++spin) {
        uint32_t status=xhci_read(controller,status_offset);
        if(status&XHCI_STATUS_FATAL)return -EIO;
        if(!(status&1u))return 0;
        if(xhci_timer_expired(&timer,1000000u) || spin==50000000u)return -ETIMEDOUT;
        __asm__ volatile("pause" ::: "memory");
    }
}

int xhci_start_rings(struct xhci_controller *controller,uint16_t pci_command)
{
    if(!controller || !controller->owned || !controller->stop.stopped)return -EINVAL;
    const struct pci_device *pci=&controller->pci;
    uint32_t op=controller->operational,rt=controller->runtime;
    /* All controller-visible structures are initialized while HALT and bus
     * mastering are confirmed. Once BME may be enabled, cleanup must halt and
     * disable it again before any of that memory can be released. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    controller->stop.stopped=false;
    __atomic_store_n(&controller->stop.failed,false,__ATOMIC_RELEASE);
    pci_config_write16(pci->bus,pci->slot,pci->function,4,pci_command|6u);
    if ((pci_config_read16(pci->bus,pci->slot,pci->function,4)&6u)!=6u) {
        __atomic_store_n(&controller->stop.failed,true,__ATOMIC_RELEASE);return -EIO;
    }
    xhci_write64(controller,op+0x30u,controller->dcbaa.physical);
    xhci_write64(controller,op+0x18u,controller->commands.physical|1u);
    if (__atomic_load_n(&controller->stop.failed,__ATOMIC_ACQUIRE))return -EIO;
    xhci_write(controller,op+0x38u,controller->slots);
    xhci_write(controller,rt+0x20u,0);
    xhci_write(controller,rt+0x28u,1);
    xhci_write64(controller,rt+0x38u,controller->events.physical);
    if (__atomic_load_n(&controller->stop.failed,__ATOMIC_ACQUIRE))return -EIO;
    /* ERSTBA activates the event-ring state machine, which may fetch DMA
     * immediately, even while the controller itself is halted. */
    xhci_write64(controller,rt+0x30u,controller->erst.physical);
    if (__atomic_load_n(&controller->stop.failed,__ATOMIC_ACQUIRE))return -EIO;
    if (xhci_read(controller,op+4u)&XHCI_STATUS_FATAL) {
        __atomic_store_n(&controller->stop.failed,true,__ATOMIC_RELEASE);return -EIO;
    }
    xhci_write(controller,op,1u);
    int rc=wait_running(controller,op+4u);
    if(rc)__atomic_store_n(&controller->stop.failed,true,__ATOMIC_RELEASE);
    return rc;
}
