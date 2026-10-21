#include <reliefnt/pci_irq.h>
#include <reliefnt/pci.h>
#include <reliefnt/apic.h>
#include <reliefnt/lock.h>
#define IRQ_COUNT 16U
struct pci_irq_entry {
    struct reliefos_driver_pci_device dev;
    uint32_t handle, active;
    uint8_t capability, live, retiring, retired;
    void (*handler)(void *);
    void *opaque;
};
static struct pci_irq_entry pci_irqs[IRQ_COUNT];
static uint32_t irq_generation;
static struct kernel_spinlock pci_irq_lock = KERNEL_SPINLOCK_INIT;
/** @brief Find MSI with bounded, cycle-checked capability traversal.
 * @param d Borrowed PCI identity; config access itself is serialized.
 * @return MSI offset, or 0. Task context; no callback or allocation.
 */
static uint8_t find_msi(const struct reliefos_driver_pci_device *d)
{
    if (!(pci_config_read16(d->bus,d->slot,d->function,6) & 0x10)) return 0;
    uint8_t p = pci_config_read32(d->bus,d->slot,d->function,0x34) & 0xff;
    uint64_t visited = 0;
    for (uint32_t n = 0; p && n < 48; ++n) {
        if (p < 0x40 || p > 0xfc || (p & 3)) return 0;
        uint64_t bit = 1ULL << (p / 4);
        if (visited & bit) return 0;
        visited |= bit;
        uint32_t cap = pci_config_read32(d->bus,d->slot,d->function,p);
        if ((cap & 0xff) == 5) {
            uint32_t bytes = (cap & (1u << 23)) ? 14 : 10;
            if (cap & (1u << 24)) bytes = (cap & (1u << 23)) ? 24 : 20;
            return p + bytes <= 256 ? p : 0;
        }
        p = (cap >> 8) & 0xff;
    }
    return 0;
}
/** @brief Allocate one MSI vector routed to the BSP; no speculative INTx.
 * @param dev Borrowed PCI function, quiesced by driver before registration.
 * @param handler Nonblocking callback that reads/clears device W1C before EOI.
 * @param opaque Borrowed callback context; pinned until free returns.
 * @param out_handle Receives generation-tagged owned handle on success.
 * @return 0, -EINVAL, -ENOSPC or -ENOTSUP. Task context; no sleeping lock.
 * Retired vectors are never reused during this boot. ENOSPC permits a driver
 * with a polling service to degrade safely after the fixed 16-vector budget.
 */
int pci_request_irq(const struct reliefos_driver_pci_device *dev, void (*handler)(void *),
                    void *opaque, uint32_t *out_handle)
{
    if (!dev || !handler || !out_handle) return -22;
    if (!apic_enabled() || apic_bsp_id() > 255) return -95;
    uint8_t capability = find_msi(dev);
    if (!capability) return -95;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pci_irq_lock, &flags);
    for (uint32_t i = 0; i < IRQ_COUNT; ++i) if (pci_irqs[i].live || pci_irqs[i].retiring) {
        struct reliefos_driver_pci_device *d = &pci_irqs[i].dev;
        if (d->bus == dev->bus && d->slot == dev->slot && d->function == dev->function) {
            kernel_spin_unlock_irqrestore(&pci_irq_lock, flags); return -16;
        }
    }
    uint32_t slot;
    for (slot = 0; slot < IRQ_COUNT; ++slot)
        if (!pci_irqs[slot].live && !pci_irqs[slot].retiring && !pci_irqs[slot].retired) break;
    if (slot == IRQ_COUNT || irq_generation == 0xffffffu) { kernel_spin_unlock_irqrestore(&pci_irq_lock, flags); return -28; }
    struct pci_irq_entry *e = &pci_irqs[slot];
    *e = (struct pci_irq_entry){.dev = *dev, .handle = (++irq_generation << 8) | slot,
        .capability = capability, .live = 1, .handler = handler, .opaque = opaque};
    uint16_t control = pci_config_read16(dev->bus,dev->slot,dev->function,capability + 2);
    /* Program exactly one message, preserving MMC and read-only capability bits. */
    control &= ~(1u | (7u << 4));
    pci_config_write16(dev->bus,dev->slot,dev->function,capability + 2,control);
    pci_config_write32(dev->bus,dev->slot,dev->function,capability + 4,0xfee00000u | (apic_bsp_id() << 12));
    if (control & (1u << 7)) pci_config_write32(dev->bus,dev->slot,dev->function,capability + 8,0);
    pci_config_write16(dev->bus,dev->slot,dev->function,capability + ((control & (1u << 7)) ? 12 : 8),0x50 + slot);
    if (control & (1u << 8)) {
        uint8_t mask_offset=capability + ((control & (1u << 7)) ? 16 : 12);
        uint32_t mask=pci_config_read32(dev->bus,dev->slot,dev->function,mask_offset);
        pci_config_write32(dev->bus,dev->slot,dev->function,mask_offset,mask & ~1u);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    pci_config_write16(dev->bus,dev->slot,dev->function,capability + 2,control | 1);
    *out_handle = e->handle;
    kernel_spin_unlock_irqrestore(&pci_irq_lock, flags);
    return 0;
}
/** @brief Invoke the real MSI callback with an in-flight execution pin.
 * @param vector Hardware vector in 0x50..0x5f.
 * @return None. IRQ context; callback must acknowledge W1C, never sleep.
 * Architecture sends EOI only after this returns. Removal waits for the pin.
 */
void pci_irq_dispatch(uint32_t vector)
{
    if (vector < 0x50 || vector > 0x5f) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pci_irq_lock, &flags);
    struct pci_irq_entry *e = &pci_irqs[vector - 0x50];
    void (*handler)(void *) = e->live ? e->handler : NULL;
    void *opaque = e->opaque;
    if (handler) __atomic_add_fetch(&e->active,1,__ATOMIC_ACQUIRE);
    kernel_spin_unlock_irqrestore(&pci_irq_lock, flags);
    if (!handler) return;
    handler(opaque);
    kernel_spin_lock_irqsave(&pci_irq_lock, &flags);
    __atomic_sub_fetch(&e->active,1,__ATOMIC_RELEASE);
    kernel_spin_unlock_irqrestore(&pci_irq_lock, flags);
}
/** @brief Disable MSI, drain ISR pins and retire the vector until reboot.
 * @param handle Owned generation token; stale/duplicate frees are harmless.
 * @return None. Task context only, no execution lock and never from handler.
 * Caller first stops device DMA and keeps callback memory alive until return.
 * A message pending before disable may reach dispatch after this returns;
 * its vector must remain a tombstone rather than identify a new callback.
 */
void pci_free_irq(uint32_t handle)
{
    uint32_t slot = handle & 255;
    if (slot >= IRQ_COUNT) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pci_irq_lock, &flags);
    struct pci_irq_entry *e = &pci_irqs[slot];
    if (!e->live || e->handle != handle) { kernel_spin_unlock_irqrestore(&pci_irq_lock, flags); return; }
    struct reliefos_driver_pci_device *d = &e->dev;
    uint16_t control = pci_config_read16(d->bus,d->slot,d->function,e->capability + 2);
    pci_config_write16(d->bus,d->slot,d->function,e->capability + 2,control & ~1u);
    (void)pci_config_read16(d->bus,d->slot,d->function,e->capability + 2);
    e->live = 0; e->retiring = 1;
    kernel_spin_unlock_irqrestore(&pci_irq_lock, flags);
    while (__atomic_load_n(&e->active, __ATOMIC_ACQUIRE)) __asm__ volatile("pause" ::: "memory");
    kernel_spin_lock_irqsave(&pci_irq_lock, &flags);
    e->handler = NULL; e->opaque = NULL; e->retired = 1; e->retiring = 0;
    kernel_spin_unlock_irqrestore(&pci_irq_lock, flags);
}
