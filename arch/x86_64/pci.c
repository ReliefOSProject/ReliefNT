/*
 * ReliefOS x86_64 PCI access: enumerates and configures PCI devices.
 * Provides low-level configuration-space reads and writes for drivers.
 */
#include <reliefnt/pci.h>
#include <reliefnt/lock.h>
#include <reliefnt/paging.h>

#include "port.h"

#define PCI_CONFIG_ADDR 0xcf8u
#define PCI_CONFIG_DATA 0xcfcu
static struct kernel_spinlock pci_config_lock = KERNEL_SPINLOCK_INIT;

/**
 * Pci make addr.
 * @param bus Value supplied by the caller.
 * @param slot Value supplied by the caller.
 * @param function Value supplied by the caller.
 * @param offset Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t pci_make_addr(uint8_t bus, uint8_t slot, uint8_t function,
                              uint8_t offset)
{
    return 0x80000000u |
           ((uint32_t)bus << 16) |
           ((uint32_t)slot << 11) |
           ((uint32_t)function << 8) |
           ((uint32_t)offset & 0xfcu);
}

/**
 * @brief Read raw config word under the caller-owned config transaction.
 * @param bus Value supplied by the caller.
 * @param slot Value supplied by the caller.
 * @param function Value supplied by the caller.
 * @param offset Value supplied by the caller.
 * @return Config word. Caller holds IRQ-safe config lock; no ownership.
 */
static uint32_t pci_raw_read32(uint8_t bus, uint8_t slot, uint8_t function,
                           uint8_t offset)
{
    x86_64_outl(pci_make_addr(bus, slot, function, offset), PCI_CONFIG_ADDR);
    return x86_64_inl(PCI_CONFIG_DATA);
}

/**
 * @brief Write raw config word under the caller-owned config transaction.
 * @param bus Value supplied by the caller.
 * @param slot Value supplied by the caller.
 * @param function Value supplied by the caller.
 * @param offset Value supplied by the caller.
 * @param value Value supplied by the caller.
 * @return None. Caller holds IRQ-safe config lock; no ownership.
 */
static void pci_raw_write32(uint8_t bus, uint8_t slot, uint8_t function,
                        uint8_t offset, uint32_t value)
{
    x86_64_outl(pci_make_addr(bus, slot, function, offset), PCI_CONFIG_ADDR);
    x86_64_outl(value, PCI_CONFIG_DATA);
}

/** @brief Write one config halfword without replaying adjacent W1C status.
 * @param bus PCI bus.
 * @param slot PCI slot.
 * @param function PCI function.
 * @param offset Even config offset.
 * @param value New halfword.
 * @return None. Caller holds config lock, IRQs masked; no ownership.
 */
static void pci_raw_write16(uint8_t bus,uint8_t slot,uint8_t function,uint8_t offset,uint16_t value)
{
    x86_64_outl(pci_make_addr(bus,slot,function,offset),PCI_CONFIG_ADDR);
    x86_64_outw(value,PCI_CONFIG_DATA+(offset & 2));
}
/** @brief Serialize the CF8/CFC transaction across IRQs and CPUs.
 * @param bus PCI bus.
 * @param slot PCI slot.
 * @param function PCI function.
 * @param offset Config offset.
 * @return Register value. IRQ-safe config lock; no allocation/ownership.
 */
uint32_t pci_config_read32(uint8_t bus,uint8_t slot,uint8_t function,uint8_t offset)
{
    uint64_t flags; kernel_spin_lock_irqsave(&pci_config_lock,&flags);
    uint32_t value=pci_raw_read32(bus,slot,function,offset);
    kernel_spin_unlock_irqrestore(&pci_config_lock,flags); return value;
}
/** @brief Write an atomic CF8/CFC transaction.
 * @param bus PCI bus.
 * @param slot PCI slot.
 * @param function PCI function.
 * @param offset Config offset.
 * @param value New word.
 * @return None. IRQ-safe config lock, no ownership transfer.
 */
void pci_config_write32(uint8_t bus,uint8_t slot,uint8_t function,uint8_t offset,uint32_t value)
{
    uint64_t flags; kernel_spin_lock_irqsave(&pci_config_lock,&flags);
    pci_raw_write32(bus,slot,function,offset,value);
    kernel_spin_unlock_irqrestore(&pci_config_lock,flags);
}
/** @brief Read a halfword through serialized config access.
 * @param bus PCI bus.
 * @param slot PCI slot.
 * @param function PCI function.
 * @param offset Even offset.
 * @return Halfword. IRQ safe, no ownership.
 */
uint16_t pci_config_read16(uint8_t bus,uint8_t slot,uint8_t function,uint8_t offset)
{
    return (uint16_t)(pci_config_read32(bus,slot,function,offset) >> ((offset & 2)*8));
}
/** @brief Write exactly one halfword, preserving adjacent status W1C bits.
 * @param bus PCI bus.
 * @param slot PCI slot.
 * @param function PCI function.
 * @param offset Even offset.
 * @param value New halfword.
 * @return None. IRQ-safe config lock, no ownership transfer.
 */
void pci_config_write16(uint8_t bus,uint8_t slot,uint8_t function,uint8_t offset,uint16_t value)
{
    uint64_t flags; kernel_spin_lock_irqsave(&pci_config_lock,&flags);
    pci_raw_write16(bus,slot,function,offset,value);
    kernel_spin_unlock_irqrestore(&pci_config_lock,flags);
}

/**
 * Pci read device.
 * @param bus Value supplied by the caller.
 * @param slot Value supplied by the caller.
 * @param function Value supplied by the caller.
 * @param out Output storage updated by the function.
 * @return The value or status produced by the operation.
 */
int pci_read_device(uint8_t bus, uint8_t slot, uint8_t function,
                    struct pci_device *out)
{
    uint32_t id = pci_config_read32(bus, slot, function, 0x00);
    uint32_t class_reg;
    if ((id & 0xffffu) == 0xffffu) {
        return -1;
    }
    if (!out) {
        return 0;
    }
    class_reg = pci_config_read32(bus, slot, function, 0x08);
    *out = (struct pci_device){
        .bus = bus,
        .slot = slot,
        .function = function,
        .vendor_id = (uint16_t)(id & 0xffffu),
        .device_id = (uint16_t)(id >> 16),
        .class_code = (uint8_t)(class_reg >> 24),
        .subclass = (uint8_t)(class_reg >> 16),
        .prog_if = (uint8_t)(class_reg >> 8),
        .revision = (uint8_t)class_reg,
    };
    return 0;
}

/**
 * Pci find device.
 * @param vendor_id Identifier or flags controlling the operation.
 * @param device_id Identifier or flags controlling the operation.
 * @param out Output storage updated by the function.
 * @return The value or status produced by the operation.
 */
int pci_find_device(uint16_t vendor_id, uint16_t device_id,
                    struct pci_device *out)
{
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            for (uint8_t function = 0; function < 8; ++function) {
                struct pci_device dev;
                if (pci_read_device((uint8_t)bus, slot, function, &dev) < 0) {
                    if (function == 0) {
                        break;
                    }
                    continue;
                }
                if (dev.vendor_id == vendor_id && dev.device_id == device_id) {
                    if (out) {
                        *out = dev;
                    }
                    return 0;
                }
            }
        }
    }
    return -1;
}

/** @brief Enumerate every present PCI function by stable bus/slot/function index.
 * @param index Zero-based ordinal among present functions.
 * @param out Receives complete module-facing identity; borrowed output.
 * @return 0, -EINVAL or -ENODEV. Task context, bounded configuration scan.
 */
int pci_enumerate(uint32_t index, struct reliefos_driver_pci_device *out)
{
    if (!out) return -22;
    for (uint32_t b = 0; b < 256; ++b) for (uint32_t s = 0; s < 32; ++s)
        for (uint32_t f = 0; f < 8; ++f) {
            struct pci_device d;
            if (pci_read_device(b,s,f,&d) < 0) { if (!f) break; continue; }
            uint8_t header = (pci_config_read32(b,s,f,0x0c) >> 16) & 0xff;
            if (!index--) {
                *out = (struct reliefos_driver_pci_device){.bus=b,.slot=s,.function=f,
                    .class_code=d.class_code,.vendor_id=d.vendor_id,.device_id=d.device_id,
                    .subclass=d.subclass,.prog_if=d.prog_if,.header_type=header}; return 0;
            }
            if (!f && !(header & 0x80)) break;
        }
    return -19;
}

/** @brief Verify a physical subrange against a memory BAR of a present device.
 * @param phys Requested physical start.
 * @param bytes Requested bytes, nonzero and overflow checked.
 * @return True only if the complete interval fits an implemented memory BAR.
 * Task context, execution transaction; caller has quiesced the candidate device.
 * Saves/restores BAR and decode command while serialized config lock is held.
 */
static bool pci_bar_contains(uint64_t phys, uint64_t bytes)
{
    if (!bytes || phys > UINT64_MAX - bytes) return false;
    struct reliefos_driver_pci_device d;
    for (uint32_t index = 0; !pci_enumerate(index,&d); ++index) {
        uint32_t count = (d.header_type & 0x7f) == 0 ? 6 : (d.header_type & 0x7f) == 1 ? 2 : 0;
        for (uint32_t bar = 0; bar < count; ++bar) {
            uint8_t offset = 0x10 + bar * 4;
            uint32_t low = pci_config_read32(d.bus,d.slot,d.function,offset);
            if (!low || low == UINT32_MAX || (low & 1)) continue;
            uint32_t type = (low >> 1) & 3;
            if (type != 0 && type != 2) continue;
            bool wide = type == 2;
            if (wide && bar + 1 >= count) continue;
            uint32_t high = wide ? pci_config_read32(d.bus,d.slot,d.function,offset+4) : 0;
            uint64_t base = ((uint64_t)high << 32) | (low & ~15u);
            if (wide) ++bar;
            if (phys != base) continue;
            /* BAR sizing changes decode briefly; only inspect a candidate whose
             * base could cover this request, never touch unrelated BARs. */
            if (phys - base >= 0x100000000ULL) continue;
            uint64_t flags;
            kernel_spin_lock_irqsave(&pci_config_lock, &flags);
            uint16_t command = pci_raw_read32(d.bus,d.slot,d.function,4);
            pci_raw_write16(d.bus,d.slot,d.function,4,command & ~3u);
            pci_raw_write32(d.bus,d.slot,d.function,offset,UINT32_MAX);
            if (wide) pci_raw_write32(d.bus,d.slot,d.function,offset+4,UINT32_MAX);
            uint32_t mask_low = pci_raw_read32(d.bus,d.slot,d.function,offset);
            uint32_t mask_high = wide ? pci_raw_read32(d.bus,d.slot,d.function,offset+4) : UINT32_MAX;
            pci_raw_write32(d.bus,d.slot,d.function,offset,low);
            if (wide) pci_raw_write32(d.bus,d.slot,d.function,offset+4,high);
            pci_raw_write16(d.bus,d.slot,d.function,4,command);
            kernel_spin_unlock_irqrestore(&pci_config_lock, flags);
            uint64_t mask = ((uint64_t)mask_high << 32) | (mask_low & ~15u);
            uint64_t size = ~mask + 1;
            if (size && !(size & (size-1)) && base && !(base & (size-1)) &&
                phys >= base && phys-base < size && bytes <= size-(phys-base)) return true;
        }
    }
    return false;
}
#define PCI_MMIO_MAX 64U
struct pci_mmio_mapping { uint64_t phys, bytes, aligned, length; void *ptr; uint32_t refs; };
static struct pci_mmio_mapping pci_mappings[PCI_MMIO_MAX];
static struct kernel_spinlock pci_mapping_lock = KERNEL_SPINLOCK_INIT;
/** @brief Map only the requested memory BAR pages UC/NX in the kernel alias.
 * @param phys Page-aligned BAR base of a quiesced PCI function.
 * @param bytes Requested byte range, nonzero; covers only rounded 4 KiB pages.
 * @return Borrowed MMIO pointer with one owned mapping reference, or NULL.
 * Task context under execution transaction/pre-SMP; caller unmaps matching bytes.
 * BAR must occupy whole pages so rounding never changes adjacent RAM attributes.
 */
void *pci_map_mmio(uint64_t phys, uint64_t bytes)
{
    if ((phys & 4095) || !bytes || !paging_kernel_direct_map_range(phys,bytes) || !pci_bar_contains(phys,bytes)) return NULL;
    uint64_t aligned = phys & ~4095ULL, end = (phys + bytes + 4095) & ~4095ULL;
    if (end <= aligned || !pci_bar_contains(aligned,end-aligned)) return NULL;
    kernel_spin_lock(&pci_mapping_lock);
    struct pci_mmio_mapping *m = NULL;
    for (uint32_t i=0;i<PCI_MMIO_MAX;++i) if (pci_mappings[i].refs && pci_mappings[i].phys==phys && pci_mappings[i].bytes==bytes) { m=&pci_mappings[i]; break; }
    if (!m) for (uint32_t i=0;i<PCI_MMIO_MAX;++i) if (!pci_mappings[i].refs) { m=&pci_mappings[i]; break; }
    if (!m || m->refs == UINT32_MAX || !paging_acquire_mmio(aligned,end-aligned)) { kernel_spin_unlock(&pci_mapping_lock); return NULL; }
    if (!m->refs) *m=(struct pci_mmio_mapping){.phys=phys,.bytes=bytes,.aligned=aligned,.length=end-aligned,.ptr=paging_kernel_direct_map(phys)};
    ++m->refs; void *result=m->ptr; kernel_spin_unlock(&pci_mapping_lock); return result;
}
/** @brief Release exactly one matching PCI MMIO mapping reference.
 * @param ptr Owned pointer returned by map_mmio; invalid/stale pointers ignored.
 * @param bytes Original requested byte length, required to match the lease.
 * @return None. Task context under execution transaction; device stopped first.
 * Final page reference restores original attributes and synchronizes CPU TLBs.
 */
void pci_unmap_mmio(void *ptr, uint64_t bytes)
{
    kernel_spin_lock(&pci_mapping_lock);
    for (uint32_t i=0;i<PCI_MMIO_MAX;++i) if (pci_mappings[i].refs && pci_mappings[i].ptr==ptr && pci_mappings[i].bytes==bytes) {
        paging_release_mmio(pci_mappings[i].aligned,pci_mappings[i].length);
        --pci_mappings[i].refs; break;
    }
    kernel_spin_unlock(&pci_mapping_lock);
}
