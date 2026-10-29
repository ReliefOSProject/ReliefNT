/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <linux/errno.h>
#include <reliefnt/console.h>

/** @brief Decode a SCSI big-endian word from an unaligned response. */
static uint32_t scsi_be32(const uint8_t *p)
{
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}

/** @brief Decode a SCSI big-endian double word from an unaligned response. */
static uint64_t scsi_be64(const uint8_t *p)
{
    return (uint64_t)scsi_be32(p)<<32 | scsi_be32(p+4);
}

/** @brief Encode a SCSI big-endian word in an unaligned command. */
static void scsi_put32(uint8_t *p, uint32_t value)
{
    p[0]=(uint8_t)(value>>24); p[1]=(uint8_t)(value>>16);
    p[2]=(uint8_t)(value>>8); p[3]=(uint8_t)value;
}

/** @brief Encode a SCSI big-endian double word in an unaligned command. */
static void scsi_put64(uint8_t *p, uint64_t value)
{
    scsi_put32(p,(uint32_t)(value>>32)); scsi_put32(p+4,(uint32_t)value);
}

/** @brief Retire a published identity when its medium or geometry can change. */
static int scsi_revoke(struct usb_storage_lun *lun)
{
    __atomic_store_n(&lun->revoked,true,__ATOMIC_RELEASE);
    (void)disconnect_block_device(&lun->registration);
    return -ENODEV;
}

/** @brief Check a LUN's media identity and physical connection before I/O. */
static bool scsi_lun_gone(struct usb_storage_lun *lun)
{
    return __atomic_load_n(&lun->revoked,__ATOMIC_ACQUIRE) || usb_storage_gone(lun->storage);
}

/** @brief Forget cached write-through proofs if BOT recovery may reset a target. */
static void scsi_forget_cache(struct usb_storage *storage)
{
    for(unsigned i=0;i<USB_STORAGE_MAX_LUNS;++i)storage->luns[i].write_through=false;
}

struct scsi_sense_result { bool valid; uint8_t key, asc, ascq; };

/** @brief Fetch and translate fixed or descriptor sense after command failure. */
static int scsi_sense(struct usb_storage_lun *lun, struct scsi_sense_result *sense)
{
    uint8_t cdb[6]={0x03,0,0,0,18,0}, data[18]={0};
    uint32_t actual=0;
    int rc=usb_bot_command(lun->storage,lun->number,cdb,sizeof(cdb),data,sizeof(data),true,&actual);
    if(rc) {
        if(rc<0)scsi_forget_cache(lun->storage);
        return rc<0 ? rc : -EIO;
    }
    uint8_t format=data[0]&0x7fu,key,asc,ascq;
    /* Deferred/no-sense data cannot explain this failed command. */
    if(format==0x70 && actual>=14 && data[7]>=6) {
        key=data[2]&0x0fu; asc=data[12]; ascq=data[13];
    } else if(format==0x72 && actual>=8) {
        key=data[1]&0x0fu; asc=data[2]; ascq=data[3];
    } else return -EIO;
    if(!key)return -EIO;
    *sense=(struct scsi_sense_result){.valid=true,.key=key,.asc=asc,.ascq=ascq};
    switch(key) {
    case 2:
        if(asc==0x3a)return lun->registered ? scsi_revoke(lun) : -ENOMEDIUM;
        return -EAGAIN;
    case 5: return asc==0x21 ? -EINVAL : -EOPNOTSUPP;
    case 6:
        /* Reset or mode-parameter attention can affect other LUNs too. */
        scsi_forget_cache(lun->storage);
        return lun->registered ? scsi_revoke(lun) : -EAGAIN;
    case 7: return -EROFS;
    default: return -EIO;
    }
}

/** @brief Execute a CDB and expose whether a failure came from valid sense. */
static int scsi_command_result(struct usb_storage_lun *lun, const uint8_t *cdb,
                               uint8_t cdb_length, void *data, uint32_t length,
                               bool input, struct scsi_sense_result *sense, uint32_t *actual_out)
{
    uint32_t actual=0;
    if(sense)*sense=(struct scsi_sense_result){0};
    int rc=usb_bot_command(lun->storage,lun->number,cdb,cdb_length,data,length,input,&actual);
    if(actual_out)*actual_out=actual;
    if(rc==1) {
        struct scsi_sense_result decoded={0};
        rc=scsi_sense(lun,&decoded);
        if(sense)*sense=decoded;
        if(!decoded.valid) {
            /* The missing explanation could be a target-wide reset or mode
             * change, including while a later LUN is still being probed. */
            scsi_forget_cache(lun->storage);
            if(lun->registered) {
                (void)scsi_revoke(lun);
                if(rc==-EAGAIN)rc=-EIO;
            }
        }
        return rc;
    }
    if(rc<0) {
        scsi_forget_cache(lun->storage);
        if(lun->registered) {
            (void)scsi_revoke(lun);
            if(rc==-EAGAIN)rc=-EIO;
        }
    }
    if(!rc && !actual_out && actual!=length)return -EIO;
    return rc;
}

/** @brief Execute one SCSI CDB and request sense on command failure. */
static int scsi_command(struct usb_storage_lun *lun, const uint8_t *cdb,
                        uint8_t cdb_length, void *data, uint32_t length, bool input)
{
    return scsi_command_result(lun,cdb,cdb_length,data,length,input,NULL,NULL);
}

/** @brief Acquire the host gate once across a command and any request sense. */
static int scsi_begin(struct usb_storage_lun *lun)
{
    struct usb_storage *storage=lun->storage;
    if(scsi_lun_gone(lun))return -ENODEV;
    int rc=storage->device->operations->acquire(storage->device);
    if(rc)return rc==-EBUSY ? -EAGAIN : rc;
    if(scsi_lun_gone(lun)) {
        storage->device->operations->release(storage->device);
        return -ENODEV;
    }
    return 0;
}

/** @brief Release the gate after one complete SCSI transaction. */
static void scsi_end(struct usb_storage_lun *lun)
{
    lun->storage->device->operations->release(lun->storage->device);
}

/** @brief Validate bounds and encode a READ/WRITE(10) or (16) CDB. */
static int scsi_transfer(struct usb_storage_lun *lun, uint64_t first,
                         uint32_t count, void *buffer, bool write)
{
    const struct block_device_info *info=&lun->registration.info;
    if(scsi_lun_gone(lun))return -ENODEV;
    if(!buffer || !count || count>info->max_blocks || first>=info->blocks ||
       (uint64_t)count>info->blocks-first)return -EINVAL;
    if(write && (info->flags&BLOCK_DEVICE_READ_ONLY))return -EROFS;
    int rc=scsi_begin(lun);
    if(rc)return rc;
    uint8_t cdb[16]={0}; uint8_t cdb_length;
    if(first<=UINT32_MAX && (uint64_t)(count-1)<=UINT32_MAX-first && count<=UINT16_MAX) {
        cdb[0]=write ? 0x2a : 0x28; scsi_put32(cdb+2,(uint32_t)first);
        cdb[7]=(uint8_t)(count>>8); cdb[8]=(uint8_t)count; cdb_length=10;
    } else {
        cdb[0]=write ? 0x8a : 0x88; scsi_put64(cdb+2,first);
        scsi_put32(cdb+10,count); cdb_length=16;
    }
    /* Under the host gate, record the attempt before any write CBW can run. */
    if(write)lun->write_attempted=true;
    rc=scsi_command(lun,cdb,cdb_length,buffer,count*512u,!write);
    /* After attempting a write CBW, target-side progress can be indeterminate. */
    if(write && rc==-EAGAIN)rc=-EIO;
    scsi_end(lun); return rc;
}

/** @brief Read a complete bounded extent through the serialized USB engine. */
static int scsi_block_read(void *context, uint64_t first, uint32_t count, void *buffer)
{
    return scsi_transfer(context,first,count,buffer,false);
}

/** @brief Write a complete bounded extent without replaying a partial transfer. */
static int scsi_block_write(void *context, uint64_t first, uint32_t count, const void *buffer)
{
    return scsi_transfer(context,first,count,(void *)buffer,true);
}

/** @brief Synchronize the target cache, preserving unsupported/error results. */
static int scsi_block_flush(void *context)
{
    struct usb_storage_lun *lun=context;
    int rc=scsi_begin(lun);
    if(rc)return rc;
    if(!lun->write_attempted || (lun->registration.info.flags&BLOCK_DEVICE_READ_ONLY) || lun->write_through) {
        scsi_end(lun);return 0;
    }
    uint8_t cdb[10]={0x35}; struct scsi_sense_result sense;
    rc=scsi_command_result(lun,cdb,sizeof(cdb),NULL,0,false,&sense,NULL);
    /* Only an explicit opcode/CDB-format rejection justifies another form. */
    if(rc==-EOPNOTSUPP && sense.valid && sense.key==5 && !sense.ascq &&
       (sense.asc==0x20 || sense.asc==0x24) && !scsi_lun_gone(lun)) {
        uint8_t cdb16[16]={0x91};
        rc=scsi_command(lun,cdb16,sizeof(cdb16),NULL,0,false);
        console_info("[usb] storage sync-cache lun=%u fallback=16 from=%02x/%02x rc=%d\n",
            lun->number,sense.asc,sense.ascq,rc);
    }
    scsi_end(lun); return rc;
}

static const struct block_device_operations scsi_operations={
    .read=scsi_block_read,.write=scsi_block_write,.flush=scsi_block_flush
};

/** @brief Retry transient unit attention or not-ready status a bounded number. */
static int scsi_ready(struct usb_storage_lun *lun)
{
    uint8_t cdb[6]={0}; int rc=-EIO;
    for(unsigned attempt=0;attempt<4;++attempt) {
        rc=scsi_command(lun,cdb,sizeof(cdb),NULL,0,false);
        if(rc!=-EAGAIN && rc!=-ENOMEDIUM)return rc;
    }
    return rc;
}

/** @brief Discover real 512-byte capacity, using READ CAPACITY(16) when needed. */
static int scsi_capacity(struct usb_storage_lun *lun, uint64_t *blocks)
{
    uint8_t cdb[16]={0x25},data[32]={0};
    int rc=scsi_command(lun,cdb,10,data,8,true);
    if(rc)return rc;
    uint64_t last=scsi_be32(data);
    uint32_t size=scsi_be32(data+4);
    if(last==UINT32_MAX) {
        for(unsigned i=0;i<sizeof(cdb);++i)cdb[i]=0;
        cdb[0]=0x9e; cdb[1]=0x10; scsi_put32(cdb+10,sizeof(data));
        rc=scsi_command(lun,cdb,sizeof(cdb),data,sizeof(data),true);
        if(rc)return rc;
        last=scsi_be64(data); size=scsi_be32(data+8);
    }
    if(size!=512)return -EOPNOTSUPP;
    if(last==UINT64_MAX)return -EOVERFLOW;
    *blocks=last+1; return 0;
}

/** @brief Copy SCSI inquiry vendor/product text into a bounded inventory name. */
static void scsi_model(char model[41], const uint8_t inquiry[36])
{
    unsigned n=0;
    for(unsigned i=8;i<32 && n<40;++i) {
        uint8_t c=inquiry[i];
        model[n++]=(c>=0x20 && c<=0x7e) ? (char)c : ' ';
    }
    while(n && model[n-1]==' ')--n;
    model[n]='\0';
}

/** @brief Validate one complete current caching page and return its WCE state. */
static int scsi_cache_page(const uint8_t *data,uint32_t actual,bool ten,bool *through)
{
    uint32_t header=ten ? 8u : 4u;
    if(actual<header)return -EPROTO;
    uint32_t total=ten ? ((uint32_t)data[0]<<8 | data[1])+2u : (uint32_t)data[0]+1u;
    uint32_t descriptors=ten ? ((uint32_t)data[6]<<8 | data[7]) : data[3];
    if(total<header || total>actual || descriptors%8u || descriptors>total-header)return -EPROTO;
    uint32_t offset=header+descriptors;
    if(total-offset<3 || (data[offset]&0x7fu)!=8 || data[offset+1]<10 ||
       (uint32_t)data[offset+1]+2u!=total-offset)return -EPROTO;
    *through=(data[offset+2]&4u)==0;
    return 0;
}

/** @brief Walk a complete MODE SENSE(6) all-pages response safely. */
static int scsi_cache_all_pages(const uint8_t *data,uint32_t actual,bool *through)
{
    if(actual<4)return -EPROTO;
    uint32_t total=(uint32_t)data[0]+1u,descriptors=data[3];
    if(total<4 || total>actual || descriptors%8u || descriptors>total-4)return -EPROTO;
    uint32_t offset=4u+descriptors;bool found=false,cache_through=false;
    while(offset<total) {
        uint32_t remaining=total-offset;
        if(remaining<2)return -EPROTO;
        bool subpage=(data[offset]&0x40u)!=0;
        uint8_t code=data[offset]&0x3fu;
        uint32_t size;
        if(subpage) {
            if(remaining<4 || !data[offset+1])return -EPROTO;
            size=4u+((uint32_t)data[offset+2]<<8 | data[offset+3]);
        } else size=2u+data[offset+1];
        if(size>remaining)return -EPROTO;
        if(code==8 && !subpage) {
            if(found || size<12)return -EPROTO;
            cache_through=(data[offset+2]&4u)==0;found=true;
        }
        offset+=size;
    }
    if(!found)return -EPROTO;
    *through=cache_through;return 0;
}

/** @brief Query bounded current cache mode; unknown is never treated as durable. */
static int scsi_cache_probe(struct usb_storage_lun *lun,bool *fatal)
{
    uint8_t data[512];uint32_t actual=0;int rc=-EOPNOTSUPP;unsigned format=6;
    bool through=false; struct scsi_sense_result sense={0};
    for(unsigned attempt=0;attempt<3;++attempt) {
        bool ten=attempt==1,all=attempt==2;format=ten ? 10 : 6;
        uint8_t cdb[10]={0};cdb[0]=ten ? 0x5a : 0x1a;cdb[1]=8;cdb[2]=all ? 0x3f : 8;
        uint32_t size=ten ? sizeof(data) : 255u;
        if(ten) {cdb[7]=(uint8_t)(size>>8);cdb[8]=(uint8_t)size;}
        else cdb[4]=(uint8_t)size;
        __builtin_memset(data,0,sizeof(data));actual=0;
        rc=scsi_command_result(lun,cdb,ten ? 10 : 6,data,size,true,&sense,&actual);
        int command_rc=rc,parse_rc=0;
        if(!rc) {
            parse_rc=all ? scsi_cache_all_pages(data,actual,&through) :
                scsi_cache_page(data,actual,ten,&through);
            rc=parse_rc;
        }
        console_info("[usb] storage cache-query lun=%u sense=%u page=%02x rc=%d bytes=%u decoded=%u key=%u asc=%02x ascq=%02x parsed=%u parse=%d\n",
            lun->number,format,cdb[2],command_rc,actual,sense.valid,sense.key,sense.asc,sense.ascq,!command_rc,parse_rc);
        if(rc) {
            if(!command_rc)continue;
            if(sense.valid && (rc==-EAGAIN || rc==-ENOMEDIUM))break;
            if(sense.valid && rc==-EOPNOTSUPP)continue;
            *fatal=true;break;
        }
        lun->write_through=through;break;
    }
    console_info("[usb] storage cache lun=%u mode=%s sense=%u rc=%d bytes=%u\n",
        lun->number,lun->write_through ? "write-through" : !rc ? "write-back" : "unknown",
        format,rc,actual);
    if(*fatal || (sense.valid && (rc==-EAGAIN || rc==-ENOMEDIUM)))return rc;
    return 0;
}

/** @brief Probe a direct-access LUN and publish a persistent removable disk. */
int usb_scsi_probe_lun(struct usb_storage *storage, uint8_t number, bool *fatal)
{
    *fatal=false;
    struct usb_storage_lun *lun=&storage->luns[number];
    lun->storage=storage; lun->number=number;
    uint8_t inquiry[36]={0},cdb[6]={0x12,0,0,0,sizeof(inquiry),0};
    int rc=scsi_command(lun,cdb,sizeof(cdb),inquiry,sizeof(inquiry),true);
    if(rc)return rc;
    if((inquiry[0]&0xe0u) || (inquiry[0]&0x1fu)!=0)return -EOPNOTSUPP;
    rc=scsi_ready(lun); if(rc)return rc;
    uint64_t blocks=0; rc=scsi_capacity(lun,&blocks); if(rc)return rc;
    uint32_t flags=BLOCK_DEVICE_REMOVABLE;
    uint8_t mode[4]={0},mode_cdb[6]={0x1a,0x08,0x3f,0,sizeof(mode),0};
    struct scsi_sense_result sense={0};uint32_t mode_actual=0;
    rc=scsi_command_result(lun,mode_cdb,sizeof(mode_cdb),mode,sizeof(mode),true,&sense,&mode_actual);
    if(!rc && mode_actual!=sizeof(mode))rc=-EIO;
    if(!rc) {
        lun->dpofua=(mode[2]&0x10u)!=0;
        if(mode[0]>=3 && !(mode[3]%8u) && mode[3]<=(uint32_t)mode[0]-3u)
            lun->dpofua_valid=true;
    }
    console_info("[usb] storage mode-header lun=%u rc=%d bytes=%u valid=%u dpofua=%u wp=%u length=%u descriptors=%u\n",
        lun->number,rc,mode_actual,lun->dpofua_valid,lun->dpofua,
        (mode[2]&0x80u)!=0,mode[0],mode[3]);
    if(!rc) { if(mode[2]&0x80u)flags|=BLOCK_DEVICE_READ_ONLY; }
    else if(sense.valid && (rc==-ENOMEDIUM || rc==-EAGAIN))return rc;
    else if(!sense.valid || rc!=-EOPNOTSUPP) { *fatal=true; return rc; }
    rc=scsi_cache_probe(lun,fatal);if(rc)return rc;
    struct block_device_info info={.blocks=blocks,.block_size=512,
        .max_blocks=USB_STORAGE_MAX_BLOCKS,.flags=flags,.transport=BLOCK_TRANSPORT_USB,
        .bus=storage->device->bus,.slot=storage->device->slot,
        .function=storage->device->function,.port=storage->device->root_port,
        .namespace_id=number};
    info.device=block_device_allocate_number(false);
    if(!info.device)return -ENOSPC;
    scsi_model(info.model,inquiry);
    lun->registration=(struct block_device_registration){.info=info,
        .operations=&scsi_operations,.context=lun,.owner=storage->device->owner};
    rc=register_block_device(&lun->registration);
    if(!rc)lun->registered=true;
    return rc;
}
