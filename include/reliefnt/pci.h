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
                    struct pci_device *out);
/**
 * @brief Scan PCI for the first device matching vendor_id/device_id; 0 if found.
 */
int pci_find_device(uint16_t vendor_id, uint16_t device_id,
                    struct pci_device *out);

/** @brief Check a range against one exact device BAR without scanning other BARs.
 * @param device Exact device identity selected by the registered driver.
 * @param bar Zero-based BAR index in the function's header.
 * @param phys Physical start of the range, including page-rounded coverage.
 * @param bytes Nonzero length; the complete page-aligned range must fit the
 * probed BAR size.
 * @return True only for that function's implemented memory BAR. Task context
 * under execution ownership after the device has drained; the probe disables
 * PCI decode only while serialized config access is held.
 */
bool pci_device_bar_contains(const struct pci_device *device, uint8_t bar,
                             uint64_t phys, uint64_t bytes);

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
 * Final low reference restores attributes; the final high reference retires its
 * virtual mapping and table pages after all CPU TLBs have been synchronized.
 */
void pci_unmap_mmio(void *ptr, uint64_t bytes);

/** @brief Map an exclusively controlled PCI BAR from its aligned base UC/RW/NX.
 * @param phys Page-aligned memory BAR base.
 * @param bytes Requested bytes; covering pages must lie entirely within BAR.
 * @return Owned mapping lease or NULL; task context under execution ownership.
 * High physical BARs use the reserved inherited device virtual window; repeated
 * exact claims share a pointer. Caller drains device/users before exact unmap.
 */
void *pci_map_mmio(uint64_t phys, uint64_t bytes);
#endif
