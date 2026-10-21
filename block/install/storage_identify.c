#include "../core/storage_core_api.h"
#include "../core/storage_driver.h"
#include "../../drivers/storage/nvme/storage_nvme.h"
#include "../../drivers/storage/ide/storage_ide.h"

int storage_install_identify(struct install_disk_state *disk, uint64_t *out_sectors)
{
    uint16_t *id = (uint16_t *)storage_scratch;
    int ret;
    if (!disk || !out_sectors) {
        return -22;
    }
    if (disk->transport == STORAGE_TRANSPORT_IDE_PIO) {
        struct ide_device_info device;
        storage_disk_ide_device(disk, &device);
        kernel_spin_lock(&storage_transport_lock);
        ret = ide_identify_device(&device);
        if (ret < 0 || device.atapi || !device.sector_count) {
            kernel_spin_unlock(&storage_transport_lock);
            return ret < 0 ? ret : -95;
        }
        disk->ide_lba48 = device.lba48;
        disk->ide_atapi = device.atapi;
        disk->ide_command_base = device.command_base;
        disk->ide_control_base = device.control_base;
        disk->sector_count = device.sector_count;
        storage_copy_text(disk->device_model, sizeof(disk->device_model), device.model);
        *out_sectors = device.sector_count;
        kernel_spin_unlock(&storage_transport_lock);
        return 0;
    }
    if (disk->transport == STORAGE_TRANSPORT_NVME) {
        if (!disk->nvme || !disk->nvme->ready || !disk->nvme_nsid || !disk->sector_count) {
            return -95;
        }
        *out_sectors = disk->sector_count;
        return 0;
    }
    if (disk->transport != STORAGE_TRANSPORT_AHCI || !disk->hba_port) {
        return -22;
    }
    kernel_spin_lock(&storage_transport_lock);
    ret = ahci_wait_idle(disk->hba_port);
    if (ret < 0) {
        ret = storage_read_failure(ret);
        goto out_unlock;
    }
    disk->hba_port->is = 0xffffffffu;
    storage_memzero(ahci_cmd_headers, sizeof(ahci_cmd_headers));
    storage_memzero(ahci_received_fis, sizeof(ahci_received_fis));
    storage_memzero(ahci_cmd_table_buf, sizeof(ahci_cmd_table_buf));
    ahci_cmd_headers[0].flags = (uint16_t)((sizeof(struct fis_reg_h2d) / sizeof(uint32_t)) & 0x1f);
    ahci_cmd_headers[0].prdtl = 1;
    ahci_cmd_headers[0].ctba = (uint32_t)(uintptr_t)ahci_cmd_table_buf;
    {
        struct ahci_cmd_table *tbl = (struct ahci_cmd_table *)(void *)ahci_cmd_table_buf;
        struct fis_reg_h2d *fis = (struct fis_reg_h2d *)(void *)tbl->cfis;
        storage_memzero(tbl, sizeof(ahci_cmd_table_buf));
        tbl->prdt[0].dba = (uint32_t)(uintptr_t)storage_scratch;
        tbl->prdt[0].dbau = 0;
        tbl->prdt[0].dbc = SECTOR_SIZE - 1u;
        fis->fis_type = FIS_TYPE_REG_H2D;
        fis->c = 1;
        fis->command = ATA_CMD_IDENTIFY_DEVICE;
        fis->device = 0;
        fis->countl = 1;
        ret = ahci_wait_cmd_slot(disk->hba_port);
        if (ret < 0) {
            ret = storage_read_failure(ret);
            goto out_unlock;
        }
        ahci_memory_barrier();
        disk->hba_port->ci = 1u;
        for (uint32_t i = 0; i < AHCI_WAIT_SPINS; ++i) {
            if ((disk->hba_port->ci & 1u) == 0) {
                if (disk->hba_port->is & AHCI_PORT_IS_TFES) {
                    ret = -5;
                    goto out_unlock;
                }
                *out_sectors = ((uint64_t)id[103] << 48) |
                               ((uint64_t)id[102] << 32) |
                               ((uint64_t)id[101] << 16) |
                               id[100];
                if (*out_sectors == 0) {
                    *out_sectors = ((uint64_t)id[61] << 16) | id[60];
                }
                ret = *out_sectors ? 0 : -5;
                goto out_unlock;
            }
            ahci_cpu_relax();
        }
    }
    ret = -5;
out_unlock:
    kernel_spin_unlock(&storage_transport_lock);
    return ret;
}
