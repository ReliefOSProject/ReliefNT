#include "storage_ide.h"
#include "../../../block/core/storage_core_api.h"
#include <reliefnt/port.h>

static void ide_io_delay(uint16_t control)
{
    if (control) {
        (void)x86_64_inb(control);
        (void)x86_64_inb(control);
        (void)x86_64_inb(control);
        (void)x86_64_inb(control);
    }
}

static uint8_t ide_status(const struct ide_device_info *device)
{
    return device ? x86_64_inb((uint16_t)(device->command_base + 7u)) : 0xffu;
}

static int ide_select(const struct ide_device_info *device)
{
    if (!device || !device->command_base || device->drive > 1u) {
        return -22;
    }
    x86_64_outb((uint8_t)(0xa0u | (device->drive << 4)),
                (uint16_t)(device->command_base + 6u));
    ide_io_delay(device->control_base);
    return 0;
}

static int ide_wait_ready(const struct ide_device_info *device, uint8_t want_drq)
{
    uint8_t status;
    if (!device) {
        return -22;
    }
    for (uint32_t spin = 0; spin < IDE_WAIT_SPINS; ++spin) {
        status = ide_status(device);
        if (status == 0xffu || status == 0x00u) {
            return -19;
        }
        if (status & IDE_STATUS_ERR) {
            return -5;
        }
        if (status & IDE_STATUS_DF) {
            return -5;
        }
        if (!(status & IDE_STATUS_BSY)) {
            if (want_drq) {
                if (status & IDE_STATUS_DRQ) {
                    return 0;
                }
            } else if (!(status & IDE_STATUS_DRQ)) {
                return 0;
            }
        }
        __asm__ volatile("pause");
    }
    return -110;
}

int ide_flush_cache(const struct ide_device_info *device)
{
    if (!device || !device->present || device->atapi) return -95;
    int ret = ide_select(device);
    if (!ret) ret = ide_wait_ready(device, 0);
    if (ret < 0) return ret;
    x86_64_outb(0xe7, (uint16_t)(device->command_base + 7u));
    ide_io_delay(device->control_base);
    return ide_wait_ready(device, 0);
}

/** @brief Reset a failed PIO channel while keeping its polling IRQ source masked.
 * @param device Borrowed stable endpoint with control port, required.
 * @return Zero, -EINVAL or -ETIMEDOUT. Caller pins code/endpoint and serializes
 * both channel devices; no DMA pages, IRQ callbacks or CPU buffer is retained.
 */
static int ide_soft_reset(const struct ide_device_info *device)
{
    uint8_t status;
    if (!device || !device->control_base) {
        return -22;
    }
    x86_64_outb(IDE_CMD_SRST|IDE_CONTROL_NIEN, device->control_base);
    for (uint32_t i = 0; i < 10000u; ++i) {
        __asm__ volatile("pause");
    }
    x86_64_outb(IDE_CONTROL_NIEN, device->control_base);
    for (uint32_t i = 0; i < IDE_WAIT_SPINS / 16u; ++i) {
        status = ide_status(device);
        if (!(status & IDE_STATUS_BSY)) {
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -110;
}

static void ide_copy_model(char out[41], const uint16_t *identify)
{
    uint32_t pos = 0;
    uint32_t start = 0;
    if (!out || !identify) {
        return;
    }
    for (uint32_t word = 27; word <= 46 && pos + 1u < 41u; ++word) {
        uint16_t value = identify[word];
        char hi = (char)(value >> 8);
        char lo = (char)(value & 0xffu);
        out[pos++] = hi ? hi : ' ';
        if (pos + 1u < 41u) {
            out[pos++] = lo ? lo : ' ';
        }
    }
    while (start < pos && out[start] == ' ') {
        ++start;
    }
    while (pos && out[pos - 1u] == ' ') {
        --pos;
    }
    if (start && start < pos) {
        uint32_t write = 0;
        while (start < pos) {
            out[write++] = out[start++];
        }
        pos = write;
    }
    out[pos] = 0;
}

/** @brief Mask polling IRQs and identify one actual ATA/ATAPI endpoint.
 * @param device Exclusive initialized endpoint, required; geometry/model are
 * updated only from complete IDENTIFY data. @return Zero or native errno.
 * Init/task under transport exclusion and code lifetime; unsupported non-512
 * ATA sectors fail explicitly. No filesystem dependency, allocation or DMA.
 */
int ide_identify_device(struct ide_device_info *device)
{
    uint16_t identify[256];
    uint8_t status;
    uint8_t lba_mid;
    uint8_t lba_high;
    if (!device || !device->command_base) {
        return -22;
    }
    if(device->control_base)x86_64_outb(IDE_CONTROL_NIEN,device->control_base);
    if (ide_select(device) < 0) {
        return -22;
    }
    status = ide_status(device);
    if (status == 0xffu || status == 0x00u) {
        return -19;
    }
    if (ide_wait_ready(device, 0) < 0) {
        return -19;
    }
    x86_64_outb(0, (uint16_t)(device->command_base + 2u));
    x86_64_outb(0, (uint16_t)(device->command_base + 3u));
    x86_64_outb(0, (uint16_t)(device->command_base + 4u));
    x86_64_outb(0, (uint16_t)(device->command_base + 5u));
    x86_64_outb(ATA_CMD_IDENTIFY_DEVICE, (uint16_t)(device->command_base + 7u));
    status = ide_status(device);
    if (status == 0x00u || status == 0xffu) {
        return -19;
    }
    for (uint32_t spin = 0; spin < IDE_WAIT_SPINS; ++spin) {
        status = ide_status(device);
        if (status & IDE_STATUS_ERR) {
            break;
        }
        if (!(status & IDE_STATUS_BSY) && (status & IDE_STATUS_DRQ)) {
            for (uint32_t i = 0; i < 256u; ++i) {
                identify[i] = x86_64_inw(device->command_base);
            }
            device->present = 1;
            device->atapi = 0;
            device->lba48 = (identify[83] & (1u << 10)) != 0;
            if ((identify[106] & (1u << 14)) && (identify[106] & (1u << 12))) {
                uint32_t logical_words = ((uint32_t)identify[118] << 16) | identify[117];
                if (logical_words && logical_words != (SECTOR_SIZE / 2u)) {
                    return -95;
                }
            }
            device->sector_count = device->lba48
                                       ? ((uint64_t)identify[103] << 48) |
                                             ((uint64_t)identify[102] << 32) |
                                             ((uint64_t)identify[101] << 16) | identify[100]
                                       : ((uint64_t)identify[61] << 16) | identify[60];
            if (!device->sector_count) {
                return -5;
            }
            ide_copy_model(device->model, identify);
            return 0;
        }
        __asm__ volatile("pause");
    }

    /* A packet device reports the ATAPI signature after the IDENTIFY probe. */
    lba_mid = x86_64_inb((uint16_t)(device->command_base + 4u));
    lba_high = x86_64_inb((uint16_t)(device->command_base + 5u));
    if (lba_mid != IDE_ATAPI_SIG_MID || lba_high != IDE_ATAPI_SIG_HIGH) {
        return -5;
    }
    if (ide_select(device) < 0) {
        return -22;
    }
    x86_64_outb(IDE_CMD_IDENTIFY_PACKET, (uint16_t)(device->command_base + 7u));
    if (ide_wait_ready(device, 1) < 0) {
        return -5;
    }
    for (uint32_t i = 0; i < 256u; ++i) {
        identify[i] = x86_64_inw(device->command_base);
    }
    device->present = 1;
    device->atapi = 1;
    device->lba48 = 0;
    device->sector_count = 0;
    ide_copy_model(device->model, identify);
    return 0;
}

/** @brief Transfer validated ATA sectors using correctly ordered task-file bytes.
 * @param device Stable identified endpoint, required. @param lba First sector
 * fitting selected LBA28/48 command. @param sectors Nonzero sectors<=128.
 * @param buffer Current readable/writable sectors*512 bytes, any CPU alignment.
 * @param write Nonzero selects write. @return Zero or bounds/native errno;
 * failed completion resets the channel with IRQs still masked. Caller pins
 * code/device and transport exclusion; repeated CPU-port transfers preserve
 * one DRQ admission per sector. CPU bytes are never DMA addresses.
 */
int ide_pio_transfer(const struct ide_device_info *device, uint64_t lba,
                            uint32_t sectors, void *buffer, uint8_t write)
{
    uint8_t *bytes = (uint8_t *)buffer;
    uint8_t use_lba48;
    if (!device || !device->present || device->atapi || !buffer || !sectors ||
        sectors > IDE_MAX_PIO_SECTORS || lba + sectors < lba ||
        (device->sector_count && lba + sectors > device->sector_count)) {
        return -22;
    }
    use_lba48 = device->lba48;
    uint64_t limit=use_lba48 ? (1ull<<48) : (1ull<<28);
    if(lba>=limit || sectors>limit-lba)return -22;
    if (use_lba48) {
        if (ide_select(device) < 0 || ide_wait_ready(device, 0) < 0) {
            return -5;
        }
        x86_64_outb(0, (uint16_t)(device->command_base + 2u));
        x86_64_outb((uint8_t)(lba >> 24), (uint16_t)(device->command_base + 3u));
        x86_64_outb((uint8_t)(lba >> 32), (uint16_t)(device->command_base + 4u));
        x86_64_outb((uint8_t)(lba >> 40), (uint16_t)(device->command_base + 5u));
        x86_64_outb((uint8_t)sectors, (uint16_t)(device->command_base + 2u));
        x86_64_outb((uint8_t)lba, (uint16_t)(device->command_base + 3u));
        x86_64_outb((uint8_t)(lba >> 8), (uint16_t)(device->command_base + 4u));
        x86_64_outb((uint8_t)(lba >> 16), (uint16_t)(device->command_base + 5u));
        x86_64_outb((uint8_t)(0xe0u | (device->drive << 4)),
                    (uint16_t)(device->command_base + 6u));
        x86_64_outb(write ? IDE_CMD_WRITE_PIO_EXT : IDE_CMD_READ_PIO_EXT,
                    (uint16_t)(device->command_base + 7u));
    } else {
        if (lba > 0x0fffffffu || ide_select(device) < 0 || ide_wait_ready(device, 0) < 0) {
            return -22;
        }
        x86_64_outb((uint8_t)sectors, (uint16_t)(device->command_base + 2u));
        x86_64_outb((uint8_t)lba, (uint16_t)(device->command_base + 3u));
        x86_64_outb((uint8_t)(lba >> 8), (uint16_t)(device->command_base + 4u));
        x86_64_outb((uint8_t)(lba >> 16), (uint16_t)(device->command_base + 5u));
        x86_64_outb((uint8_t)(0xe0u | (device->drive << 4) | ((lba >> 24) & 0x0fu)),
                    (uint16_t)(device->command_base + 6u));
        x86_64_outb(write ? IDE_CMD_WRITE_PIO : IDE_CMD_READ_PIO,
                    (uint16_t)(device->command_base + 7u));
    }
    for (uint32_t sector = 0; sector < sectors; ++sector) {
        int wait_ret = ide_wait_ready(device, 1);
        if (wait_ret < 0) {
            (void)ide_soft_reset(device);
            return wait_ret;
        }
        uint8_t *data=bytes+(size_t)sector*SECTOR_SIZE;
        /* Keep each DRQ phase at exactly one logical sector. REP batches the
         * CPU port words without exposing virtual addresses as DMA addresses
         * or incurring a separate hypervisor exit for every individual word. */
        if (write) x86_64_outsw(device->command_base,data,SECTOR_SIZE/2u);
        else x86_64_insw(device->command_base,data,SECTOR_SIZE/2u);
        ide_io_delay(device->control_base);
    }
    {
        int wait_ret = ide_wait_ready(device, 0);
        if (wait_ret < 0) {
            (void)ide_soft_reset(device);
        }
        return wait_ret;
    }
}

/** @brief Execute one standard ATAPI data-in packet using CPU PIO words.
 * @param device Stable identified ATAPI device, borrowed under transport lock.
 * @param packet Readable 12-byte padded CDB. @param bytes Nonzero even extent.
 * @param buffer Writable current kernel extent of bytes bytes.
 * @return Zero or protocol/native timeout errno; timeout resets the channel.
 * No DMA/old task pointer is involved; caller pins device/code through completion.
 */
static int ide_atapi_packet(const struct ide_device_info *device,const uint8_t packet[12],uint32_t bytes,void *buffer)
{
    uint8_t *dst=buffer;uint32_t bytes_left=bytes;
    if(!device || !device->present || !device->atapi || !packet || !buffer || !bytes || (bytes&1u))return -22;
    if (ide_select(device) < 0 || ide_wait_ready(device, 0) < 0) {
        return -5;
    }
    x86_64_outb(0, (uint16_t)(device->command_base + 1u));
    x86_64_outb((uint8_t)(ISO9660_BLOCK_SIZE & 0xffu), (uint16_t)(device->command_base + 4u));
    x86_64_outb((uint8_t)(ISO9660_BLOCK_SIZE >> 8), (uint16_t)(device->command_base + 5u));
    x86_64_outb(IDE_CMD_PACKET, (uint16_t)(device->command_base + 7u));
    if (ide_wait_ready(device, 1) < 0) {
        return -5;
    }
    for (uint32_t i = 0; i < 6u; ++i) {
        x86_64_outw((uint16_t)packet[i * 2u] | ((uint16_t)packet[i * 2u + 1u] << 8),
                    device->command_base);
    }
    for (uint32_t spin = 0; bytes_left && spin < IDE_WAIT_SPINS; ++spin) {
        uint8_t status = ide_status(device);
        if (status & (IDE_STATUS_ERR | IDE_STATUS_DF)) {
            return -5;
        }
        if (status & IDE_STATUS_BSY) {
            __asm__ volatile("pause");
            continue;
        }
        if (!(status & IDE_STATUS_DRQ)) {
            if (!bytes_left) {
                break;
            }
            return -5;
        }
        uint32_t phase = (uint32_t)x86_64_inb((uint16_t)(device->command_base + 4u)) |
                         ((uint32_t)x86_64_inb((uint16_t)(device->command_base + 5u)) << 8);
        if (!phase || (phase & 1u) || phase > bytes_left) {
            return -5;
        }
        for (uint32_t i = 0; i < phase / 2u; ++i) {
            uint16_t word = x86_64_inw(device->command_base);
            dst[i * 2u] = (uint8_t)word;
            dst[i * 2u + 1u] = (uint8_t)(word >> 8);
        }
        dst += phase;
        bytes_left -= phase;
    }
    if (bytes_left) {
        (void)ide_soft_reset(device);
        return -110;
    }
    return ide_wait_ready(device, 0);
}

/** @brief Read bounded 2048-byte blocks through standard ATAPI READ(10).
 * @param device Stable identified optical device. @param lba First logical block.
 * @param blocks Nonzero blocks up to IDE_MAX_ATAPI_BLOCKS. @param buffer Current
 * writable blocks*2048 bytes. @return Zero or -EINVAL/native I/O errno.
 * Caller owns transport exclusion and device/code lifetime; all PIO is CPU I/O.
 */
int ide_atapi_read_blocks(const struct ide_device_info *device,uint64_t lba,uint32_t blocks,void *buffer)
{
    if(!blocks || blocks>IDE_MAX_ATAPI_BLOCKS || lba>UINT32_MAX || blocks-1u>UINT32_MAX-lba)return -22;
    uint8_t packet[12]={0};packet[0]=SCSI_CMD_READ10;
    packet[2]=lba>>24;packet[3]=lba>>16;packet[4]=lba>>8;packet[5]=lba;
    packet[7]=blocks>>8;packet[8]=blocks;
    return ide_atapi_packet(device,packet,blocks*2048u,buffer);
}

/** @brief Query READ CAPACITY(10) without relying on any filesystem parser.
 * @param device Stable identified optical device. @param response Current
 * writable eight-byte big-endian last-LBA/block-size output, valid on success.
 * @return Zero or native I/O errno. Caller owns transport exclusion and exact
 * code/device reference; packet PIO never treats a CPU address as physical DMA.
 */
int ide_read_capacity(const struct ide_device_info *device,uint8_t response[8])
{
    uint8_t packet[12]={0x25};return ide_atapi_packet(device,packet,8,response);
}

void ide_fill_device(struct ide_device_info *device, uint8_t channel,
                            uint8_t drive, uint16_t command, uint16_t control)
{
    storage_memzero(device, sizeof(*device));
    device->channel = channel;
    device->drive = drive;
    device->command_base = command;
    device->control_base = control;
}
