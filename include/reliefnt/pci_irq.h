#ifndef RELIEFNT_PCI_IRQ_H
#define RELIEFNT_PCI_IRQ_H
#include <reliefos/driver.h>
/** @brief Allocate one MSI vector routed to the BSP; no speculative INTx.
 * @param dev Borrowed PCI function, quiesced by driver before registration.
 * @param handler Nonblocking callback that reads/clears device W1C before EOI.
 * @param opaque Borrowed callback context; pinned until free returns.
 * @param out_handle Receives generation-tagged owned handle on success.
 * @return 0, -EINVAL, -ENOSPC or -ENOTSUP. Task context; no sleeping lock.
 * Each vector is issued at most once per boot; polling-capable drivers may
 * fall back when the fixed 16-vector budget is exhausted.
 */
int pci_request_irq(const struct reliefos_driver_pci_device *dev, void (*handler)(void *),
                    void *opaque, uint32_t *out_handle);
/** @brief Invoke the real MSI callback with an in-flight execution pin.
 * @param vector Hardware vector in 0x50..0x5f.
 * @return None. IRQ context; callback must acknowledge W1C, never sleep.
 * Architecture sends EOI only after this returns. Removal waits for the pin.
 */
void pci_irq_dispatch(uint32_t vector);
/** @brief Disable MSI, drain ISR pins and retire the vector until reboot.
 * @param handle Owned generation token; stale/duplicate frees are harmless.
 * @return None. Task context only, no execution lock and never from handler.
 * Caller first stops device DMA and keeps callback memory alive until return.
 * Pending messages can still arrive; the vector never names a new owner.
 */
void pci_free_irq(uint32_t handle);
#endif
