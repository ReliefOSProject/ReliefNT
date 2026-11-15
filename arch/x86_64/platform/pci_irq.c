#include <reliefnt/pci_irq.h>
#include <reliefnt/pci.h>
#include <reliefnt/apic.h>
#include <reliefnt/lock.h>
#include <reliefnt/export.h>
#define IRQ_COUNT 16U
struct pci_irq_entry {
    struct reliefos_driver_pci_device dev;
    uint32_t handle, active;
    uint8_t capability, live, retiring, retired;
    void (*handler)(void *);
    void *opaque;
    void *msix_mapping;
    volatile uint32_t *msix_table;
    uint64_t msix_bytes;
};
static struct pci_irq_entry pci_irqs[IRQ_COUNT];
static uint32_t irq_generation;
static struct kernel_spinlock pci_irq_lock = KERNEL_SPINLOCK_INIT;
/** @brief Find MSI or MSI-X with bounded, cycle-checked capability traversal.
 * @param d Borrowed PCI identity; config access itself is serialized.
 * @param type MSI (5) or MSI-X (0x11) capability ID.
 * @return Complete capability offset, or 0. Task context; no allocation.
 */
static uint8_t find_capability(const struct reliefos_driver_pci_device *d, uint8_t type)
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
        if ((cap & 0xff) == type) {
            uint32_t bytes = 12;
            if (type == 5) {
                bytes = (cap & (1u << 23)) ? 14 : 10;
                if (cap & (1u << 24)) bytes = (cap & (1u << 23)) ? 24 : 20;
            }
            return p + bytes <= 256 ? p : 0;
        }
        p = (cap >> 8) & 0xff;
    }
    return 0;
}
/** @brief Allocate one MSI vector routed to the BSP; no speculative INTx.
 * @param dev Borrowed PCI function, quiesced by driver before registration.
 * @param handler Nonblocking callback that masks/acknowledges its source before EOI.
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
    uint8_t capability = find_capability(dev, 5);
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
    uint8_t msix=find_capability(dev,0x11);
    if (msix) {
        uint16_t c=pci_config_read16(dev->bus,dev->slot,dev->function,msix+2);
        pci_config_write16(dev->bus,dev->slot,dev->function,msix+2,(c&~0x8000u)|0x4000u);
        (void)pci_config_read16(dev->bus,dev->slot,dev->function,msix+2);
    }
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

/** @brief Validate the complete MSI-X structure against its exact memory BAR.
 * @param dev Quiesced function identity. @param location BIR/offset field.
 * @param bytes Actual table/PBA size. @param out Receives the physical start.
 * @return True for a non-overflowing structure whose covering pages fit the BAR.
 * Task context under execution ownership; sizing temporarily disables decode.
 */
static bool msix_range(const struct reliefos_driver_pci_device *dev, uint32_t location,
                        uint64_t bytes, uint64_t *out)
{
    uint8_t bar = location & 7u;
    if (bar >= 6) return false;
    uint8_t offset = 0x10 + bar * 4;
    uint32_t low = pci_config_read32(dev->bus,dev->slot,dev->function,offset);
    if (!low || low == UINT32_MAX || (low & 1u) || ((low & 6u) != 0 && (low & 6u) != 4))
        return false;
    bool wide = (low & 6u) == 4;
    if (wide && bar == 5) return false;
    uint32_t high = wide ? pci_config_read32(dev->bus,dev->slot,dev->function,offset+4) : 0;
    uint64_t base = ((uint64_t)high << 32) | (low & ~15u), start = location & ~7u;
    if (base > UINT64_MAX - start) return false;
    start += base;
    if (!bytes || start > UINT64_MAX - bytes || start + bytes > UINT64_MAX - 4095u)
        return false;
    uint64_t page = start & ~4095ull, end = (start + bytes + 4095u) & ~4095ull;
    struct pci_device identity = {.bus=dev->bus, .slot=dev->slot, .function=dev->function,
        .vendor_id=dev->vendor_id, .device_id=dev->device_id};
    if (!pci_device_bar_contains(&identity,bar,page,end-page)) return false;
    *out = start;
    return true;
}

/** @brief Allocate MSI-X table entry zero with every other entry masked.
 * @param dev Quiesced function with exclusive driver ownership.
 * @param handler IRQ-safe callback; mask/ack the device before returning.
 * @param opaque Context retained until pci_free_irq returns.
 * @param out_handle Receives an owned generation token only on success.
 * @return Zero or negative EINVAL/ENOTSUP/EBUSY/ENOSPC/ENOMEM.
 * Task context; mapping is outside the IRQ lock. No INTx fallback or table
 * offset from an unvalidated BAR is used. A failed prepare enables no vector.
 */
int pci_request_msix_irq(const struct reliefos_driver_pci_device *dev,
    void (*handler)(void *), void *opaque, uint32_t *out_handle)
{
    if (!dev || !handler || !out_handle) return -22;
    if (!apic_enabled() || apic_bsp_id() > 255) return -95;
    uint8_t capability = find_capability(dev, 0x11);
    if (!capability) return -95;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pci_irq_lock,&flags);
    uint32_t slot;
    for (slot=0; slot<IRQ_COUNT; ++slot) {
        struct pci_irq_entry *e=&pci_irqs[slot];
        if ((e->live || e->retiring) && e->dev.bus==dev->bus &&
            e->dev.slot==dev->slot && e->dev.function==dev->function) {
            kernel_spin_unlock_irqrestore(&pci_irq_lock,flags); return -16;
        }
    }
    for (slot=0; slot<IRQ_COUNT; ++slot)
        if (!pci_irqs[slot].live && !pci_irqs[slot].retiring && !pci_irqs[slot].retired) break;
    if (slot==IRQ_COUNT || irq_generation==0xffffffu) {
        kernel_spin_unlock_irqrestore(&pci_irq_lock,flags); return -28;
    }
    struct pci_irq_entry *e=&pci_irqs[slot];
    *e=(struct pci_irq_entry){.dev=*dev, .handle=(++irq_generation<<8)|slot,
        .capability=capability, .retiring=1, .handler=handler, .opaque=opaque};
    kernel_spin_unlock_irqrestore(&pci_irq_lock,flags);

    uint64_t execution_flags;
    kernel_execution_lock_irqsave(&execution_flags);
    uint16_t control=pci_config_read16(dev->bus,dev->slot,dev->function,capability+2);
    uint32_t entries=(control&0x7ffu)+1;
    uint64_t table_bytes=(uint64_t)entries*16, pba_bytes=((entries+63u)/64u)*8u;
    uint32_t table_location=pci_config_read32(dev->bus,dev->slot,dev->function,capability+4);
    uint32_t pba_location=pci_config_read32(dev->bus,dev->slot,dev->function,capability+8);
    uint64_t table_start, pba_start;
    int result=-22;
    if (!msix_range(dev,table_location,table_bytes,&table_start) ||
        !msix_range(dev,pba_location,pba_bytes,&pba_start) ||
        (table_start < pba_start+pba_bytes && pba_start < table_start+table_bytes))
        goto failed;
    uint64_t page=table_start&~4095ull;
    e->msix_bytes=((table_start+table_bytes+4095u)&~4095ull)-page;
    struct pci_device identity={.bus=dev->bus,.slot=dev->slot,.function=dev->function,
        .vendor_id=dev->vendor_id,.device_id=dev->device_id};
    e->msix_mapping=pci_map_device_bar(&identity,table_location&7u,page,e->msix_bytes);
    if (!e->msix_mapping) { result=-12; goto failed; }
    e->msix_table=(volatile uint32_t *)((uint8_t *)e->msix_mapping+(table_start-page));
    pci_config_write16(dev->bus,dev->slot,dev->function,capability+2,(control&~0x8000u)|0x4000u);
    (void)pci_config_read16(dev->bus,dev->slot,dev->function,capability+2);
    for (uint32_t i=0;i<entries;++i) e->msix_table[i*4+3] |= 1u;
    e->msix_table[0]=0xfee00000u | ((uint32_t)apic_bsp_id()<<12);
    e->msix_table[1]=0;
    e->msix_table[2]=0x50+slot;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    (void)e->msix_table[3]; /* Flush posted table programming before enable. */
    uint8_t msi=find_capability(dev,5);
    if (msi) {
        uint16_t c=pci_config_read16(dev->bus,dev->slot,dev->function,msi+2);
        pci_config_write16(dev->bus,dev->slot,dev->function,msi+2,c&~1u);
    }
    kernel_spin_lock_irqsave(&pci_irq_lock,&flags);
    e->live=1; e->retiring=0;
    e->msix_table[3] &= ~1u;
    (void)e->msix_table[3];
    pci_config_write16(dev->bus,dev->slot,dev->function,capability+2,(control&~0x4000u)|0x8000u);
    (void)pci_config_read16(dev->bus,dev->slot,dev->function,capability+2);
    *out_handle=e->handle;
    kernel_spin_unlock_irqrestore(&pci_irq_lock,flags);
    kernel_execution_unlock_irqrestore(execution_flags);
    return 0;
failed:
    kernel_execution_unlock_irqrestore(execution_flags);
    kernel_spin_lock_irqsave(&pci_irq_lock,&flags);
    *e=(struct pci_irq_entry){0};
    kernel_spin_unlock_irqrestore(&pci_irq_lock,flags);
    return result;
}
/** @brief Invoke the MSI/MSI-X callback with an in-flight execution pin.
 * @param vector Hardware vector in 0x50..0x5f.
 * @return None. IRQ context; callback must mask/ack its source, never sleep.
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
/** @brief Disable MSI/MSI-X, drain ISR pins, unmap the table and retire the vector.
 * @param handle Owned generation token; stale/duplicate frees are harmless.
 * @return None. Task context only, never from handler or with a lock needed by
 * it. An execution writer is allowed only for handlers that never acquire it.
 * Caller first closes command/source admission and retains callback memory
 * through return. PCI messages are disabled before callback pins drain;
 * DMA quiescence must be established before freeing DMA resources.
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
    if (e->msix_mapping) {
        pci_config_write16(d->bus,d->slot,d->function,e->capability+2,(control&~0x8000u)|0x4000u);
        e->msix_table[3] |= 1u;
        (void)e->msix_table[3];
    } else {
        pci_config_write16(d->bus,d->slot,d->function,e->capability + 2,control & ~1u);
    }
    (void)pci_config_read16(d->bus,d->slot,d->function,e->capability + 2);
    e->live = 0; e->retiring = 1;
    kernel_spin_unlock_irqrestore(&pci_irq_lock, flags);
    while (__atomic_load_n(&e->active, __ATOMIC_ACQUIRE)) __asm__ volatile("pause" ::: "memory");
    if (e->msix_mapping) {
        uint64_t execution_flags;
        kernel_execution_lock_irqsave(&execution_flags);
        pci_unmap_mmio(e->msix_mapping,e->msix_bytes);
        kernel_execution_unlock_irqrestore(execution_flags);
    }
    kernel_spin_lock_irqsave(&pci_irq_lock, &flags);
    e->handler = NULL; e->opaque = NULL; e->retired = 1; e->retiring = 0;
    e->msix_mapping=NULL; e->msix_table=NULL; e->msix_bytes=0;
    kernel_spin_unlock_irqrestore(&pci_irq_lock, flags);
}
EXPORT_SYMBOL(pci_request_irq);
EXPORT_SYMBOL(pci_request_msix_irq);
EXPORT_SYMBOL(pci_free_irq);
