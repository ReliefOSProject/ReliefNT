#include <reliefnt/console.h>
/* SPDX-License-Identifier: Apache-2.0 */
#include "storage_ide.h"
#include "../../../block/core/storage_core_api.h"
#include <reliefnt/blkdev.h>
#include <reliefnt/module.h>
#include <reliefnt/pci.h>
#include <reliefnt/heap.h>
#include <linux/errno.h>

struct ide_disk {
    struct ide_device_info native;
    struct block_device_registration registration;
};

/** @brief Read registry-validated ATA sectors or ATAPI blocks through CPU PIO.
 * @param context Stable owned disk. @param first First logical block.
 * @param count Bounded complete blocks. @param buffer Current writable bytes.
 * @return Zero or native errno. Caller pins device/code and transport exclusion;
 * all data moves through port I/O, never bus-addressed caller memory.
 */
static int ide_block_read(void *context,uint64_t first,uint32_t count,void *buffer)
{
    struct ide_disk *disk=context;
    return disk->native.atapi ? ide_atapi_read_blocks(&disk->native,first,count,buffer) :
        ide_pio_transfer(&disk->native,first,count,buffer,0);
}
/** @brief Write a bounded ATA extent from current readable kernel bytes.
 * @param context Stable owned disk. @param first First sector. @param count
 * Complete sectors. @param buffer Readable count*512 bytes.
 * @return Zero or native errno. Caller retains code/device/transport exclusion;
 * registry rejects optical writes before reaching this callback.
 */
static int ide_block_write(void *context,uint64_t first,uint32_t count,const void *buffer)
{ return ide_pio_transfer(&((struct ide_disk *)context)->native,first,count,(void *)buffer,1); }
/** @brief Flush the ATA volatile cache after filesystem writeback.
 * @param context Stable pinned disk under caller's transport exclusion.
 * @return Zero or native errno; no registry/module lock spans CPU polling.
 */
static int ide_block_flush(void *context)
{ return ide_flush_cache(&((struct ide_disk *)context)->native); }
static const struct block_device_operations ide_operations={
    .read=ide_block_read,.write=ide_block_write,.flush=ide_block_flush};

/** @brief Release one CPU-PIO context after registration/code users drain.
 * @param context Owned heap disk, consumed once. No pending DMA or interrupt
 * handler exists; owner quiesce already closes admission before final release.
 * Execution cleanup task, no callback, hardware I/O or allocation occurs.
 */
static void ide_disk_release(void *context) {kernel_free(context);}

/** @brief Decode one big-endian capacity word without provider state changes.
 * @param bytes Borrowed readable four-byte field. @return Native wire value.
 * Pure arithmetic, no locks, callbacks, allocations or ownership transfer.
 */
static uint32_t ide_be32(const uint8_t *bytes)
{ return (uint32_t)bytes[0]<<24|(uint32_t)bytes[1]<<16|(uint32_t)bytes[2]<<8|bytes[3]; }

/** @brief Probe one ATA/ATAPI endpoint and publish independent immutable geometry.
 * @param pci Exact controller identity. @param channel Primary/secondary index.
 * @param drive Master/slave index. @param command Native command port base.
 * @param control Native alternate-status/control port.
 * @return Zero when absent or published, negative allocation/registration errno.
 * Init task after MM/IRQ, caller serializes PIO. Context ownership predates
 * publication, and module init failure unwinds registrations before heap state.
 */
static int ide_probe_disk(const struct pci_device *pci,uint8_t channel,uint8_t drive,uint16_t command,uint16_t control)
{
    struct ide_device_info device;ide_fill_device(&device,channel,drive,command,control);
    if(ide_identify_device(&device)<0)return 0;
    struct block_device_info info={.block_size=device.atapi ? 2048u : 512u,
        .max_blocks=device.atapi ? IDE_MAX_ATAPI_BLOCKS : IDE_MAX_PIO_SECTORS,
        .transport=STORAGE_TRANSPORT_IDE_PIO,.bus=pci->bus,.slot=pci->slot,
        .function=pci->function,.port=channel*2u+drive,.blocks=device.sector_count};
    storage_copy_text(info.model,sizeof(info.model),device.model);
    if(device.atapi) {
        uint8_t response[8];if(ide_read_capacity(&device,response)<0)return 0;
        if(ide_be32(response+4)!=2048 || ide_be32(response)==UINT32_MAX)return 0;
        info.blocks=(uint64_t)ide_be32(response)+1u;
        info.flags=BLOCK_DEVICE_READ_ONLY|BLOCK_DEVICE_REMOVABLE|BLOCK_DEVICE_OPTICAL;
    }
    info.device=block_device_allocate_number(device.atapi);if(!info.device)return -ENOSPC;
    struct ide_disk *disk=kernel_malloc(sizeof(*disk));if(!disk)return -ENOMEM;
    *disk=(struct ide_disk){.native=device,.registration={.info=info,.operations=&ide_operations,
        .context=disk,.owner=THIS_MODULE}};
    int rc=module_add_resource(THIS_MODULE,NULL,ide_disk_release,disk);
    if(rc){kernel_free(disk);return rc;}
    rc=register_block_device(&disk->registration);
    if(rc && !THIS_MODULE)kernel_free(disk);
    if(!rc)console_info("[ide] registered pci=%u:%u.%u channel=%u drive=%u blocks=%llu block_size=%u model=%s\n",
        pci->bus,pci->slot,pci->function,channel,drive,(unsigned long long)info.blocks,info.block_size,info.model);
    return rc;
}

/** @brief Resolve a native-mode I/O BAR including its alternate-status offset.
 * @param pci Exact controller. @param offset BAR configuration offset.
 * @param control Whether the BAR denotes a control block (alternate status +2).
 * @return Valid 16-bit port or zero when malformed; never invent legacy ports
 * for a controller declaring native mode. Init-only PCI config observation.
 */
static uint16_t ide_io_bar(const struct pci_device *pci,uint8_t offset,bool control)
{
    uint32_t bar=pci_config_read32(pci->bus,pci->slot,pci->function,offset);
    if(!(bar&1u) || !(bar&~3u) || (bar&~3u)>65535u-(control ? 2u : 7u))return 0;
    return (bar&~3u)+(control ? 2u : 0u);
}

/** @brief Discover PCI IDE CPU-PIO transports before root probing or module use.
 * @return Zero when absent/probed or first registration/allocation error.
 * Init under execution ownership after IRQ/MM. Bus mastering stays disabled,
 * device IRQs are masked by IDENTIFY; no filesystem callback or self code pin.
 */
static int ide_init(void)
{
    int result=0;
    for(unsigned bus=0;bus<256;++bus)for(unsigned slot=0;slot<32;++slot)for(unsigned fn=0;fn<8;++fn) {
        struct pci_device pci;
        if(pci_read_device(bus,slot,fn,&pci)){if(!fn)break;continue;}
        if(pci.class_code!=ATA_CLASS_MASS_STORAGE || pci.subclass!=ATA_SUBCLASS_IDE)continue;
        uint16_t command=pci_config_read16(bus,slot,fn,4);
        pci_config_write16(bus,slot,fn,4,(command|1u)&~4u);
        for(unsigned channel=0;channel<2;++channel) {
            bool native=pci.prog_if&(channel ? 4u : 1u);
            uint16_t cmd=native ? ide_io_bar(&pci,0x10+channel*8,false) :
                channel ? IDE_SECONDARY_CMD_DEFAULT : IDE_PRIMARY_CMD_DEFAULT;
            uint16_t ctl=native ? ide_io_bar(&pci,0x14+channel*8,true) :
                channel ? IDE_SECONDARY_CTRL_DEFAULT : IDE_PRIMARY_CTRL_DEFAULT;
            if(!cmd || !ctl)continue;
            for(unsigned drive=0;drive<2;++drive) {
                int rc=ide_probe_disk(&pci,channel,drive,cmd,ctl);if(rc && !result)result=rc;
            }
        }
    }
    return result;
}
/** @brief Finish module exit after all PIO callbacks/registrations have drained.
 * No DMA/IRQ resources exist; owner ledger consumes the retained disk contexts.
 */
static void ide_exit(void) { }
block_initcall(ide_init);
module_exit(ide_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT PCI IDE ATA and ATAPI PIO block transport");
MODULE_ALIAS("pci:v*d*sv*sd*bc01sc01i*");
