/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/export.h>
#include <reliefnt/heap.h>
#include <linux/errno.h>

/** @brief Encode an unaligned BOT little-endian word. */
static void bot_put32(uint8_t *p, uint32_t value)
{
    p[0]=(uint8_t)value; p[1]=(uint8_t)(value>>8);
    p[2]=(uint8_t)(value>>16); p[3]=(uint8_t)(value>>24);
}

/** @brief Decode an unaligned BOT little-endian word. */
static uint32_t bot_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 |
        (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}

/** @brief Reset the class then clear IN and OUT halts in required order. */
static void bot_reset(struct usb_storage *storage)
{
    struct usb_device *device=storage->device;
    const struct usb_device_operations *ops=device->operations;
    uint8_t setup[8]={0x21,0xff,0,0,storage->interface_number,0,0,0};
    uint32_t actual=0;
    int reset=ops->control(device,setup,NULL,0,&actual);
    int clear_in=ops->clear_halt(device,&storage->in);
    int clear_out=ops->clear_halt(device,&storage->out);
    /* A failed recovery leaves command synchronization indeterminate. */
    if(reset || actual || clear_in || clear_out)storage->broken=true;
}

/** @brief Issue the CBW, data and CSW once; never replay a write. */
int usb_bot_command(struct usb_storage *storage, uint8_t lun, const uint8_t *cdb,
                    uint8_t cdb_length, void *data, uint32_t length, bool input,
                    uint32_t *actual)
{
    if(!storage || !cdb || !cdb_length || cdb_length>16 || lun>=16 ||
       (length && !data) || !actual)return -EINVAL;
    *actual=0;
    if(usb_storage_gone(storage))return -ENODEV;
    if(storage->broken)return -EIO;
    struct usb_device *device=storage->device;
    const struct usb_device_operations *ops=device->operations;
    uint8_t cbw[31]={0},csw[13]={0};
    uint32_t tag=++storage->tag, transferred=0;
    bot_put32(cbw,0x43425355u); bot_put32(cbw+4,tag);
    bot_put32(cbw+8,length); cbw[12]=input ? 0x80u : 0;
    cbw[13]=lun; cbw[14]=cdb_length;
    for(uint8_t i=0;i<cdb_length;++i)cbw[15+i]=cdb[i];
    int rc=ops->bulk(device,&storage->out,cbw,sizeof(cbw),&transferred);
    if(rc || transferred!=sizeof(cbw)) {
        if(rc!=-ENODEV && !usb_storage_gone(storage))bot_reset(storage);
        return rc ? rc : -EPROTO;
    }
    uint32_t data_actual=0;
    if(length) {
        struct usb_endpoint *ep=input ? &storage->in : &storage->out;
        rc=ops->bulk(device,ep,data,length,&data_actual);
        if(data_actual>length) {
            if(!usb_storage_gone(storage))bot_reset(storage);
            return -EPROTO;
        }
        if(rc==-EPIPE) {
            rc=ops->clear_halt(device,ep);
            if(rc) {
                if(rc!=-ENODEV && !usb_storage_gone(storage))bot_reset(storage);
                return rc;
            }
        } else if(rc) {
            if(rc!=-ENODEV && !usb_storage_gone(storage))bot_reset(storage);
            return rc;
        }
    }
    *actual=data_actual;
    transferred=0;
    rc=ops->bulk(device,&storage->in,csw,sizeof(csw),&transferred);
    if(rc && rc!=-ENODEV && !usb_storage_gone(storage)) {
        int clear=ops->clear_halt(device,&storage->in);
        if(!clear) {
            transferred=0;
            rc=ops->bulk(device,&storage->in,csw,sizeof(csw),&transferred);
        } else rc=clear;
    }
    if(rc) {
        if(rc!=-ENODEV && !usb_storage_gone(storage))bot_reset(storage);
        return rc;
    }
    uint32_t residue=bot_get32(csw+8);
    if(transferred!=sizeof(csw) || bot_get32(csw)!=0x53425355u ||
       bot_get32(csw+4)!=tag || csw[12]>2 || csw[12]==2 || residue>length ||
       length-residue>data_actual) {
        if(!usb_storage_gone(storage))bot_reset(storage);
        return -EPROTO;
    }
    *actual=length-residue;
    if(csw[12]==1)return 1;
    return 0;
}

/** @brief Allocate a class state and discover all reported BOT logical units. */
int usb_storage_probe(struct usb_device *device, uint8_t interface_number,
                      struct usb_endpoint in, struct usb_endpoint out,
                      struct usb_storage **result)
{
    if(!result || *result || !device || !device->operations ||
       !device->operations->control || !device->operations->bulk ||
       !device->operations->clear_halt || !device->operations->acquire ||
       !device->operations->release || !(in.address&0x80u) ||
       (out.address&0x80u) || !(in.address&0x0fu) || !(out.address&0x0fu) ||
       !in.max_packet || !out.max_packet)return -EINVAL;
    struct usb_storage *storage=kernel_malloc(sizeof(*storage));
    if(!storage)return -ENOMEM;
    *storage=(struct usb_storage){.device=device,.interface_number=interface_number,
                                 .in=in,.out=out};
    *result=storage;
    if(usb_storage_gone(storage))return -ENODEV;
    uint8_t max_lun=0,setup[8]={0xa1,0xfe,0,0,interface_number,0,1,0};
    uint32_t actual=0;
    int rc=device->operations->control(device,setup,&max_lun,1,&actual);
    if(rc==-EPIPE)max_lun=0;
    else if(rc)return rc;
    else if(actual!=1 || max_lun>=USB_STORAGE_MAX_LUNS)return -EPROTO;
    int first=0; unsigned found=0;
    for(unsigned i=0;i<=(unsigned)max_lun;++i) {
        bool fatal=false;
        rc=usb_scsi_probe_lun(storage,(uint8_t)i,&fatal);
        if(storage->broken)return -EIO;
        if(fatal)return rc;
        if(!rc) { ++found; continue; }
        if(!first)first=rc;
        if(rc!=-ENOMEDIUM && rc!=-EAGAIN && rc!=-EOPNOTSUPP && rc!=-ENODEV)
            return rc;
        if(usb_storage_gone(storage))return -ENODEV;
    }
    return found ? 0 : (first ? first : -ENODEV);
}

/** @brief Revoke I/O and retire each idle descriptor, retaining busy LUNs. */
int usb_storage_disconnect(struct usb_storage *storage)
{
    if(!storage)return 0;
    __atomic_store_n(&storage->detached,true,__ATOMIC_RELEASE);
    int result=0;
    for(unsigned i=0;i<USB_STORAGE_MAX_LUNS;++i) {
        struct usb_storage_lun *lun=&storage->luns[i];
        if(!lun->registered)continue;
        int rc=disconnect_block_device(&lun->registration);
        if(rc && rc!=-ENOENT) { if(!result)result=rc; continue; }
        rc=unregister_block_device(&lun->registration);
        if(!rc || rc==-ENOENT)lun->registered=false;
        else if(!result || rc==-EBUSY)result=rc;
    }
    return result;
}

/** @brief Free class metadata after registrations and callbacks have drained. */
void usb_storage_release(struct usb_storage *storage)
{
    if(storage)kernel_free(storage);
}

EXPORT_SYMBOL(usb_storage_probe);
EXPORT_SYMBOL(usb_storage_disconnect);
EXPORT_SYMBOL(usb_storage_release);
