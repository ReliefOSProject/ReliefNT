/*
 * ReliefOS PCI interface: declares architecture-neutral PCI configuration APIs.
 * Used by device discovery and PCI-backed kernel drivers.
 */
#ifndef RELIEFNT_PCI_H
#define RELIEFNT_PCI_H

#include <reliefnt/types.h>
#include <reliefos/driver.h>

struct pci_device {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
};

/** @brief Serialize the CF8/CFC transaction across IRQs and CPUs.
 * @param bus PCI bus.
 * @param slot PCI slot.
 * @param function PCI function.
 * @param offset Config offset.
 * @return Register value. IRQ-safe config lock; no allocation/ownership.
 */
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function,
                           uint8_t offset);
/** @brief Write an atomic CF8/CFC transaction.
 * @param bus PCI bus.
 * @param slot PCI slot.
 * @param function PCI function.
 * @param offset Config offset.
 * @param value New word.
 * @return None. IRQ-safe config lock, no ownership transfer.
 */
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function,
                        uint8_t offset, uint32_t value);
/** @brief Read a halfword through serialized config access.
 * @param bus PCI bus.
 * @param slot PCI slot.
 * @param function PCI function.
 * @param offset Even offset.
 * @return Halfword. IRQ safe, no ownership.
 */
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t function,
                           uint8_t offset);
/** @brief Write exactly one halfword, preserving adjacent status W1C bits.
 * @param bus PCI bus.
 * @param slot PCI slot.
 * @param function PCI function.
 * @param offset Even offset.
 * @param value New halfword.
 * @return None. IRQ-safe config lock, no ownership transfer.
 */
void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t function,
                        uint8_t offset, uint16_t value);
/**
 * @brief Fill out with the vendor/device/class identity at bus:slot:function; 0 if present.
 */
int pci_read_device(uint8_t bus, uint8_t slot, uint8_t function,
                    struct pci_device *out);
/**
 * @brief Scan PCI for the first device matching vendor_id/device_id; 0 if found.
 */
int pci_find_device(uint16_t vendor_id, uint16_t device_id,
                    struct pci_device *out);

/** @brief Enumerate every present PCI function by stable bus/slot/function index.
 * @param index Zero-based ordinal among present functions.
 * @param out Receives complete module-facing identity; borrowed output.
 * @return 0, -EINVAL or -ENODEV. Task context, bounded configuration scan.
 */
int pci_enumerate(uint32_t index, struct reliefos_driver_pci_device *out);

/** @brief Release exactly one matching PCI MMIO mapping reference.
 * @param ptr Owned pointer returned by map_mmio; invalid/stale pointers ignored.
 * @param bytes Original requested byte length, required to match the lease.
 * @return None. Task context under execution transaction; device stopped first.
 * Final page reference restores original attributes and synchronizes CPU TLBs.
 */
void pci_unmap_mmio(void *ptr, uint64_t bytes);

/** @brief Map a quiesced PCI memory BAR from its page-aligned base UC/NX.
 * @param phys Page-aligned memory BAR base.
 * @param bytes Requested bytes; covering pages must lie entirely within BAR.
 * @return Owned mapping lease or NULL; task context under execution transaction.
 */
void *pci_map_mmio(uint64_t phys, uint64_t bytes);
#endif
