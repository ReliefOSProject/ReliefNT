/* SPDX-License-Identifier: Apache-2.0 */
#include "storage_nvme.h"
#include <reliefnt/pci_irq.h>
#include <reliefnt/pci.h>
#include <reliefnt/storage_wait.h>

/** @brief Acknowledge phase-valid entries under completion_lock, with bounded work.
 * @param c Live mapped controller; caller holds the IRQ-safe completion lock.
 * @return Number acknowledged. No data copy, reset, allocation or task pointer.
 */
static unsigned drain(struct nvme_controller *c)
{
    if(!c->queues_live || !c->io_cq)return 0;
    unsigned count=0;
    while(count<c->queue_depth) {
        volatile struct nvme_completion *entry=&c->io_cq[c->io_cq_head];
        uint16_t status=entry->status;
        if((status&1u)!=c->cq_phase_io)break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        bool matches=c->completion_active && !c->completion_done &&
            entry->sq_id==1 && entry->command_id==c->completion_cid;
        if(!matches)c->queue_fault=true;
        c->completion_result=(!matches || (status&0xfffeu)) ? -5 : 0;
        c->completion_done=true;
        c->io_cq_head=(c->io_cq_head+1u)%c->queue_depth;
        if(!c->io_cq_head)c->cq_phase_io^=1;
        ++count;
    }
    if(count) {
        __atomic_thread_fence(__ATOMIC_RELEASE);
        *(volatile uint32_t *)(c->mmio+0x1000u+3u*c->doorbell_stride)=c->io_cq_head;
    }
    return count;
}

/** @brief Confirm I/O completions before EOI and notify the permanent core queue.
 * @param context IRQ-pinned controller retained until pci_free_irq returns.
 * No filesystem, transport or execution writer is acquired in this callback.
 */
static void interrupt(void *context)
{
    struct nvme_controller *c=context;
    uint64_t flags;kernel_spin_lock_irqsave(&c->completion_lock,&flags);
    unsigned completed=drain(c);
    kernel_spin_unlock_irqrestore(&c->completion_lock,flags);
    if(completed)storage_completion_notify();
}

/** @brief Submit one command after publishing its CID under IRQ-safe exclusion.
 * @param c Task-locked controller. @param command Prepared stable command.
 * @return Zero or -EINVAL/-EBUSY/-EIO; the driver retains all DMA ownership.
 */
int nvme_queue_issue(struct nvme_controller *c,struct nvme_command *command)
{
    if(!c || !command || !c->ready || !c->queue_depth || !c->io_sq)return -22;
    uint64_t flags;kernel_spin_lock_irqsave(&c->completion_lock,&flags);
    int rc=0;
    if(!c->queues_live || c->queue_fault)rc=-5;
    else if(c->completion_active)rc=-16;
    else {
        uint16_t cid=++c->next_command_id;
        if(!cid)cid=++c->next_command_id;
        c->completion_cid=cid;c->completion_done=false;c->completion_active=true;
        command->cdw0=(command->cdw0&0xffu)|((uint32_t)cid<<16);
        c->io_sq[c->io_sq_tail]=*command;
        c->io_sq_tail=(c->io_sq_tail+1u)%c->queue_depth;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        *(volatile uint32_t *)(c->mmio+0x1000u+2u*c->doorbell_stride)=c->io_sq_tail;
    }
    kernel_spin_unlock_irqrestore(&c->completion_lock,flags);
    return rc;
}

/** @brief Poll/consume the same completion state used by the interrupt handler.
 * @param c Task-locked live controller. @return Zero, -EAGAIN or native error.
 * Driver-owned data is not reusable on errors until controller STOP succeeds.
 */
int nvme_queue_poll(struct nvme_controller *c)
{
    uint64_t flags;kernel_spin_lock_irqsave(&c->completion_lock,&flags);
    unsigned completed=drain(c);
    int rc=-11;
    if(!c->queues_live || c->queue_fault)rc=-5;
    else if(!c->completion_active)rc=-22;
    else if(c->completion_done) {
        rc=c->completion_result;c->completion_active=false;
    } else if(*(volatile uint32_t *)(c->mmio+0x1cu)&2u)rc=-5;
    kernel_spin_unlock_irqrestore(&c->completion_lock,flags);
    if(completed)storage_completion_notify();
    return rc;
}

/** @brief Negotiate entry-zero MSI-X or one MSI, retaining a polling fallback.
 * @param c Exclusively owned, mapped and stopped controller. Init task only.
 */
void nvme_interrupt_prepare(struct nvme_controller *c)
{
    struct pci_device pci;
    if(pci_read_device(c->bus,c->slot,c->function,&pci))return;
    struct reliefos_driver_pci_device dev={.bus=c->bus,.slot=c->slot,.function=c->function,
        .vendor_id=pci.vendor_id,.device_id=pci.device_id};
    int rc=pci_request_msix_irq(&dev,interrupt,c,&c->irq_handle);
    if(!rc)c->irq_mode=1;
    else if(!pci_request_irq(&dev,interrupt,c,&c->irq_handle))c->irq_mode=2;
}

/** @brief Drain IRQ callbacks without holding a lock needed by the handler.
 * @param c Stable controller; execution writer is safe because ISR never uses it.
 */
void nvme_interrupt_stop(struct nvme_controller *c)
{
    if(c->irq_handle)pci_free_irq(c->irq_handle);
    c->irq_handle=0;c->irq_mode=0;
}
