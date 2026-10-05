#ifndef RELIEFNT_DRIVER_RESOURCES_H
#define RELIEFNT_DRIVER_RESOURCES_H
#include <reliefos/driver.h>
#include <reliefnt/types.h>
/** @brief Allocate an init-owned zeroed DMA buffer wholly inside its mask.
 * @param owner Loading module slot; must be less than RELIEFOS_DRIVER_MAX.
 * @param pages Requested 4 KiB pages.
 * @param mask Maximum inclusive final bus byte.
 * @return Owned physical run or 0; allocation failure is represented as ENOMEM
 * by the driver. Task context during init; caller explicitly frees or rollback does.
 */
uint64_t driver_alloc_dma_owned(uint32_t owner,uint32_t pages,uint64_t mask);
/** @brief Return one exact DMA lease; duplicate/unmatched frees are ignored.
 * @param phys Original owned physical base.
 * @param pages Original page count.
 * @return None. Task context, hardware quiesced; no callback or held ledger lock.
 */
void driver_free_dma(uint64_t phys,uint32_t pages);
/** @brief Map a quiesced BAR at init and register its owned mapping lease.
 * @param owner Loading module slot; must be less than RELIEFOS_DRIVER_MAX.
 * @param phys Aligned memory BAR base.
 * @param bytes Requested bytes entirely inside BAR pages.
 * @return Owned kernel pointer or NULL. Task context; acquires a short execution
 * transaction and restores attributes if ledger registration fails.
 */
void *driver_map_mmio_owned(uint32_t owner,uint64_t phys,uint64_t bytes);
/** @brief Release a matching owned BAR mapping after hardware is stopped.
 * @param ptr Original module pointer.
 * @param bytes Original requested bytes.
 * @return None. Task context; acquires a short execution transaction and synchronizes all CPUs.
 */
void driver_unmap_mmio(void *ptr,uint64_t bytes);
/** @brief Register an init-owned MSI callback and its cleanup obligation.
 * @param owner Loading module slot; must be less than RELIEFOS_DRIVER_MAX.
 * @param dev Borrowed quiesced PCI function identity.
 * @param handler Nonblocking W1C callback; read/clear device before returning.
 * @param opaque Borrowed module context retained until free synchronization.
 * @param out_handle Receives an owned generation token.
 * @return 0 or provider negative errno. Task context, no callback under ledger lock.
 */
int driver_request_pci_irq_owned(uint32_t owner,const struct reliefos_driver_pci_device *dev,
                                 void (*handler)(void *),void *opaque,uint32_t *out_handle);
/** @brief Remove an exact module IRQ lease, then disable and synchronize MSI.
 * @param handle Owned generation token.
 * @return None. Task context, never from ISR; module stays mapped through return.
 */
void driver_free_pci_irq(uint32_t handle);
/** @brief Clean up owner resources in provider order outside the ledger lock.
 * @param owner Loading module slot; must be less than RELIEFOS_DRIVER_MAX.
 * @param irq_only Nonzero cancels IRQs before fini; zero frees remaining resources.
 * @return None. Task context. Core has stopped streams before IRQ removal; fini
 * must quiesce controller rings before remaining DMA/MMIO resources are reclaimed.
 * Caller runs irq_only cleanup without execution ownership and full cleanup under
 * execution ownership; each consumed MMIO lease is released directly to the provider
 * under that transaction.
 */
void driver_resources_release(uint32_t owner,bool irq_only);
#endif
