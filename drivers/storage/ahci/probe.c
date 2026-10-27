#include <reliefnt/console.h>
/* SPDX-License-Identifier: Apache-2.0 */
#include "ahci.h"
#include "../../../block/core/storage_core_api.h"
#include <reliefnt/module.h>
#include <reliefnt/pci.h>
#include <reliefnt/heap.h>
#include <linux/errno.h>

struct ahci_disk {
    struct ahci_disk *next;
    struct ahci_hba_port *port;
    struct block_device_registration registration;
};
struct ahci_controller {
    struct ahci_hba_mem *abar;
    size_t bytes;
    struct ahci_disk *disks;
    uint32_t implemented;
    uint8_t bus,slot,function;
};

/** @brief Enter one ATA/ATAPI read using a pinned registration and owned DMA.
 * @param context Stable driver-owned disk. @param first First logical block.
 * @param count Registry-validated blocks. @param buffer Current writable bytes.
 * @return Zero or native errno including -EAGAIN. Caller owns transport lock
 * and exact block/code lifetime; no registry lock spans hardware access.
 */
static int ahci_disk_read(void *context,uint64_t first,uint32_t count,void *buffer)
{
    struct ahci_disk *disk=context;
    return disk->registration.info.flags&BLOCK_DEVICE_OPTICAL ?
        ahci_read_atapi_blocks_retry(disk->port,first,count,buffer) :
        ahci_read_lba_retry(disk->port,first,count,buffer);
}
/** @brief Write one registry-validated ATA extent via independently owned DMA.
 * @param context Stable disk. @param first First sector. @param count Sectors.
 * @param buffer Current readable bytes. @return Zero or native errno. Caller
 * holds transport exclusion and block/code lifetime; optical writes are gated.
 */
static int ahci_disk_write(void *context,uint64_t first,uint32_t count,const void *buffer)
{ return ahci_write_lba_retry(((struct ahci_disk *)context)->port,first,count,buffer); }
/** @brief Flush a writable ATA registration after native filesystem writeback.
 * @param context Stable pinned disk under transport exclusion.
 * @return Zero or native errno; no registry lock spans device completion.
 */
static int ahci_disk_flush(void *context)
{ return ahci_flush_cache(((struct ahci_disk *)context)->port); }
/** @brief Discard only this port's pending task command without touching task memory.
 * @param context Stable pinned disk. @param pid Exiting task identity.
 * Transport exclusion supplied by caller; shared driver cancellation is idempotent.
 */
static void ahci_disk_cancel(void *context,uint32_t pid)
{ (void)context;ahci_cancel_task(pid); }
static const struct block_device_operations ahci_operations={.read=ahci_disk_read,
    .write=ahci_disk_write,.flush=ahci_disk_flush,.cancel=ahci_disk_cancel};

/** @brief STOP all implemented engines before releasing any controller memory.
 * @param context Stable controller with new admission and external users closed.
 * @return Zero or -EIO; failed STOP retains BAR, contexts, DMA and module image.
 * Owner cleanup task under execution ownership; no registry/module lock spans
 * bounded hardware waits. All ports are attempted even after one fails.
 */
static int ahci_controller_stop(void *context)
{
    struct ahci_controller *controller=context;int rc=0;
    for(unsigned p=0;p<32;++p)if(controller->implemented&(1u<<p))
        if(ahci_stop_port(&controller->abar->ports[p]))rc=-EIO;
    if(!rc) {
        uint16_t command=pci_config_read16(controller->bus,controller->slot,controller->function,4);
        pci_config_write16(controller->bus,controller->slot,controller->function,4,command&~4u);
    }
    return rc;
}
/** @brief Free registrations/backing and exact BAR lease only after complete STOP.
 * @param context Owned controller; all registry resources already retired.
 * Cleanup task with execution ownership, no callbacks/users/IRQs can restart it.
 * DMA buffers are consumed before MMIO unmapping and controller heap release.
 */
static void ahci_controller_release(void *context)
{
    struct ahci_controller *controller=context;
    for(unsigned p=0;p<32;++p)if(controller->implemented&(1u<<p))
        ahci_release_stopped_port(&controller->abar->ports[p]);
    while(controller->disks) {
        struct ahci_disk *disk=controller->disks;controller->disks=disk->next;kernel_free(disk);
    }
    pci_unmap_mmio(controller->abar,controller->bytes);kernel_free(controller);
}

/** @brief Decode one fixed-width big-endian SCSI capacity response word.
 * @param bytes Borrowed readable four-byte wire field.
 * @return Native unsigned value; pure arithmetic, no lock or ownership changes.
 */
static uint32_t ahci_be32(const uint8_t *bytes)
{ return (uint32_t)bytes[0]<<24|(uint32_t)bytes[1]<<16|(uint32_t)bytes[2]<<8|bytes[3]; }

/** @brief Probe capacity then publish one disk independently of any filesystem.
 * @param controller Owned STOP-clean controller/BAR with busmaster enabled.
 * @param port Implemented attached port index, below 32.
 * @return Zero or allocation/geometry/native I/O error. Init task under execution
 * ownership; descriptor context is linked before publication and cleanup ledger
 * predates hardware DMA. Registration itself creates no module self-pin.
 */
static int ahci_probe_disk(struct ahci_controller *controller,unsigned port)
{
    struct ahci_hba_port *p=&controller->abar->ports[port];
    bool optical=p->sig==AHCI_PORT_SIG_ATAPI;
    int rc=ahci_setup_port(p,controller->abar->cap&(1u<<31) ? UINT64_MAX : UINT32_MAX);
    if(rc)return rc;
    struct block_device_info info={.block_size=optical ? 2048u : 512u,.max_blocks=optical ? 32u : AHCI_MAX_SECTORS,
        .transport=STORAGE_TRANSPORT_AHCI,.bus=controller->bus,.slot=controller->slot,
        .function=controller->function,.port=port};
    if(optical) {
        uint8_t response[8];rc=ahci_read_capacity(p,response);if(rc)return rc;
        if(ahci_be32(response+4)!=2048 || ahci_be32(response)==UINT32_MAX)return -EOPNOTSUPP;
        info.blocks=(uint64_t)ahci_be32(response)+1u;
        info.flags=BLOCK_DEVICE_READ_ONLY|BLOCK_DEVICE_REMOVABLE|BLOCK_DEVICE_OPTICAL;
        storage_copy_text(info.model,sizeof(info.model),"SATA ATAPI optical drive");
    } else {
        uint16_t identify[256];rc=ahci_identify_device(p,identify);if(rc)return rc;
        if(!(identify[83]&(1u<<10)))return -EOPNOTSUPP;
        if((identify[106]&0xc000u)==0x4000u && (identify[106]&(1u<<12)) &&
            ((uint32_t)identify[117]|(uint32_t)identify[118]<<16)!=256u)return -EOPNOTSUPP;
        info.blocks=(uint64_t)identify[100]|(uint64_t)identify[101]<<16|
            (uint64_t)identify[102]<<32|(uint64_t)identify[103]<<48;
        if(!info.blocks || info.blocks>(1ull<<48))return -EOPNOTSUPP;
        for(unsigned i=0;i<20;++i){info.model[2*i]=identify[27+i]>>8;info.model[2*i+1]=identify[27+i];}
        for(unsigned i=40;i && info.model[i-1]==' ';--i)info.model[i-1]=0;
    }
    info.device=block_device_allocate_number(optical);if(!info.device)return -ENOSPC;
    struct ahci_disk *disk=kernel_malloc(sizeof(*disk));if(!disk)return -ENOMEM;
    *disk=(struct ahci_disk){.next=controller->disks,.port=p,.registration={.info=info,
        .operations=&ahci_operations,.context=disk,.owner=THIS_MODULE}};
    controller->disks=disk;
    rc=register_block_device(&disk->registration);
    if(!rc)console_info("[ahci] registered pci=%u:%u.%u port=%u blocks=%llu block_size=%u model=%s\n",
        info.bus,info.slot,info.function,port,(unsigned long long)info.blocks,info.block_size,info.model);
    return rc;
}

/** @brief Map only the implemented AHCI BAR extent and take over stopped engines.
 * @param device Exact class-validated PCI function, borrowed during probe.
 * @return Zero or probe/STOP/allocation errno; failed STOP retains ledger/code.
 * Init task under execution ownership, before any registration is admitted.
 * ABAR is BAR5 (32-bit, last BAR); configuration offset 0x28 is never its high
 * word. Memory decode/BME ordering keeps firmware DMA off until STOP/owned CLBs.
 */
static int ahci_probe_controller(const struct pci_device *device)
{
    uint32_t bar=pci_config_read32(device->bus,device->slot,device->function,0x24);
    if(!bar || (bar&7u))return -EOPNOTSUPP;
    uint64_t physical=bar&~15u;
    uint16_t command=pci_config_read16(device->bus,device->slot,device->function,4);
    pci_config_write16(device->bus,device->slot,device->function,4,(command|2u)&~4u);
    if(!pci_device_bar_contains(device,5,physical,4096))return -EOPNOTSUPP;
    struct ahci_hba_mem *abar=pci_map_mmio(physical,4096);if(!abar)return -ENOMEM;
    uint32_t implemented=abar->pi;
    unsigned ports=32;while(ports && !(implemented&(1u<<(ports-1))))--ports;
    size_t bytes=(0x100u+ports*0x80u+4095u)&~4095u;
    if(bytes>4096) {
        pci_unmap_mmio(abar,4096);
        if(!pci_device_bar_contains(device,5,physical,bytes))return -EOPNOTSUPP;
        abar=pci_map_mmio(physical,bytes);if(!abar)return -ENOMEM;
    }
    struct ahci_controller *controller=kernel_malloc(sizeof(*controller));
    if(!controller){pci_unmap_mmio(abar,bytes);return -ENOMEM;}
    *controller=(struct ahci_controller){.abar=abar,.bytes=bytes,.implemented=implemented,
        .bus=device->bus,.slot=device->slot,.function=device->function};
    int rc=module_add_resource(THIS_MODULE,ahci_controller_stop,ahci_controller_release,controller);
    if(rc){pci_unmap_mmio(abar,bytes);kernel_free(controller);return rc;}
    abar->ghc=(abar->ghc|AHCI_GHC_AE)&~2u;
    for(unsigned p=0;p<32;++p)if(implemented&(1u<<p))abar->ports[p].ie=0;
    rc=ahci_controller_stop(controller);
    if(rc)return rc;
    pci_config_write16(device->bus,device->slot,device->function,4,command|6u);
    for(unsigned p=0;p<32;++p)if(implemented&(1u<<p)) {
        struct ahci_hba_port *port=&abar->ports[p];
        if((port->ssts&15u)!=AHCI_PORT_DET_PRESENT || ((port->ssts>>8)&15u)!=AHCI_PORT_IPM_ACTIVE ||
           (port->sig!=AHCI_PORT_SIG_ATA && port->sig!=AHCI_PORT_SIG_ATAPI))continue;
        int result=ahci_probe_disk(controller,p);
        if(result)console_warn("[ahci] unavailable pci=%u:%u.%u port=%u status=%d\n",device->bus,device->slot,device->function,p,result);
    }
    return 0;
}

/** @brief Discover AHCI functions during early builtin phase or native module init.
 * @return Zero when absent/probed, or first controller-level error triggering
 * native owner rollback. Init task after IRQ/MM; no filesystem/root probing.
 * The immutable registration gate prevents COMING code from admitting users.
 */
static int ahci_init(void)
{
    int rc=0;
    for(unsigned bus=0;bus<256;++bus)for(unsigned slot=0;slot<32;++slot)for(unsigned function=0;function<8;++function) {
        struct pci_device device;
        if(pci_read_device(bus,slot,function,&device)){if(!function)break;continue;}
        if(device.class_code==ATA_CLASS_MASS_STORAGE && device.subclass==ATA_SUBCLASS_SATA && device.prog_if==ATA_PROGIF_AHCI) {
            int result=ahci_probe_controller(&device);if(result && !rc)rc=result;
        }
    }
    return rc;
}
/** @brief Finish module exit after registry admission and hardware/backing drain.
 * All fallible STOP work belongs to owner quiesce; no pointer remains published.
 */
static void ahci_exit(void) { }
block_initcall(ahci_init);
module_exit(ahci_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT AHCI ATA and ATAPI block transport");
MODULE_ALIAS("pci:v*d*sv*sd*bc01sc06i01*");
