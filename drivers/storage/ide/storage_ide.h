/* IDE driver-private device description.  Only the IDE driver and the legacy
 * storage probe paths that fill or inspect these fields may include this. */
#ifndef RELIEFOS_STORAGE_IDE_H
#define RELIEFOS_STORAGE_IDE_H

#include <stdint.h>
#include "hardware.h"

struct ide_device_info {
    uint8_t present;
    uint8_t atapi;
    uint8_t lba48;
    uint8_t channel;
    uint8_t drive;
    uint16_t command_base;
    uint16_t control_base;
    uint64_t sector_count;
    char model[41];
};


/** @brief Fill a fresh CPU-PIO endpoint identity without touching hardware.
 * @param device Required writable context. @param channel Primary/secondary.
 * @param drive Master/slave. @param command Command base. @param control Control
 * port. Caller owns context and serialization; no allocation or publication.
 */
void ide_fill_device(struct ide_device_info *device,uint8_t channel,uint8_t drive,uint16_t command,uint16_t control);
/** @brief Identify an ATA/ATAPI endpoint and snapshot actual geometry/model.
 * @param device Required exclusive CPU-PIO context; updated on success.
 * @return Zero or native error, no DMA. Init/task under transport exclusion;
 * interrupt source masked before publication, caller retains code/context.
 */
int ide_identify_device(struct ide_device_info *device);
/** @brief Transfer bounded 512-byte ATA sectors through CPU port I/O.
 * @param device Borrowed identified context. @param lba First sector.
 * @param sectors Nonzero sectors <=IDE_MAX_PIO_SECTORS. @param buffer Current
 * readable/writable sectors*512 bytes. @param write Nonzero for device write.
 * @return Zero or range/native errno. Caller owns device/code/transport exclusion;
 * no registry lock spans polling, timeout resets without freeing live context.
 */
int ide_pio_transfer(const struct ide_device_info *device,uint64_t lba,uint32_t sectors,void *buffer,uint8_t write);
/** @brief Read standard optical logical blocks through synchronous ATAPI PIO.
 * @param device Borrowed optical context. @param lba First 32-bit block.
 * @param blocks Nonzero blocks <=IDE_MAX_ATAPI_BLOCKS. @param buffer Current
 * writable blocks*2048 bytes. @return Zero or native errno. Caller owns code,
 * endpoint and transport exclusion; never treats caller bytes as DMA addresses.
 */
int ide_atapi_read_blocks(const struct ide_device_info *device,uint64_t lba,uint32_t blocks,void *buffer);
/** @brief Flush a writable ATA endpoint's volatile cache before final sync returns.
 * @param device Borrowed identified ATA context, required.
 * @return Zero or native errno. Caller pins code/endpoint/transport exclusion;
 * synchronous PIO polling, no registry callback lock, allocation or DMA.
 */
int ide_flush_cache(const struct ide_device_info *device);

/** @brief Read a standard eight-byte READ CAPACITY(10) wire response.
 * @param device Stable optical device. @param response Writable eight bytes.
 * @return Zero or native errno. Task context with transport exclusion and
 * exact code/device lifetime held; synchronous CPU PIO, no DMA ownership.
 */
int ide_read_capacity(const struct ide_device_info *device,uint8_t response[8]);
#endif /* RELIEFOS_STORAGE_IDE_H */
