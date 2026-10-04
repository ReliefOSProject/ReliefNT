#include <reliefnt/driver_resources.h>
#include <reliefnt/mm.h>
#include <reliefnt/pci.h>
#include <reliefnt/pci_irq.h>
#include <reliefnt/lock.h>
#define DRIVER_RESOURCE_MAX 256U
#define DRIVER_RESOURCE_DMA 1U
#define DRIVER_RESOURCE_MMIO 2U
#define DRIVER_RESOURCE_IRQ 3U
struct driver_resource { uint32_t live, owner, kind; uint64_t address, amount; };
static struct driver_resource driver_resources[DRIVER_RESOURCE_MAX];
static struct kernel_spinlock driver_resource_lock = KERNEL_SPINLOCK_INIT;
/** @brief Record a module-owned resource allocated during serialized init.
 * @param owner Loading module slot; must be less than RELIEFOS_DRIVER_MAX.
 * @param kind DMA/MMIO/IRQ resource selector.
 * @param address Physical address, pointer value or IRQ token.
 * @param amount Pages or mapping bytes, zero for IRQ.
 * @return 0 or -ENOSPC/-EINVAL/-EBUSY (another owner maps this BAR). IRQ-safe ledger; no callback/ownership on failure.
 */
static int driver_resource_add(uint32_t owner,uint32_t kind, uint64_t address, uint64_t amount)
{
    if (owner >= RELIEFOS_DRIVER_MAX) return -22;
    uint64_t flags;
    kernel_spin_lock_irqsave(&driver_resource_lock,&flags);
    if (kind==DRIVER_RESOURCE_MMIO) for(uint32_t i=0;i<DRIVER_RESOURCE_MAX;++i) {
        const struct driver_resource *r=&driver_resources[i];
        if(r->live && r->kind==kind && r->owner!=owner &&
           address < r->address+r->amount && r->address < address+amount) {
            kernel_spin_unlock_irqrestore(&driver_resource_lock,flags);return -16;
        }
    }
    for (uint32_t i=0;i<DRIVER_RESOURCE_MAX;++i) if (!driver_resources[i].live) {
        driver_resources[i]=(struct driver_resource){.live=1,.owner=owner,
            .kind=kind,.address=address,.amount=amount};
        kernel_spin_unlock_irqrestore(&driver_resource_lock,flags); return 0;
    }
    kernel_spin_unlock_irqrestore(&driver_resource_lock,flags); return -28;
}
/** @brief Remove exactly one owned resource lease before releasing its provider.
 * @param kind Resource selector.
 * @param address Resource identity.
 * @param amount Exact allocation size.
 * @return True if a matching lease was claimed for release. IRQ-safe ledger only.
 */
static bool driver_resource_remove(uint32_t kind,uint64_t address,uint64_t amount)
{
    uint64_t flags; kernel_spin_lock_irqsave(&driver_resource_lock,&flags);
    for(uint32_t i=0;i<DRIVER_RESOURCE_MAX;++i) if(driver_resources[i].live &&
        driver_resources[i].kind==kind && driver_resources[i].address==address && driver_resources[i].amount==amount) {
        driver_resources[i].live=0;kernel_spin_unlock_irqrestore(&driver_resource_lock,flags);return true;
    }
    kernel_spin_unlock_irqrestore(&driver_resource_lock,flags);return false;
}
/** @brief Allocate an init-owned zeroed DMA buffer wholly inside its mask.
 * @param owner Loading module slot; must be less than RELIEFOS_DRIVER_MAX.
 * @param pages Requested 4 KiB pages.
 * @param mask Maximum inclusive final bus byte.
 * @return Owned physical run or 0; allocation failure is represented as ENOMEM
 * by the driver. Task context during init; caller explicitly frees or rollback does.
 */
uint64_t driver_alloc_dma_owned(uint32_t owner,uint32_t pages,uint64_t mask)
{
    if(owner>=RELIEFOS_DRIVER_MAX)return 0;
    uint64_t phys=mm_alloc_pages_below(pages,mask);
    if(phys && driver_resource_add(owner,DRIVER_RESOURCE_DMA,phys,pages)) {mm_free_pages(phys,pages);return 0;}
    return phys;
}
/** @brief Return one exact DMA lease; duplicate/unmatched frees are ignored.
 * @param phys Original owned physical base.
 * @param pages Original page count.
 * @return None. Task context, hardware quiesced; no callback or held ledger lock.
 */
void driver_free_dma(uint64_t phys,uint32_t pages)
{
    if(driver_resource_remove(DRIVER_RESOURCE_DMA,phys,pages)) mm_free_pages(phys,pages);
}
/** @brief Map a quiesced BAR at init and register its owned mapping lease.
 * @param owner Loading module slot; must be less than RELIEFOS_DRIVER_MAX.
 * @param phys Aligned memory BAR base.
 * @param bytes Requested bytes entirely inside BAR pages.
 * @return Owned kernel pointer or NULL. Task context; wrapper acquires a short
 * execution transaction. Rollback restores attributes if ledger registration fails.
 */
void *driver_map_mmio_owned(uint32_t owner,uint64_t phys,uint64_t bytes)
{
    if(owner>=RELIEFOS_DRIVER_MAX)return NULL;
    uint64_t execution_flags;
    kernel_execution_lock_irqsave(&execution_flags);
    void *ptr=pci_map_mmio(phys,bytes);
    if(ptr && driver_resource_add(owner,DRIVER_RESOURCE_MMIO,(uint64_t)(uintptr_t)ptr,bytes)) {
        pci_unmap_mmio(ptr,bytes);ptr=NULL;
    }
    kernel_execution_unlock_irqrestore(execution_flags);
    return ptr;
}
/** @brief Release a matching owned BAR mapping after hardware is stopped.
 * @param ptr Original module pointer.
 * @param bytes Original requested bytes.
 * @return None. Task context; acquires a short execution transaction and synchronizes all CPUs.
 */
void driver_unmap_mmio(void *ptr,uint64_t bytes)
{
    uint64_t execution_flags;
    kernel_execution_lock_irqsave(&execution_flags);
    if(driver_resource_remove(DRIVER_RESOURCE_MMIO,(uint64_t)(uintptr_t)ptr,bytes))
        pci_unmap_mmio(ptr,bytes);
    kernel_execution_unlock_irqrestore(execution_flags);
}
/** @brief Register an init-owned MSI callback and its cleanup obligation.
 * @param owner Loading module slot; must be less than RELIEFOS_DRIVER_MAX.
 * @param dev Borrowed quiesced PCI function identity.
 * @param handler Nonblocking W1C callback; read/clear device before returning.
 * @param opaque Borrowed module context retained until free synchronization.
 * @param out_handle Receives an owned generation token.
 * @return 0 or provider negative errno. Task context, no callback under ledger lock.
 */
int driver_request_pci_irq_owned(uint32_t owner,const struct reliefos_driver_pci_device *dev,
                                 void (*handler)(void *),void *opaque,uint32_t *out_handle)
{
    if(owner>=RELIEFOS_DRIVER_MAX || !out_handle)return -22;
    uint32_t handle;int ret=pci_request_irq(dev,handler,opaque,&handle);
    if(ret)return ret;
    ret=driver_resource_add(owner,DRIVER_RESOURCE_IRQ,handle,0);
    if(ret){pci_free_irq(handle);return ret;}
    *out_handle=handle;return 0;
}
/** @brief Remove an exact module IRQ lease, then disable and synchronize MSI.
 * @param handle Owned generation token.
 * @return None. Task context, never from ISR; module stays mapped through return.
 */
void driver_free_pci_irq(uint32_t handle)
{
    if(driver_resource_remove(DRIVER_RESOURCE_IRQ,handle,0))pci_free_irq(handle);
}
/** @brief Clean up owner resources in provider order outside the ledger lock.
 * @param owner Loading module slot; must be less than RELIEFOS_DRIVER_MAX.
 * @param irq_only Nonzero cancels IRQs before fini; zero frees remaining resources.
 * @return None. Task context. Core has stopped streams before IRQ removal; fini
 * must quiesce controller rings before remaining DMA/MMIO resources are reclaimed.
 * The caller runs irq_only cleanup without execution ownership and full cleanup
 * under the execution transaction; each consumed MMIO lease is released directly
 * to the provider under that transaction.
 */
void driver_resources_release(uint32_t owner,bool irq_only)
{
    for(uint32_t i=0;i<DRIVER_RESOURCE_MAX;++i){
        uint64_t flags;kernel_spin_lock_irqsave(&driver_resource_lock,&flags);
        struct driver_resource r=driver_resources[i];
        if(!r.live || r.owner!=owner || (irq_only && r.kind!=DRIVER_RESOURCE_IRQ)){
            kernel_spin_unlock_irqrestore(&driver_resource_lock,flags);continue;
        }
        driver_resources[i].live=0;kernel_spin_unlock_irqrestore(&driver_resource_lock,flags);
        if(r.kind==DRIVER_RESOURCE_IRQ)pci_free_irq((uint32_t)r.address);
        else if(r.kind==DRIVER_RESOURCE_DMA)mm_free_pages(r.address,(uint32_t)r.amount);
        else if(r.kind==DRIVER_RESOURCE_MMIO)
            pci_unmap_mmio((void*)(uintptr_t)r.address,r.amount);
    }
}
