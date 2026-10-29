#include <reliefnt/export.h>
/*
 * ReliefOS x86_64 PCI access: enumerates and configures PCI devices.
 * Provides low-level configuration-space reads and writes for drivers.
 */
#include <reliefnt/pci.h>
#include <reliefnt/console.h>
#include <reliefnt/lock.h>
#include <reliefnt/paging.h>

#include "../port.h"

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
 * @brief Probe one PCI function and optionally copy its scalar identification fields.
 * @param bus PCI bus number.
 * @param slot PCI device slot.
 * @param function PCI function number.
 * @param out Optional writable scalar identity; NULL only tests presence.
 * @return Zero for a present function or internal -1 when the vendor ID is absent.
 * @par Context
 * Each config transaction uses the IRQ-safe PCI config lock. No device binding, allocation, retained pointer or provider ownership is created.
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

/* PCI enumeration cache. A full config-space probe walks 256*32*8 addresses and
 * costs one CF8/CFC transaction each; under hardware virtualization every one of
 * those is a VM exit. Drivers used to pay that scan once per pci_find_device call
 * (e1000 alone issues several per init). The cache records the present functions
 * once in bus/slot/function order and serves every lookup from the snapshot.
 */
struct pci_cache_entry {
    struct pci_device dev;
    uint8_t header_type;
    uint8_t enum_visible;
};

#define PCI_CACHE_MAX 256u

static struct pci_cache_entry pci_cache[PCI_CACHE_MAX];
static uint32_t pci_cache_count;
static int pci_cache_overflow;
static volatile int pci_cache_ready;
static struct kernel_spinlock pci_enum_lock = KERNEL_SPINLOCK_INIT;

/** @brief Snapshot every present function in stable bus/slot/function order.
 * @return None. Caller holds pci_enum_lock; per-read config lock taken inside.
 */
static void pci_scan_fill(void)
{
    pci_cache_count = 0;
    pci_cache_overflow = 0;
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            uint8_t multifunction = 0;
            for (uint8_t function = 0; function < 8; ++function) {
                struct pci_device dev;
                if (pci_read_device((uint8_t)bus, slot, function, &dev) < 0) {
                    if (function == 0) {
                        break;
                    }
                    continue;
                }
                uint8_t header = (uint8_t)((pci_config_read32((uint8_t)bus, slot,
                                            function, 0x0c) >> 16) & 0xffu);
                if (function == 0) {
                    multifunction = (uint8_t)(header & 0x80u);
                }
                if (pci_cache_count >= PCI_CACHE_MAX) {
                    pci_cache_overflow = 1;
                    continue;
                }
                struct pci_cache_entry *entry = &pci_cache[pci_cache_count++];
                entry->dev = dev;
                entry->header_type = header;
                /* pci_enumerate's historical walk breaks past function 0 of a
                 * non-multifunction device; mirror that so only function 0 of
                 * such a slot is visible to enumeration. */
                entry->enum_visible = (function == 0 || multifunction) ? 1u : 0u;
            }
        }
    }
}

/** @brief Build the enumeration snapshot on first use.
 * @return None. Double-checked under a lock disjoint from the non-recursive
 * config lock, so the fill can take the config lock per read without deadlock.
 */
static void pci_cache_ensure(void)
{
    uint64_t flags;
    bool scanned = false;
    if (pci_cache_ready) {
        return;
    }
    kernel_spin_lock_irqsave(&pci_enum_lock, &flags);
    if (!pci_cache_ready) {
        pci_scan_fill();
        pci_cache_ready = 1;
        scanned = true;
    }
    kernel_spin_unlock_irqrestore(&pci_enum_lock, flags);
    if (scanned) {
        console_info("[pci] inventory cached_functions=%u capacity=%u\n",
                     pci_cache_count, PCI_CACHE_MAX);
        if (pci_cache_overflow) {
            console_warn("[pci] inventory cache full; lookups use exhaustive scan\n");
        }
    }
}

/** @brief Brute-force first-match scan, used only when the snapshot overflowed.
 * @param vendor_id Vendor half of the device identity.
 * @param device_id Device half of the device identity.
 * @param out Receives the first match in bus/slot/function order.
 * @return 0 on match, -1 when absent.
 */
static int pci_live_find(uint16_t vendor_id, uint16_t device_id,
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

/** @brief Brute-force ordinal lookup, used only when the snapshot overflowed.
 * @param index Zero-based ordinal among enumerate-visible functions.
 * @param out Receives the index-th module-facing identity.
 * @return 0, -EINVAL or -ENODEV, matching pci_enumerate.
 */
static int pci_live_nth(uint32_t index, struct reliefos_driver_pci_device *out)
{
    for (uint32_t b = 0; b < 256; ++b) for (uint32_t s = 0; s < 32; ++s)
        for (uint32_t f = 0; f < 8; ++f) {
            struct pci_device d;
            if (pci_read_device(b, s, f, &d) < 0) { if (!f) break; continue; }
            uint8_t header = (pci_config_read32(b, s, f, 0x0c) >> 16) & 0xff;
            if (!index--) {
                *out = (struct reliefos_driver_pci_device){.bus=b,.slot=s,.function=f,
                    .class_code=d.class_code,.vendor_id=d.vendor_id,.device_id=d.device_id,
                    .subclass=d.subclass,.prog_if=d.prog_if,.header_type=header}; return 0;
            }
            if (!f && !(header & 0x80)) break;
        }
    return -19;
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
    pci_cache_ensure();
    if (pci_cache_overflow) {
        return pci_live_find(vendor_id, device_id, out);
    }
    for (uint32_t i = 0; i < pci_cache_count; ++i) {
        struct pci_device *dev = &pci_cache[i].dev;
        if (dev->vendor_id == vendor_id && dev->device_id == device_id) {
            if (out) {
                *out = *dev;
            }
            return 0;
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
    pci_cache_ensure();
    if (pci_cache_overflow) {
        return pci_live_nth(index, out);
    }
    for (uint32_t i = 0; i < pci_cache_count; ++i) {
        struct pci_cache_entry *entry = &pci_cache[i];
        if (!entry->enum_visible) {
            continue;
        }
        if (!index--) {
            struct pci_device *d = &entry->dev;
            *out = (struct reliefos_driver_pci_device){.bus=d->bus,.slot=d->slot,
                .function=d->function,.class_code=d->class_code,
                .vendor_id=d->vendor_id,.device_id=d->device_id,
                .subclass=d->subclass,.prog_if=d->prog_if,
                .header_type=entry->header_type}; return 0;
        }
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

/** @brief Validate a page range against one exact function BAR only.
 * @param device Exact PCI identity selected by the driver.
 * @param bar Zero-based memory BAR index.
 * @param phys Page-aligned physical start of the requested aperture.
 * @param bytes Nonzero page-aligned byte length.
 * @return True when that implemented memory BAR contains the complete range.
 * Task context after the selected device is drained. The command decode and BAR are restored while
 * the serialized PCI configuration lock is still held, including on rejects.
 */
bool pci_device_bar_contains(const struct pci_device *device, uint8_t bar,
                             uint64_t phys, uint64_t bytes)
{
    if (!device || !bytes || (phys & 4095u) || (bytes & 4095u) ||
        phys > UINT64_MAX - bytes)
        return false;

    uint64_t flags;
    kernel_spin_lock_irqsave(&pci_config_lock, &flags);
    uint32_t identity = pci_raw_read32(device->bus, device->slot, device->function, 0x00);
    uint32_t header = pci_raw_read32(device->bus, device->slot, device->function, 0x0c);
    uint32_t header_type = (header >> 16) & 0x7fu;
    uint32_t bar_count = header_type == 0 ? 6u : header_type == 1 ? 2u : 0u;
    bool valid = (uint16_t)identity == device->vendor_id &&
                 (uint16_t)(identity >> 16) == device->device_id &&
                 bar < bar_count;
    uint32_t layout = 0;
    while (valid && layout < bar) {
        uint32_t previous = pci_raw_read32(device->bus, device->slot,
            device->function, (uint8_t)(0x10u + layout * 4u));
        if (previous == UINT32_MAX) {
            valid = false;
        } else if (previous && !(previous & 1u)) {
            uint32_t previous_type = (previous >> 1) & 3u;
            if (previous_type == 2u) {
                if (layout + 1u >= bar_count) valid = false;
                layout += 2u;
            } else if (previous_type == 0u) {
                ++layout;
            } else {
                valid = false;
            }
        } else {
            ++layout;
        }
    }
    if (layout != bar) valid = false; /* Selected index is a BAR's high dword. */
    uint32_t bar_offset = 0x10u + (uint32_t)bar * 4u;
    uint32_t low = valid ? pci_raw_read32(device->bus, device->slot,
                                           device->function, (uint8_t)bar_offset)
                         : 0;
    valid = valid && low != 0 && low != UINT32_MAX &&
                 !(low & 1u);
    uint32_t type = (low >> 1) & 3u;
    bool wide = type == 2u;
    if (type != 0u && !wide) valid = false;
    if (wide && bar + 1 >= bar_count) valid = false;
    uint32_t high = wide && valid ? pci_raw_read32(device->bus, device->slot,
                                          device->function, (uint8_t)(bar_offset+4)) : 0;
    uint64_t base = ((uint64_t)high << 32) | (low & ~15u);
    if (wide && high == UINT32_MAX) valid = false;

    uint64_t size = 0;
    if (valid && base) {
        uint16_t command = (uint16_t)pci_raw_read32(
            device->bus, device->slot, device->function, 0x04);
        pci_raw_write16(device->bus, device->slot, device->function, 0x04,
                        command & (uint16_t)~3u);
        pci_raw_write32(device->bus, device->slot, device->function,
                        (uint8_t)bar_offset,
                        UINT32_MAX);
        if (wide)
            pci_raw_write32(device->bus, device->slot, device->function,
                            (uint8_t)(bar_offset+4),
                            UINT32_MAX);
        uint32_t mask_low = pci_raw_read32(device->bus, device->slot,
                                           device->function, (uint8_t)bar_offset);
        uint32_t mask_high = wide ? pci_raw_read32(device->bus, device->slot,
                                                   device->function,
                                                   (uint8_t)(bar_offset+4))
                                  : UINT32_MAX;
        pci_raw_write32(device->bus, device->slot, device->function,
                        (uint8_t)bar_offset, low);
        if (wide)
            pci_raw_write32(device->bus, device->slot, device->function,
                            (uint8_t)(bar_offset+4),
                            high);
        pci_raw_write16(device->bus, device->slot, device->function, 0x04, command);

        if (wide) {
            uint64_t mask = ((uint64_t)mask_high << 32) | (mask_low & ~15u);
            size = ~mask + 1;
        } else {
            uint32_t mask = mask_low & ~15u;
            size = (uint32_t)(~mask + 1u);
        }
        if (!size || (size & (size - 1)) || (base & (size - 1)) ||
            size > UINT64_MAX - base)
            valid = false;
    } else {
        valid = false;
    }
    kernel_spin_unlock_irqrestore(&pci_config_lock, flags);

    if (!valid) return false;
    uint64_t bar_end = base + size;
    if (phys < base || phys > bar_end || bytes > bar_end - phys) return false;
    return true;
}

#define PCI_MMIO_MAX 64U
struct pci_mmio_mapping { uint64_t phys, bytes, aligned, length; void *ptr; uint32_t refs; bool high; };
static struct pci_mmio_mapping pci_mappings[PCI_MMIO_MAX];
static struct kernel_spinlock pci_mapping_lock = KERNEL_SPINLOCK_INIT;
/** @brief Retain already-admitted MMIO pages UC/RW/NX without touching PCI config.
 * @param phys Page-aligned BAR base of an exclusively controlled PCI function.
 * @param bytes Nonzero requested bytes; covering pages must fit the actual BAR.
 * @return Owned mapping lease or NULL. Task context under execution ownership;
 * repeated exact claims share a pointer/reference, high BARs use the inherited
 * device virtual window, and low BARs keep precise direct-map attribute leases.
 */
static void *pci_map_mmio_lease(uint64_t phys, uint64_t bytes)
{
    if ((phys & 4095u) || !bytes || bytes > UINT64_MAX - 4095u) return NULL;
    uint64_t length = (bytes + 4095u) & ~4095ull;
    if (phys > UINT64_MAX - length) return NULL;
    bool high = !paging_kernel_direct_map_range(phys, length);
    /* A high UC alias must never overlap a still-WB part of the RAM direct map. */
    if (high && phys < RELIEFNT_KERNEL_DIRECT_MAP_SIZE) return NULL;
    kernel_spin_lock(&pci_mapping_lock);
    struct pci_mmio_mapping *mapping = NULL;
    for (uint32_t i = 0; i < PCI_MMIO_MAX; ++i)
        if (pci_mappings[i].refs && pci_mappings[i].phys == phys &&
            pci_mappings[i].bytes == bytes) { mapping = &pci_mappings[i]; break; }
    if (!mapping) for (uint32_t i = 0; i < PCI_MMIO_MAX; ++i)
        if (!pci_mappings[i].refs) { mapping = &pci_mappings[i]; break; }
    if (!mapping || mapping->refs == UINT32_MAX) {
        kernel_spin_unlock(&pci_mapping_lock); return NULL;
    }
    if (!mapping->refs) {
        void *pointer;
        if (high) pointer = paging_ioremap_uc(phys, length);
        else pointer = paging_acquire_mmio(phys, length) ? paging_kernel_direct_map(phys) : NULL;
        if (!pointer) { kernel_spin_unlock(&pci_mapping_lock); return NULL; }
        *mapping = (struct pci_mmio_mapping){.phys = phys, .bytes = bytes,
            .aligned = phys, .length = length, .ptr = pointer, .high = high};
    } else if (!mapping->high && !paging_acquire_mmio(mapping->aligned, mapping->length)) {
        kernel_spin_unlock(&pci_mapping_lock); return NULL;
    }
    ++mapping->refs;
    void *result = mapping->ptr;
    kernel_spin_unlock(&pci_mapping_lock);
    return result;
}
/** @brief Size a drained device's aperture, then map only fully covered pages. */
void *pci_map_mmio(uint64_t phys, uint64_t bytes)
{
    if ((phys & 4095u) || !bytes || bytes > UINT64_MAX-4095u) return NULL;
    uint64_t length=(bytes+4095u)&~4095ull;
    if (phys>UINT64_MAX-length || !pci_bar_contains(phys,bytes) ||
        !pci_bar_contains(phys,length)) return NULL;
    return pci_map_mmio_lease(phys,bytes);
}

/** @brief Read firmware's assigned BAR0 identity without writes or sizing probes.
 * Only capability-header reads are admitted before handoff. A normal memory
 * BAR implements at least16 bytes; the containing page is a mapping granule,
 * not evidence that all4096 bytes are implemented. After handoff, the caller
 * may use its architected STOP registers, then must size the drained aperture
 * before any larger access or DMA start.
 */
void *pci_map_firmware_bar0(const struct pci_device *device,uint64_t phys)
{
    if (!device || !phys || (phys&4095u) || phys>UINT64_MAX-4096u) return NULL;
    uint64_t flags;kernel_spin_lock_irqsave(&pci_config_lock,&flags);
    uint32_t identity=pci_raw_read32(device->bus,device->slot,device->function,0);
    uint32_t header=pci_raw_read32(device->bus,device->slot,device->function,0x0c);
    uint32_t low=pci_raw_read32(device->bus,device->slot,device->function,0x10);
    uint32_t type=(low>>1)&3u;
    uint32_t high=type==2u ? pci_raw_read32(device->bus,device->slot,device->function,0x14) : 0;
    bool valid=(uint16_t)identity==device->vendor_id && (uint16_t)(identity>>16)==device->device_id &&
        device->vendor_id!=0xffffu && ((header>>16)&0x7fu)<=1u && low && low!=UINT32_MAX &&
        !(low&1u) && (type==0u || type==2u) && high!=UINT32_MAX &&
        (((uint64_t)high<<32)|(low&~15u))==phys;
    kernel_spin_unlock_irqrestore(&pci_config_lock,flags);
    return valid ? pci_map_mmio_lease(phys,4096) : NULL;
}
/** @brief Release one exact PCI MMIO lease after the device/users have stopped.
 * @param ptr Owned original pointer; wrong/stale pointers are ignored.
 * @param bytes Original requested byte count, must match the lease.
 * Task context under execution ownership. Low pages restore their attributes;
 * the final high lease retires all CPU translations before freeing paging tables.
 */
void pci_unmap_mmio(void *ptr, uint64_t bytes)
{
    kernel_spin_lock(&pci_mapping_lock);
    for (uint32_t i = 0; i < PCI_MMIO_MAX; ++i) {
        struct pci_mmio_mapping *mapping = &pci_mappings[i];
        if (!mapping->refs || mapping->ptr != ptr || mapping->bytes != bytes) continue;
        if (!mapping->high) paging_release_mmio(mapping->aligned, mapping->length);
        else if (mapping->refs == 1 && !paging_iounmap(mapping->ptr, mapping->length)) {
            kernel_spin_unlock(&pci_mapping_lock); return; /* Retain an inconsistent lease. */
        }
        if (!--mapping->refs) *mapping = (struct pci_mmio_mapping){0};
        break;
    }
    kernel_spin_unlock(&pci_mapping_lock);
}

EXPORT_SYMBOL(pci_config_read16);

EXPORT_SYMBOL(pci_config_read32);

EXPORT_SYMBOL(pci_config_write16);
EXPORT_SYMBOL(pci_config_write32);

EXPORT_SYMBOL(pci_device_bar_contains);

EXPORT_SYMBOL(pci_map_mmio);
EXPORT_SYMBOL(pci_map_firmware_bar0);

EXPORT_SYMBOL(pci_read_device);

EXPORT_SYMBOL(pci_unmap_mmio);
