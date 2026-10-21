/* IDE driver-private device description.  Only the IDE driver and the legacy
 * storage probe paths that fill or inspect these fields may include this. */
#ifndef RELIEFOS_STORAGE_IDE_H
#define RELIEFOS_STORAGE_IDE_H

#include <stdint.h>
#include "../../../block/core/storage_driver.h"

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


#endif /* RELIEFOS_STORAGE_IDE_H */
