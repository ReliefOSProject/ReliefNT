/* SPDX-License-Identifier: Apache-2.0 */
#include "ahci.h"
#include <reliefnt/pci_irq.h>
#include <reliefnt/storage_wait.h>

/* DHRS, PSS, DSS, SDBS, DPS and fatal/interface/task-file errors. No hotplug
 * admission is implied; polling fallback still checks CI and the taskfile. */

/** @brief Preserve port error evidence before acknowledging W1C sources and EOI.
 * @param context IRQ-pinned controller with immutable per-port DMA pointers.
 * Bounded IRQ work, no list traversal, command submission or filesystem lock.
 */
static void interrupt(void *context)
{
    struct ahci_controller *c=context;
    bool notify=false;
    for(unsigned pass=0;pass<4;++pass) {
        uint32_t pending=c->abar->is&c->implemented;
        if(!pending)break;
        for(unsigned p=0;p<32;++p)if(pending&(1u<<p)) {
            struct ahci_hba_port *port=&c->abar->ports[p];
            struct ahci_port_dma *dma=c->ports[p];
            uint64_t flags=0;
            if(dma)kernel_spin_lock_irqsave(&dma->irq_lock,&flags);
            uint32_t status=port->is;
            if(dma)dma->irq_status|=status;
            else port->ie=0;
            __atomic_thread_fence(__ATOMIC_RELEASE);
            port->is=status;
            if(dma)kernel_spin_unlock_irqrestore(&dma->irq_lock,flags);
            notify|=!!status;
        }
        c->abar->is=pending;
        (void)c->abar->is;
    }
    if(notify)storage_completion_notify();
}

/** @brief Combine live status with evidence saved before interrupt W1C.
 * @param port Task-owned stable port. @return Raw/latching AHCI status bits.
 */
uint32_t ahci_interrupt_status(struct ahci_hba_port *port)
{
    struct ahci_port_dma *dma=ahci_dma_find(port);
    if(!dma)return port->is;
    uint64_t flags;kernel_spin_lock_irqsave(&dma->irq_lock,&flags);
    uint32_t status=port->is|dma->irq_status;
    kernel_spin_unlock_irqrestore(&dma->irq_lock,flags);
    return status;
}

/** @brief Retire the old command's latch and status atomically against the ISR.
 * @param dma Stable idle port; task holds transport exclusion, no hardware slot owned.
 */
void ahci_interrupt_begin(struct ahci_port_dma *dma)
{
    uint64_t flags;kernel_spin_lock_irqsave(&dma->irq_lock,&flags);
    dma->irq_status=0;dma->port->is=0xffffffffu;
    kernel_spin_unlock_irqrestore(&dma->irq_lock,flags);
}

/** @brief Enable single-message MSI only after every active port context exists.
 * @param c Probed stable controller, init task. No INTx/MSI-X port routing guesses.
 */
void ahci_interrupt_start(struct ahci_controller *c)
{
    struct reliefos_driver_pci_device dev={.bus=c->bus,.slot=c->slot,.function=c->function};
    if(pci_request_irq(&dev,interrupt,c,&c->irq_handle))return;
    for(unsigned p=0;p<32;++p)if(c->ports[p]) {
        struct ahci_port_dma *dma=c->ports[p];
        uint64_t flags;kernel_spin_lock_irqsave(&dma->irq_lock,&flags);
        dma->irq_status=0;dma->irq_enabled=true;
        dma->port->is=0xffffffffu;dma->port->ie=AHCI_IRQ_MASK;
        kernel_spin_unlock_irqrestore(&dma->irq_lock,flags);
    }
    c->abar->is=c->implemented;
    c->abar->ghc|=2u;
    (void)c->abar->ghc;
}

/** @brief Disable port/global sources and drain callback pins before native STOP.
 * @param c Stable controller with port DMA/BAR retained through return.
 * The callback never acquires execution ownership; no port lock spans free.
 */
void ahci_interrupt_stop(struct ahci_controller *c)
{
    c->abar->ghc&=~2u;
    for(unsigned p=0;p<32;++p)if(c->implemented&(1u<<p)) {
        struct ahci_port_dma *dma=c->ports[p];
        uint64_t flags=0;
        if(dma)kernel_spin_lock_irqsave(&dma->irq_lock,&flags);
        c->abar->ports[p].ie=0;
        if(dma)dma->irq_enabled=false;
        if(dma)kernel_spin_unlock_irqrestore(&dma->irq_lock,flags);
    }
    (void)c->abar->ghc;
    if(c->irq_handle)pci_free_irq(c->irq_handle);
    c->irq_handle=0;
}
