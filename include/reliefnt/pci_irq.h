#ifndef RELIEFNT_PCI_IRQ_H
#define RELIEFNT_PCI_IRQ_H
#include <reliefos/driver.h>
/** @brief Allocate one MSI vector routed to the BSP; no speculative INTx.
 * @param dev Borrowed PCI function, quiesced by driver before registration.
 * @param handler Nonblocking callback that masks/acknowledges its source before EOI.
 * @param opaque Borrowed callback context; pinned until free returns.
 * @param out_handle Receives generation-tagged owned handle on success.
 * @return 0, -EINVAL, -ENOSPC or -ENOTSUP. Task context; no sleeping lock.
 * Each vector is issued at most once per boot; polling-capable drivers may
 * fall back when the fixed 16-vector budget is exhausted.
 */
int pci_request_irq(const struct reliefos_driver_pci_device *dev, void (*handler)(void *),
                    void *opaque, uint32_t *out_handle);
/** @brief Allocate MSI-X entry zero routed to the BSP, masking unused entries.
 * @param dev Exclusive quiesced function. @param handler Nonblocking IRQ callback.
 * @param opaque Stable context through free. @param out_handle Owned token output.
 * @return Zero or negative EINVAL/ENOTSUP/EBUSY/ENOSPC/ENOMEM.
 * Task context, with bounded table/PBA validation and synchronized mappings.
 * Shares MSI's never-reused vector budget. Release with pci_free_irq.
 */
int pci_request_msix_irq(const struct reliefos_driver_pci_device *dev,
    void (*handler)(void *), void *opaque, uint32_t *out_handle);
/** @brief Bind MSI-X entry zero to the currently initializing driver owner.
 * @param dev Exclusive quiesced function. @param handler Nonblocking IRQ callback.
 * @param opaque Stable module context. @param out_handle Owned generation output.
 * @return Zero or provider/ledger errno. Init task context; automatic owner
 * cleanup disables messages and drains handlers before module fini/unmapping.
 */
int driver_request_pci_msix_irq(const struct reliefos_driver_pci_device *dev,
    void (*handler)(void *), void *opaque, uint32_t *out_handle);
/** @brief Invoke the MSI/MSI-X callback with an in-flight execution pin.
 * @param vector Hardware vector in 0x50..0x5f.
 * @return None. IRQ context; callback must mask/ack its source, never sleep.
 * Architecture sends EOI only after this returns. Removal waits for the pin.
 */
void pci_irq_dispatch(uint32_t vector);
/** @brief Disable MSI/MSI-X, drain ISR pins and retire the vector until reboot.
 * @param handle Owned generation token; stale/duplicate frees are harmless.
 * @return None. Task context only, no execution lock and never from handler.
 * Caller first masks the device source and keeps callback memory alive until
 * return. DMA quiescence must be established before freeing DMA resources.
 * Pending messages can still arrive; the vector never names a new owner.
 */
void pci_free_irq(uint32_t handle);
#endif
