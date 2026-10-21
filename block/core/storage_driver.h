/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_STORAGE_DRIVER_H
#define RELIEFNT_STORAGE_DRIVER_H

#include <reliefnt/console.h>

#include "storage_types.h"

/**
 * @file storage_driver.h
 * @brief Private transport interface used by storage controller drivers.
 *
 * Driver code includes this contract directly so it does not depend on VFS,
 * filesystem-family prototypes, partition editing, or installer interfaces.
 */

struct ide_device_info;
struct nvme_controller;

/* Controller driver interface (drivers/storage/). */
int ahci_flush_cache(struct ahci_hba_port *port); /* drivers/storage/ahci/storage_ahci.c */
int ahci_read_atapi_blocks_retry(struct ahci_hba_port *port, uint64_t lba, uint32_t block_count, void *buffer); /* drivers/storage/ahci/storage_ahci.c */
int ahci_read_lba_retry(struct ahci_hba_port *port, uint64_t lba, uint32_t sector_count, void *buffer); /* drivers/storage/ahci/storage_ahci.c */
int ahci_setup_port(struct ahci_hba_port *port); /* drivers/storage/ahci/storage_ahci.c */
int ahci_wait_cmd_slot(struct ahci_hba_port *port); /* drivers/storage/ahci/storage_ahci.c */
int ahci_wait_idle(struct ahci_hba_port *port); /* drivers/storage/ahci/storage_ahci.c */
int ahci_write_lba_retry(struct ahci_hba_port *port, uint64_t lba, uint32_t sector_count, const void *buffer); /* drivers/storage/ahci/storage_ahci.c */
int ide_atapi_read_blocks(const struct ide_device_info *device, uint64_t lba, uint32_t blocks, void *buffer); /* drivers/storage/ide/storage_ide.c */
int ide_flush_cache(const struct ide_device_info *device); /* drivers/storage/ide/storage_ide.c */
int ide_identify_device(struct ide_device_info *device); /* drivers/storage/ide/storage_ide.c */
int ide_pio_transfer(const struct ide_device_info *device, uint64_t lba, uint32_t sectors, void *buffer, uint8_t write); /* drivers/storage/ide/storage_ide.c */
int nvme_flush_cache(struct nvme_controller *controller, uint32_t nsid); /* drivers/storage/nvme/storage_nvme.c */
int nvme_readwrite(struct nvme_controller *controller, uint32_t nsid, uint64_t lba, uint32_t sector_count, void *buffer, uint8_t write); /* drivers/storage/nvme/storage_nvme.c */
int storage_read_failure(int ret); /* drivers/storage/ahci/storage_ahci.c */
uint32_t fat_offset_for_cluster(uint32_t cluster); /* drivers/storage/ide/storage_ide.c */
void ahci_cpu_relax(void); /* drivers/storage/ahci/storage_ahci.c */
void ahci_memory_barrier(void); /* drivers/storage/ahci/storage_ahci.c */
void ahci_pending_clear(void); /* drivers/storage/ahci/storage_ahci.c */
void ide_fill_device(struct ide_device_info *device, uint8_t channel, uint8_t drive, uint16_t command, uint16_t control); /* drivers/storage/ide/storage_ide.c */
void nvme_reset_all_controllers(void); /* drivers/storage/nvme/storage_nvme.c */
void storage_scan_nvme_controller(uint8_t bus, uint8_t slot, uint8_t function, uint8_t *root_ready); /* drivers/storage/nvme/storage_nvme.c */
#endif /* RELIEFNT_STORAGE_DRIVER_H */
