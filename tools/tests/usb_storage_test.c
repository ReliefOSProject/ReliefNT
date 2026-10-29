/* SPDX-License-Identifier: Apache-2.0 */
/* BOT target scripted at the USB/registry boundary; implementation is linked. */
#include <reliefnt/usb_device.h>
#include <reliefnt/blkdev.h>
#include <reliefnt/console.h>
#ifdef REAL_REGISTRY
#include <reliefnt/module.h>
#include <reliefnt/lock.h>
#endif
#include <linux/errno.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int usb_storage_probe(struct usb_device *, uint8_t, struct usb_endpoint,
                             struct usb_endpoint, struct usb_storage **) __attribute__((weak));
extern int usb_storage_disconnect(struct usb_storage *) __attribute__((weak));
extern void usb_storage_release(struct usb_storage *) __attribute__((weak));

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #x); exit(1); } } while (0)
#define IN 0x81u
#define OUT 0x02u
#define OWNER ((struct module *)(uintptr_t)0x1234)
#define MAX_REG 16

#ifndef REAL_REGISTRY
struct registration_slot { const struct block_device_registration *registration; int busy, inactive; };
static struct registration_slot registered[MAX_REG];
static unsigned reg_count;
#endif
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *p) { free(p); }
static char diagnostics[65536];static size_t diagnostics_length;
void console_log(enum console_log_level level,const char *format,...)
{
    (void)level;
    if(diagnostics_length>=sizeof(diagnostics)-1)return;
    va_list args;va_start(args,format);
    int written=vsnprintf(diagnostics+diagnostics_length,sizeof(diagnostics)-diagnostics_length,format,args);
    va_end(args);
    if(written>0) {
        size_t room=sizeof(diagnostics)-diagnostics_length;
        diagnostics_length+=(size_t)written<room ? (size_t)written : room-1;
    }
}
#ifndef REAL_REGISTRY
static uint64_t next_number = 1;
uint64_t block_device_allocate_number(bool optical) { CHECK(!optical); return next_number++; }
int register_block_device(const struct block_device_registration *r)
{
    CHECK(reg_count < MAX_REG && r && r->context && r->operations);
    CHECK(r->owner == OWNER && r->info.transport == 5);
    CHECK(r->info.flags & BLOCK_DEVICE_REMOVABLE);
    registered[reg_count++].registration = r;
    return 0;
}
int disconnect_block_device(const struct block_device_registration *r)
{
    for (unsigned i=0; i<reg_count; ++i) {
        if (registered[i].registration==r) { registered[i].inactive=1; return 0; }
    }
    return -ENOENT;
}
int unregister_block_device(const struct block_device_registration *r)
{
    for (unsigned i = 0; i < reg_count; ++i) {
        if (registered[i].registration == r) {
            if (registered[i].busy) return -EBUSY;
            registered[i].registration = NULL;
            return 0;
        }
    }
    return -ENOENT;
}
#else
static int (*ledger_quiesce[MAX_REG])(void *);
static void (*ledger_release[MAX_REG])(void *);
static void *ledger_context[MAX_REG];
static unsigned ledger_count, module_refs, executions;
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ CHECK(!lock->state); lock->state=1; if (flags) *flags=0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; CHECK(lock->state); lock->state=0; }
bool try_module_get(struct module *owner)
{ CHECK(owner==OWNER); ++module_refs; return true; }
void module_put(struct module *owner)
{ CHECK(owner==OWNER && module_refs); --module_refs; }
bool module_execution_get(struct module *owner)
{ CHECK(owner==OWNER); ++executions; return true; }
void module_execution_put(struct module *owner)
{ CHECK(owner==OWNER && executions); --executions; }
int module_add_resource(struct module *owner, int (*quiesce)(void *),
                        void (*release_fn)(void *), void *context)
{
    CHECK(owner==OWNER && quiesce && release_fn && ledger_count<MAX_REG);
    ledger_quiesce[ledger_count]=quiesce; ledger_release[ledger_count]=release_fn;
    ledger_context[ledger_count++]=context; return 0;
}
#endif

enum fault { NONE, BAD_SIG, BAD_TAG, BAD_RESIDUE, BAD_STATUS, BAD_COUNTS, SHORT_CSW, PHASE,
             CSW_STALL, CSW_STALL_TWICE, DATA_STALL, SHORT_DATA, CBW_STALL, CBW_ERROR, CBW_UNSUPPORTED, CBW_AGAIN };
struct host {
    struct usb_device dev;
    int gate, acquire_calls, release_calls, gate_busy, reset_fail;
    unsigned stage, toggle_in, toggle_out;
    uint8_t max_lun, max_stall, write_protected, mode_unsupported;
    uint8_t cache_page, cache6_unsupported, cache10_unsupported, cache_descriptors;
    uint8_t cache6_corrupt, cache10_corrupt, fault_page, sync16_unsupported;
    uint8_t all_pages, all_descriptors, header_dpofua, header_invalid, allpages_lun_fail;
    uint8_t pending_lun, opcode, cdb[16], cdb_length, direction;
    uint32_t tag, length, data_actual, partial_stall;
    const uint8_t *expected_write; uint32_t expected_write_length;
    uint64_t last_lba, blocks[16];
    uint32_t block_size[16], last_count;
    unsigned commands[256], lun_commands[16], resets, clears_in, clears_out;
    unsigned tur_not_ready, tur_asc, tur_key, sense_key, sense_asc, sense_ascq, sense_len, sense_format, fail_opcode;
    bool fail_once, fail_lun_match, malformed_sense, bad_sense_length, sense_fail; uint8_t fail_lun;
    enum fault fault;
    uint8_t fault_opcode, fault_lun;
    bool fault_used, fault_lun_match;
    char events[128]; unsigned event_count;
};
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p)<<32 | be32(p+4); }
static void put_le32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void put_be32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static void put_be64(uint8_t *p, uint64_t v) { put_be32(p,(uint32_t)(v>>32)); put_be32(p+4,(uint32_t)v); }
static void event(struct host *h, char c) { CHECK(h->event_count < sizeof(h->events)); h->events[h->event_count++]=c; }
static bool fault_for(struct host *h, enum fault f)
{ return !h->fault_used && h->fault == f && h->fault_opcode == h->opcode &&
    (!h->fault_lun_match || h->pending_lun==h->fault_lun) &&
    (!h->fault_page || (h->cdb[2]&0x3f)==h->fault_page); }
static int acquire(struct usb_device *d)
{
    struct host *h=d->host_context; ++h->acquire_calls;
    if (h->gate_busy || h->gate) return -EAGAIN;
    h->gate=1; return 0;
}
static void release(struct usb_device *d)
{ struct host *h=d->host_context; CHECK(h->gate==1); h->gate=0; ++h->release_calls; }
static int control(struct usb_device *d, const uint8_t s[8], void *data, uint16_t length, uint32_t *actual)
{
    struct host *h=d->host_context; CHECK(h->gate==1); CHECK(s[4]==3 && s[5]==0);
    CHECK(s[2]==0 && s[3]==0);
    *actual=0;
    if (s[1]==0xfe) {
        CHECK(s[0]==0xa1 && s[6]==1 && s[7]==0 && length==1 && data);
        if (h->max_stall) return -EPIPE;
        *(uint8_t *)data=h->max_lun; *actual=1; return 0;
    }
    CHECK(s[0]==0x21 && s[1]==0xff && !s[6] && !s[7] && !length);
    ++h->resets; event(h,'R'); if (h->reset_fail) return -EIO; h->stage=0; return 0;
}
static int clear_halt(struct usb_device *d, struct usb_endpoint *ep)
{
    struct host *h=d->host_context; CHECK(h->gate==1);
    if (ep->address==IN) { ++h->clears_in; h->toggle_in=0; event(h,'I'); }
    else { CHECK(ep->address==OUT); ++h->clears_out; h->toggle_out=0; event(h,'O'); }
    ep->toggle=0; return 0;
}
static void transferred(struct host *h, struct usb_endpoint *ep, uint32_t actual)
{
    unsigned *toggle=ep->address==IN ? &h->toggle_in : &h->toggle_out;
    /* Each fake transfer advances an odd number of packets, exposing lost state. */
    (void)actual; *toggle ^= 1u; ep->toggle=(uint8_t)*toggle;
}
static int bulk(struct usb_device *d, struct usb_endpoint *ep, void *data, uint32_t len, uint32_t *actual)
{
    struct host *h=d->host_context; uint8_t *p=data;
    CHECK(h->gate==1 && (ep->address==IN || ep->address==OUT));
    CHECK(ep->toggle==(ep->address==IN ? h->toggle_in : h->toggle_out));
    *actual=0;
    if (h->stage==0) {
        CHECK(ep->address==OUT && len==31 && le32(p)==0x43425355u);
        CHECK(p[12]==0 || p[12]==0x80); CHECK(p[13]<16 && p[14]>=1 && p[14]<=16);
        h->tag=le32(p+4); h->length=le32(p+8); h->direction=p[12];
        h->pending_lun=p[13]; h->cdb_length=p[14]; memcpy(h->cdb,p+15,16);
        h->opcode=p[15]; ++h->commands[h->opcode]; ++h->lun_commands[h->pending_lun];
        if(h->opcode==0x91) {
            CHECK(h->cdb_length==16 && !h->length && !h->direction);
            for(unsigned i=1;i<16;++i)CHECK(!h->cdb[i]);
        }
        if(h->allpages_lun_fail==3 && h->pending_lun==1 && h->opcode==0x1a &&
           (h->cdb[2]&0x3f)==0x3f && h->length>4)return -EIO;
        if (fault_for(h,CBW_STALL)) { h->fault_used=true; return -EPIPE; }
        if (fault_for(h,CBW_ERROR)) { h->fault_used=true; return -EIO; }
        if (fault_for(h,CBW_UNSUPPORTED)) { h->fault_used=true; return -EOPNOTSUPP; }
        if (fault_for(h,CBW_AGAIN)) { h->fault_used=true; return -EAGAIN; }
        if (h->opcode==0x28 || h->opcode==0x2a) {
            CHECK(h->cdb_length==10); h->last_lba=be32(h->cdb+2);
            h->last_count=(uint32_t)h->cdb[7]<<8 | h->cdb[8];
        } else if (h->opcode==0x88 || h->opcode==0x8a) {
            CHECK(h->cdb_length==16); h->last_lba=be64(h->cdb+2); h->last_count=be32(h->cdb+10);
        }
        h->stage=h->length ? 1 : 2; h->data_actual=0; *actual=31; transferred(h,ep,*actual); return 0;
    }
    if (h->stage==1) {
        CHECK(len==h->length && ep->address==(h->direction ? IN : OUT));
        h->stage=2;
        if (fault_for(h,DATA_STALL)) {
            h->fault_used=true; CHECK(h->partial_stall<=len);
            h->data_actual=h->partial_stall; *actual=h->partial_stall;
            if(*actual)transferred(h,ep,*actual);
            return -EPIPE;
        }
        uint32_t n=len;
        if (h->opcode==0x03 && h->sense_len && h->sense_len<n) n=h->sense_len;
        bool caching=(h->opcode==0x1a || h->opcode==0x5a) && (h->cdb[2]&0x3f)==8;
        bool allpages=h->opcode==0x1a && (h->cdb[2]&0x3f)==0x3f && len>4;
        unsigned header=h->opcode==0x1a ? 4u : 8u;
        unsigned corrupt=h->opcode==0x1a ? h->cache6_corrupt : h->cache10_corrupt;
        unsigned all_total=4u;
        if(caching) {
            CHECK(h->direction==0x80 && h->cdb[1]==8);
            CHECK(h->opcode==0x1a ? (h->cdb_length==6 && h->cdb[4]==len) :
                (h->cdb_length==10 && (((unsigned)h->cdb[7]<<8)|h->cdb[8])==len));
            unsigned size=header+(h->cache_page ? h->cache_descriptors+20u : 0u);
            if(n>size)n=size;
            if(corrupt==4 && n>header+h->cache_descriptors+2)n=header+h->cache_descriptors+2;
        }
        if(allpages) {
            CHECK(h->direction==0x80 && h->cdb[1]==8 && h->cdb_length==6 && h->cdb[4]==len);
            if(h->all_pages>=13)all_total=20;
            else if(h->all_pages)all_total+=h->all_descriptors+12u+6u+12u+
                (h->all_pages==3 ? 0u : 20u)+(h->all_pages==7 ? 20u : 0u);
            if(n>all_total)n=all_total;
            if(h->all_pages==6)--n;
        }
        if (fault_for(h,SHORT_DATA)) { h->fault_used=true; n=len-1; }
        if (fault_for(h,BAD_COUNTS)) n=len-1;
        if (ep->address==OUT && h->expected_write &&
            (h->opcode==0x2a || h->opcode==0x8a)) {
            CHECK(len==h->expected_write_length && !memcmp(p,h->expected_write,len));
        }
        if (ep->address==IN) {
            memset(p,0,n);
            if (h->opcode==0x12 && n>=36) { p[0]=0; p[1]=0x80; p[4]=31; memcpy(p+8,"RELIEF  USB DISK        ",24); }
            if (h->opcode==0x03 && h->malformed_sense && n>=4) {
                p[0]=0x55;
            } else if (h->opcode==0x03 && (h->sense_format==0x72 || h->sense_format==0x73) && n>=4) {
                p[0]=(uint8_t)h->sense_format; p[1]=(uint8_t)h->sense_key; p[2]=(uint8_t)h->sense_asc; p[3]=(uint8_t)h->sense_ascq;
            } else if (h->opcode==0x03 && n>=14) {
                p[0]=(uint8_t)(h->sense_format==0x71 ? 0x71 : 0x70);
                p[2]=(uint8_t)h->sense_key; p[7]=h->bad_sense_length ? 0 : 10; p[12]=(uint8_t)h->sense_asc; p[13]=(uint8_t)h->sense_ascq;
            }
            if (h->opcode==0x25 && n>=8) {
                uint64_t last=h->blocks[h->pending_lun]-1;
                put_be32(p,last>=UINT32_MAX ? UINT32_MAX : (uint32_t)last);
                put_be32(p+4,h->block_size[h->pending_lun]);
            }
            if (h->opcode==0x9e && n>=12) {
                CHECK(h->cdb[1]==0x10); put_be64(p,h->blocks[h->pending_lun]-1);
                put_be32(p+8,h->block_size[h->pending_lun]);
            }
            if (h->opcode==0x1a && n>=4) {
                p[0]=(uint8_t)(n-1); p[2]=(h->write_protected ? 0x80 : 0) | (h->header_dpofua ? 0x10 : 0);
                if(len==4 && h->header_invalid)p[0]=2;
            }
            if(allpages && n>=4) {
                p[0]=(uint8_t)(all_total-1);p[3]=h->all_descriptors;
                if(h->all_pages>=13) {
                    /* Invalid SPF/subpage 00, before or after a cache page. */
                    unsigned invalid=h->all_pages==13 ? 4 : 16;
                    unsigned cache=h->all_pages==13 ? 8 : 4;
                    p[invalid]=0x4a;p[invalid+1]=0;p[invalid+2]=0;p[invalid+3]=0;
                    p[cache]=8;p[cache+1]=10;p[cache+2]=0;
                } else if(h->all_pages) {
                    unsigned offset=4+h->all_descriptors;
                    p[offset]=1;p[offset+1]=h->all_pages==4 ? 255 : 10;offset+=12;
                    p[offset]=h->all_pages==10 ? 0x48 : 0x4a;p[offset+1]=1;
                    p[offset+2]=0;p[offset+3]=h->all_pages==11 ? 255 : 2;offset+=6;
                    if(h->all_pages!=3) {
                        p[offset]=h->all_pages==9 ? 0x48 : 8;
                        p[offset+1]=h->all_pages==9 ? 0 : h->all_pages==12 ? 9 : 18;
                        p[offset+2]=h->all_pages==2 ? 4 : 0;offset+=20;
                    }
                    p[offset]=0x0a;p[offset+1]=h->all_pages==5 ? 255 : 10;offset+=12;
                    if(h->all_pages==7) {p[offset]=8;p[offset+1]=18;}
                    if(h->all_pages==8)p[3]=7;
                }
            }
            if(caching && n>=header) {
                if(h->opcode==0x1a)p[3]=h->cache_descriptors;
                else {p[0]=(uint8_t)((n-2)>>8);p[1]=(uint8_t)(n-2);p[7]=h->cache_descriptors;}
                unsigned offset=header+h->cache_descriptors;
                if(h->cache_page && n>=offset+3) {
                    p[offset]=8;p[offset+1]=18;p[offset+2]=h->cache_page==2 ? 4 : 0;
                    if(corrupt==1)p[offset]=9;
                    if(corrupt==2)p[offset+1]=0;
                    if(corrupt==5)p[offset]=0x48; /* unsupported subpage format */
                }
                if(corrupt==3) {if(h->opcode==0x1a)p[0]=(uint8_t)(n+10);else p[1]=(uint8_t)(n+10);}
                if(corrupt==6) {if(h->opcode==0x1a)p[3]=7;else p[7]=7;}
            }
            if (h->opcode==0x28 || h->opcode==0x88) memset(p,0xa5,n);
        }
        h->data_actual=n; *actual=n; transferred(h,ep,n); return 0;
    }
    CHECK(h->stage==2 && ep->address==IN && len==13);
    if (fault_for(h,CSW_STALL) || fault_for(h,CSW_STALL_TWICE)) {
        if (h->fault==CSW_STALL) h->fault_used=true;
        else if (h->clears_in) h->fault_used=true;
        return -EPIPE;
    }
    uint32_t sig=0x53425355u, tag=h->tag, residue=h->length-h->data_actual;
    uint8_t status=0;
    if (h->opcode==0x03 && h->sense_fail) {
        status=1;
    } else if(h->allpages_lun_fail && h->allpages_lun_fail<3 && h->pending_lun==1 &&
              h->opcode==0x1a && (h->cdb[2]&0x3f)==0x3f && h->length>4) {
        status=1;h->sense_key=2;h->sense_asc=h->allpages_lun_fail==1 ? 0x3a : 0x04;h->sense_ascq=0;
    } else if (h->opcode==0 && h->tur_not_ready) {
        --h->tur_not_ready; h->sense_key=h->tur_key ? h->tur_key : 2;
        h->sense_asc=h->tur_asc ? h->tur_asc : 4; status=1;
    } else if ((h->fail_opcode && h->opcode==h->fail_opcode &&
                (!h->fail_lun_match || h->pending_lun==h->fail_lun)) ||
               (h->opcode==0x1a && h->mode_unsupported) ||
               (h->opcode==0x1a && (h->cdb[2]&0x3f)==8 && h->cache6_unsupported) ||
               (h->opcode==0x5a && h->cache10_unsupported) ||
               (h->opcode==0x91 && h->sync16_unsupported)) {
        status=1;
        if((h->opcode==0x1a && (h->mode_unsupported || h->cache6_unsupported)) ||
           (h->opcode==0x5a && h->cache10_unsupported) ||
           (h->opcode==0x91 && h->sync16_unsupported)) { h->sense_key=5; h->sense_asc=0x20; h->sense_ascq=0; }
        if(h->fail_once && h->opcode==h->fail_opcode) { h->fail_opcode=0; h->fail_once=false; }
    }
    if (fault_for(h,BAD_SIG)) { sig=0; h->fault_used=true; }
    if (fault_for(h,BAD_TAG)) { ++tag; h->fault_used=true; }
    if (fault_for(h,BAD_RESIDUE)) { residue=h->length+1; h->fault_used=true; }
    if (fault_for(h,BAD_COUNTS)) { residue=0; h->fault_used=true; }
    if (fault_for(h,BAD_STATUS)) { status=3; h->fault_used=true; }
    if (fault_for(h,PHASE)) { status=2; h->fault_used=true; }
    put_le32(p,sig); put_le32(p+4,tag); put_le32(p+8,residue); p[12]=status;
    *actual=13;
    if (fault_for(h,SHORT_CSW)) { *actual=12; h->fault_used=true; }
    transferred(h,ep,*actual); h->stage=0; return 0;
}
static const struct usb_device_operations ops={acquire,release,control,bulk,clear_halt};
static void init_host(struct host *h)
{
    memset(h,0,sizeof(*h));
    diagnostics[0]='\0';diagnostics_length=0;
    h->sync16_unsupported=1;
#ifndef REAL_REGISTRY
    memset(registered,0,sizeof(registered)); reg_count=0;
#endif
    h->dev=(struct usb_device){.operations=&ops,.host_context=h,.owner=OWNER,
                               .bus=1,.slot=2,.function=3,.root_port=4};
    for (unsigned i=0; i<16; ++i) { h->blocks[i]=4096; h->block_size[i]=512; }
}
static int probe(struct host *h, struct usb_storage **s)
{
    struct usb_endpoint in={.address=IN,.max_packet=64}, out={.address=OUT,.max_packet=64};
    CHECK(!h->gate); h->gate=1; int rc=usb_storage_probe(&h->dev,3,in,out,s); h->gate=0; return rc;
}
#ifndef REAL_REGISTRY
static void destroy(struct host *h, struct usb_storage *s)
{
    h->dev.disconnected=true; CHECK(usb_storage_disconnect(s)==0); usb_storage_release(s);
    for (unsigned i=0; i<reg_count; ++i) CHECK(!registered[i].registration);
    CHECK(h->acquire_calls == h->release_calls);
}
static const struct block_device_registration *reg(unsigned i)
{ CHECK(i<reg_count && registered[i].registration); return registered[i].registration; }
static void write_attempt_latch(void)
{
    struct host h;struct usb_storage *s=NULL;uint8_t bytes[512]={0};init_host(&h);
    h.max_lun=1;CHECK(probe(&h,&s)==0 && reg_count==2);
    const struct block_device_registration *r=reg(0),*other=reg(1);
    CHECK(r->operations->read(r->context,0,1,bytes)==0);
    CHECK(r->operations->write(r->context,0,0,bytes)==-EINVAL);
    h.gate_busy=1;CHECK(r->operations->write(r->context,0,1,bytes)==-EAGAIN);
    CHECK(r->operations->flush(r->context)==-EAGAIN);
    h.gate_busy=0;h.acquire_calls-=2; /* neither failed acquire owns the gate */
    CHECK(other->operations->write(other->context,0,1,bytes)==0);
    h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
    CHECK(r->operations->flush(r->context)==0 && !h.commands[0x35] && !h.commands[0x91]);
    CHECK(other->operations->flush(other->context)==-EOPNOTSUPP && h.commands[0x35]==1 && h.commands[0x91]==1);
    h.dev.disconnected=true;
    CHECK(r->operations->flush(r->context)==-ENODEV && h.commands[0x35]==1);
    destroy(&h,s);

    /* Failed writes count even when a partial data stage leaves progress uncertain. */
    for(unsigned partial=0;partial<2;++partial) {
        init_host(&h);s=NULL;CHECK(probe(&h,&s)==0);r=reg(0);
        h.fail_opcode=0x2a;h.sense_key=3;h.sense_asc=0;
        if(partial) {h.fault=DATA_STALL;h.fault_opcode=0x2a;h.partial_stall=256;}
        CHECK(r->operations->write(r->context,0,1,bytes)==-EIO && !registered[0].inactive);
        if(partial)CHECK(h.clears_out==1);
        h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
        CHECK(r->operations->flush(r->context)==-EOPNOTSUPP && h.commands[0x35]==1 && h.commands[0x91]==1);
        destroy(&h,s);
    }

    /* A successful synchronization never makes later flushes no-write. */
    init_host(&h);s=NULL;CHECK(probe(&h,&s)==0);r=reg(0);
    CHECK(r->operations->write(r->context,0,1,bytes)==0);
    CHECK(r->operations->flush(r->context)==0 && h.commands[0x35]==1);
    h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
    CHECK(r->operations->flush(r->context)==-EOPNOTSUPP && h.commands[0x35]==2 && h.commands[0x91]==1);
    destroy(&h,s);

    /* BOT reset of another LUN revokes the cache proof, not the write latch. */
    init_host(&h);s=NULL;h.max_lun=1;h.cache_page=1;
    CHECK(probe(&h,&s)==0 && reg_count==2);r=reg(0);other=reg(1);
    CHECK(r->operations->write(r->context,0,1,bytes)==0);
    h.fault=BAD_SIG;h.fault_opcode=0x28;h.fault_lun_match=true;h.fault_lun=1;
    CHECK(other->operations->read(other->context,0,1,bytes)==-EPROTO && h.resets==1);
    h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
    CHECK(r->operations->flush(r->context)==-EOPNOTSUPP && h.commands[0x35]==1 && h.commands[0x91]==1);
    destroy(&h,s);

    /* Ambiguous write completion revokes the LUN; disconnect wins on flush. */
    init_host(&h);s=NULL;CHECK(probe(&h,&s)==0);r=reg(0);
    h.fault=BAD_SIG;h.fault_opcode=0x2a;
    CHECK(r->operations->write(r->context,0,1,bytes)==-EPROTO && registered[0].inactive);
    CHECK(r->operations->flush(r->context)==-ENODEV && !h.commands[0x35]);
    destroy(&h,s);

    /* Retirement/replug creates a new LUN with no inherited write attempt. */
    init_host(&h);s=NULL;CHECK(probe(&h,&s)==0);r=reg(0);
    h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
    CHECK(r->operations->flush(r->context)==0 && !h.commands[0x35]);
    destroy(&h,s);
}
static void normal(void)
{
    struct host h; struct usb_storage *s=NULL; uint8_t data[1024]={0}; init_host(&h);
    h.max_lun=1; h.tur_not_ready=2;
    CHECK(probe(&h,&s)==0 && s && reg_count==2);
    CHECK(h.lun_commands[1]>0 && h.commands[3]>=2);
    const struct block_device_registration *r=reg(0), *r1=reg(1);
    CHECK(r->info.blocks==4096 && r->info.block_size==512 && r->info.max_blocks==128);
    CHECK(!strcmp(r->info.model,"RELIEF  USB DISK"));
    CHECK(r->info.bus==1 && r->info.slot==2 && r->info.function==3);
    CHECK(r->operations->read(r->context,17,2,data)==0 && data[0]==0xa5 && data[1023]==0xa5);
    CHECK(h.opcode==0x28 && h.last_lba==17 && h.last_count==2);
    h.expected_write=data; h.expected_write_length=sizeof(data);
    CHECK(r1->operations->write(r1->context,33,2,data)==0 && h.pending_lun==1);
    CHECK(h.opcode==0x2a && h.last_lba==33 && h.last_count==2);
    CHECK(r->operations->flush(r->context)==0 && !h.commands[0x35]);
    CHECK(r1->operations->flush(r1->context)==0 && h.opcode==0x35);
    destroy(&h,s);
}
static void cacheless_flush(void)
{
    struct host h;struct usb_storage *s=NULL;uint8_t data[512]={0};init_host(&h);
    h.cache_page=1;
    CHECK(probe(&h,&s)==0);const struct block_device_registration *r=reg(0);
    CHECK(r->operations->write(r->context,0,1,data)==0);
    h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
    CHECK(r->operations->flush(r->context)==0 && h.commands[0x35]==0);
    destroy(&h,s);
}
static void sync_cache16_fallback(void)
{
    struct host h;struct usb_storage *s=NULL;uint8_t bytes[512]={0};
    for(unsigned asc_case=0;asc_case<2;++asc_case) {
        init_host(&h);s=NULL;h.sync16_unsupported=0;
        CHECK(probe(&h,&s)==0);const struct block_device_registration *r=reg(0);
        CHECK(r->operations->write(r->context,0,1,bytes)==0);
        h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=asc_case ? 0x24 : 0x20;
        if(asc_case)h.sense_format=0x72; /* current descriptor sense */
        CHECK(r->operations->flush(r->context)==0 && h.commands[0x35]==1 && h.commands[0x91]==1);
        CHECK(strstr(diagnostics,asc_case ? "sync-cache lun=0 fallback=16 from=24/00 rc=0" :
            "sync-cache lun=0 fallback=16 from=20/00 rc=0"));
        destroy(&h,s);
    }
    /* Unrelated valid sense, undecodable sense and transport faults cannot
     * be reclassified as unsupported or trigger the alternate command. */
    for(unsigned kind=0;kind<8;++kind) {
        init_host(&h);s=NULL;h.sync16_unsupported=0;CHECK(probe(&h,&s)==0);
        const struct block_device_registration *r=reg(0);
        CHECK(r->operations->write(r->context,0,1,bytes)==0);
        h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
        if(kind==0)h.sense_asc=0x26;
        if(kind==1){h.sense_key=3;h.sense_asc=0;}
        if(kind==2){h.sense_key=6;h.sense_asc=0x29;}
        if(kind==3)h.sense_fail=true;
        if(kind==4)h.malformed_sense=true;
        if(kind==5){h.fault=CBW_ERROR;h.fault_opcode=0x35;}
        if(kind==6)h.sense_ascq=1;
        if(kind==7){h.fault=CBW_UNSUPPORTED;h.fault_opcode=0x35;}
        int rc=r->operations->flush(r->context);
        CHECK(rc==((kind==0 || kind==6 || kind==7) ? -EOPNOTSUPP : kind==2 ? -ENODEV : -EIO));
        CHECK(!h.commands[0x91]);destroy(&h,s);
    }
    init_host(&h);s=NULL;CHECK(probe(&h,&s)==0);
    CHECK(reg(0)->operations->write(reg(0)->context,0,1,bytes)==0);
    h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
    CHECK(reg(0)->operations->flush(reg(0)->context)==-EOPNOTSUPP && h.commands[0x91]==1);
    destroy(&h,s);
    init_host(&h);s=NULL;h.sync16_unsupported=0;CHECK(probe(&h,&s)==0);
    CHECK(reg(0)->operations->write(reg(0)->context,0,1,bytes)==0);
    h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;h.fault=CBW_ERROR;h.fault_opcode=0x91;
    CHECK(reg(0)->operations->flush(reg(0)->context)==-EIO && registered[0].inactive);
    CHECK(h.commands[0x91]==1);destroy(&h,s);
}
static void allpages_cache_discovery(void)
{
    for(unsigned kind=1;kind<=12;++kind) {
        struct host h;struct usb_storage *s=NULL;uint8_t bytes[512]={0};init_host(&h);
        h.cache6_unsupported=h.cache10_unsupported=1;h.all_pages=(uint8_t)kind;h.all_descriptors=8;
        CHECK(probe(&h,&s)==0 && h.commands[0x1a]==3 && h.commands[0x5a]==1);
        const struct block_device_registration *r=reg(0);
        CHECK(r->operations->write(r->context,0,1,bytes)==0);
        h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
        int rc=r->operations->flush(r->context);
        if(kind==1 || kind==10)CHECK(!rc && !h.commands[0x35] && !h.commands[0x91]);
        else CHECK(rc==-EOPNOTSUPP && h.commands[0x35]==1 && h.commands[0x91]==1);
        destroy(&h,s);
    }
}
static void allpages_invalid_subpage_zero(void)
{
    unsigned accepted=0;
    for(unsigned kind=13;kind<=14;++kind) {
        struct host h;struct usb_storage *s=NULL;uint8_t bytes[512]={0};init_host(&h);
        h.cache6_unsupported=h.cache10_unsupported=1;h.all_pages=(uint8_t)kind;
        CHECK(probe(&h,&s)==0 && h.commands[0x1a]==3 && h.commands[0x5a]==1);
        const struct block_device_registration *r=reg(0);
        CHECK(r->operations->write(r->context,0,1,bytes)==0);
        h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
        int rc=r->operations->flush(r->context);
        if(rc!=-EOPNOTSUPP || h.commands[0x35]!=1 || h.commands[0x91]!=1) {
            fprintf(stderr,"invalid unrelated SPF/subpage00 %s cache: rc=%d sync10=%u sync16=%u\n",
                kind==13 ? "before" : "after",rc,h.commands[0x35],h.commands[0x91]);
            ++accepted;
        }
        destroy(&h,s);
    }
    CHECK(!accepted);
}
static void cache_diagnostics(void)
{
    for(unsigned invalid=0;invalid<2;++invalid) {
        struct host h;struct usb_storage *s=NULL;uint8_t data[512]={0};init_host(&h);
        h.header_dpofua=1;h.header_invalid=(uint8_t)invalid;
        h.cache6_unsupported=h.cache10_unsupported=1;h.all_pages=1;
        CHECK(probe(&h,&s)==0);
        CHECK(strstr(diagnostics,invalid ? "valid=0 dpofua=1" : "valid=1 dpofua=1"));
        CHECK(strstr(diagnostics,"cache-query lun=0 sense=6 page=08 rc=-95 bytes=4 decoded=1"));
        CHECK(strstr(diagnostics,"cache-query lun=0 sense=10 page=08 rc=-95 bytes=8 decoded=1"));
        CHECK(strstr(diagnostics,"cache-query lun=0 sense=6 page=3f rc=0"));
        CHECK(reg(0)->operations->write(reg(0)->context,0,1,data)==0 && h.cdb[1]==0);
        destroy(&h,s);
    }
}
static void allpages_lun_unavailable(void)
{
    for(unsigned kind=1;kind<=3;++kind) {
        struct host h;struct usb_storage *s=NULL;uint8_t data[512]={0};init_host(&h);
        h.max_lun=1;h.cache6_unsupported=h.cache10_unsupported=1;
        h.all_pages=1;h.allpages_lun_fail=(uint8_t)kind;
        CHECK(probe(&h,&s)==(kind==3 ? -EIO : 0) && reg_count==1);
        CHECK(reg(0)->info.namespace_id==0);
        CHECK(reg(0)->operations->read(reg(0)->context,0,1,data)==0);
        if(kind==3) {
            CHECK(reg(0)->operations->write(reg(0)->context,0,1,data)==0);
            h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
            CHECK(reg(0)->operations->flush(reg(0)->context)==-EOPNOTSUPP && h.commands[0x91]==1);
        }
        destroy(&h,s);
    }
}
static void cache_modes_and_failures(void)
{
    struct host h;struct usb_storage *s=NULL;uint8_t bytes[512]={0};
    /* MODE SENSE(6) may be unsupported, and a device may return block
     * descriptors even when DBD is requested. */
    init_host(&h);h.cache_page=1;h.cache6_unsupported=1;h.cache_descriptors=8;
    CHECK(probe(&h,&s)==0 && h.commands[0x5a]);
    CHECK(reg(0)->operations->write(reg(0)->context,0,1,bytes)==0);
    CHECK(reg(0)->operations->flush(reg(0)->context)==0 && !h.commands[0x35]);destroy(&h,s);
    init_host(&h);s=NULL;h.cache_page=1;h.cache_descriptors=8;
    CHECK(probe(&h,&s)==0 && !h.commands[0x5a]);
    CHECK(reg(0)->operations->write(reg(0)->context,0,1,bytes)==0);
    CHECK(reg(0)->operations->flush(reg(0)->context)==0 && !h.commands[0x35]);destroy(&h,s);
    /* Write-back and unsupported/malformed page data never imply durability. */
    for(unsigned kind=0;kind<8;++kind) {
        init_host(&h);s=NULL;
        if(kind==0)h.cache_page=2;
        else if(kind==1)h.cache6_unsupported=h.cache10_unsupported=1;
        else {h.cache_page=1;h.cache6_corrupt=h.cache10_corrupt=(uint8_t)(kind-1);}
        CHECK(probe(&h,&s)==0);
        if(kind==0)CHECK(h.commands[0x1a]==2 && !h.commands[0x5a]);
        const struct block_device_registration *r=reg(0);
        CHECK(r->operations->write(r->context,0,1,bytes)==0);
        h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
        CHECK(r->operations->flush(r->context)==-EOPNOTSUPP && h.commands[0x35]==1);
        destroy(&h,s);
    }
    init_host(&h);s=NULL;h.write_protected=1;
    CHECK(probe(&h,&s)==0 && (reg(0)->info.flags&BLOCK_DEVICE_READ_ONLY));
    CHECK(reg(0)->operations->flush(reg(0)->context)==0 && !h.commands[0x35]);destroy(&h,s);
    /* Recovery of another LUN may reset mode parameters for the entire target. */
    init_host(&h);s=NULL;h.max_lun=1;h.cache_page=1;
    CHECK(probe(&h,&s)==0 && reg_count==2);
    CHECK(reg(0)->operations->write(reg(0)->context,0,1,bytes)==0);
    h.fault=BAD_SIG;h.fault_opcode=0x28;h.fault_lun_match=true;h.fault_lun=1;
    CHECK(reg(1)->operations->read(reg(1)->context,0,1,bytes)==-EPROTO && h.resets==1);
    h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
    CHECK(reg(0)->operations->flush(reg(0)->context)==-EOPNOTSUPP && h.commands[0x35]==1);
    destroy(&h,s);
    /* A reset UNIT ATTENTION on another LUN can also change shared mode pages. */
    init_host(&h);s=NULL;h.max_lun=1;h.cache_page=1;
    CHECK(probe(&h,&s)==0 && reg_count==2);
    h.fail_opcode=0x28;h.fail_lun_match=true;h.fail_lun=1;h.sense_key=6;h.sense_asc=0x29;
    CHECK(reg(1)->operations->read(reg(1)->context,0,1,bytes)==-ENODEV);
    h.fail_opcode=0x35;h.fail_lun_match=false;h.sense_key=5;h.sense_asc=0x20;
    CHECK(reg(0)->operations->write(reg(0)->context,0,1,bytes)==0);
    CHECK(reg(0)->operations->flush(reg(0)->context)==-EOPNOTSUPP && h.commands[0x35]==1);
    destroy(&h,s);
    init_host(&h);s=NULL;h.cache_page=1;h.fault=CBW_ERROR;h.fault_opcode=0x1a;h.fault_page=8;
    CHECK(probe(&h,&s)==-EIO && h.resets==1 && !reg_count);destroy(&h,s);
    init_host(&h);s=NULL;h.cache_page=1;CHECK(probe(&h,&s)==0);
    h.fail_opcode=0x2a;h.sense_key=6;h.sense_asc=0x2a;
    CHECK(reg(0)->operations->write(reg(0)->context,0,1,bytes)==-ENODEV);
    CHECK(reg(0)->operations->flush(reg(0)->context)==-ENODEV);destroy(&h,s);
}
static void undecodable_sense_invalidates_all_luns(void)
{
    unsigned stale=0;
    for(unsigned kind=0;kind<7;++kind) {
        struct host h;struct usb_storage *s=NULL;uint8_t bytes[512]={0};init_host(&h);
        h.max_lun=1;h.cache_page=1;
        CHECK(probe(&h,&s)==0 && reg_count==2);
        h.fail_opcode=0x28;h.fail_lun_match=true;h.fail_lun=1;h.fail_once=true;
        h.sense_key=6;h.sense_asc=0x29;
        if(kind==0)h.sense_fail=true;
        if(kind==1)h.malformed_sense=true;
        if(kind==2){h.sense_format=0x72;h.sense_len=4;}
        if(kind==3)h.sense_format=0x71; /* deferred fixed sense */
        if(kind==4)h.sense_format=0x73; /* deferred descriptor sense */
        if(kind==5)h.sense_key=0;
        if(kind==6)h.bad_sense_length=true;
        CHECK(reg(1)->operations->read(reg(1)->context,0,1,bytes)==-EIO);
        CHECK(registered[1].inactive && !registered[0].inactive);
        h.sense_fail=h.malformed_sense=h.bad_sense_length=false;
        h.sense_len=h.sense_format=0;h.fail_lun_match=false;
        h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
        CHECK(reg(0)->operations->write(reg(0)->context,0,1,bytes)==0);
        int rc=reg(0)->operations->flush(reg(0)->context);
        if(rc!=-EOPNOTSUPP || h.commands[0x35]!=1 || h.commands[0x91]!=1) {
            fprintf(stderr,"undecodable sense kind=%u allowed stale flush rc=%d sync=%u\n",kind,rc,h.commands[0x35]);
            ++stale;
        }
        destroy(&h,s);
    }
    /* The same rule applies when LUN1 fails during probing after LUN0 was
     * already published: LUN1 is not yet registered, LUN0 still exists. */
    for(unsigned kind=0;kind<2;++kind) {
        struct host h;struct usb_storage *s=NULL;uint8_t bytes[512]={0};init_host(&h);
        h.max_lun=1;h.cache_page=1;h.fail_opcode=0x1a;
        h.fail_lun_match=true;h.fail_lun=1;h.fail_once=true;h.sense_key=6;h.sense_asc=0x29;
        if(kind==0)h.sense_fail=true;else h.malformed_sense=true;
        CHECK(probe(&h,&s)==-EIO && s && reg_count==1 && !registered[0].inactive);
        h.sense_fail=h.malformed_sense=false;h.fail_lun_match=false;
        h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
        CHECK(reg(0)->operations->write(reg(0)->context,0,1,bytes)==0);
        int rc=reg(0)->operations->flush(reg(0)->context);
        if(rc!=-EOPNOTSUPP || h.commands[0x35]!=1 || h.commands[0x91]!=1) {
            fprintf(stderr,"probe sense kind=%u allowed stale flush rc=%d sync=%u\n",kind,rc,h.commands[0x35]);
            ++stale;
        }
        destroy(&h,s);
    }
    CHECK(!stale);
}
static void max_lun_stall(void)
{
    struct host h; struct usb_storage *s=NULL; init_host(&h); h.max_stall=1;
    CHECK(probe(&h,&s)==0 && reg_count==1); destroy(&h,s);
}
static void large_and_protected(void)
{
    struct host h; struct usb_storage *s=NULL; uint8_t data[512]; init_host(&h);
    h.blocks[0]=UINT64_C(0x100000200); h.write_protected=1;
    CHECK(probe(&h,&s)==0 && h.commands[0x9e] && reg(0)->info.blocks==h.blocks[0]);
    const struct block_device_registration *r=reg(0);
    CHECK(r->info.flags & BLOCK_DEVICE_READ_ONLY);
    CHECK(r->operations->read(r->context,UINT64_C(0x100000001),1,data)==0);
    CHECK(h.opcode==0x88 && h.last_lba==UINT64_C(0x100000001) && h.last_count==1);
    CHECK(r->operations->read(r->context,h.blocks[0],1,data)<0);
    CHECK(r->operations->write(r->context,0,1,data)==-EROFS);
    destroy(&h,s);
}
static void media_change_revokes_identity(void)
{
    for(unsigned kind=0;kind<3;++kind) {
        struct host h; struct usb_storage *s=NULL; uint8_t bytes[1024]={0}; init_host(&h);
        h.tur_not_ready=1; h.tur_key=6; h.tur_asc=0x28;
        CHECK(probe(&h,&s)==0 && h.commands[0]==2);
        const struct block_device_registration *r=reg(0);
        CHECK(r->info.blocks==4096); h.blocks[0]=1024;
        h.fail_opcode=0x2a; h.fail_once=true;
        h.sense_key=kind==2 ? 2 : 6; h.sense_asc=kind==0 ? 0x28 : kind==1 ? 0x2a : 0x3a;
        unsigned writes=h.commands[0x2a], reads=h.commands[0x28];
        CHECK(r->operations->write(r->context,0,2,bytes)==-ENODEV);
        CHECK(registered[0].inactive && h.commands[0x2a]==writes+1);
        CHECK(r->operations->write(r->context,0,2,bytes)==-ENODEV);
        CHECK(r->operations->read(r->context,0,1,bytes)==-ENODEV);
        CHECK(r->operations->flush(r->context)==-ENODEV);
        CHECK(h.commands[0x2a]==writes+1 && h.commands[0x28]==reads);
        destroy(&h,s);
    }
}
static void untrusted_completion_revokes_identity(void)
{
    for(unsigned kind=0;kind<12;++kind) {
        struct host h; struct usb_storage *s=NULL; uint8_t bytes[512]={0}; init_host(&h);
        CHECK(probe(&h,&s)==0); const struct block_device_registration *r=reg(0);
        if(kind==11)CHECK(r->operations->write(r->context,0,1,bytes)==0);
        h.blocks[0]=1024; h.fail_opcode=kind==10 ? 0x28 : kind==11 ? 0x35 : 0x2a; h.fail_once=true;
        h.sense_key=6; h.sense_asc=0x28;
        if(kind==0) { h.fault=BAD_SIG; h.fault_opcode=0x2a; }
        if(kind==1) { h.fault=BAD_SIG; h.fault_opcode=0x03; }
        if(kind==2) { h.fault=CBW_ERROR; h.fault_opcode=0x03; }
        if(kind==3) h.malformed_sense=true;
        if(kind==4) { h.fault=CBW_ERROR; h.fault_opcode=0x2a; }
        if(kind==5) { h.sense_key=0; h.sense_asc=0; }
        if(kind==6) h.bad_sense_length=true;
        if(kind==7) { h.sense_format=0x72; h.sense_len=4; }
        if(kind==8) h.sense_format=0x71;
        if(kind==9) h.sense_fail=true;
        if(kind==10) { h.fault=BAD_SIG; h.fault_opcode=0x28; }
        if(kind==11) h.malformed_sense=true;
        int rc=kind==10 ? r->operations->read(r->context,0,1,bytes) :
               kind==11 ? r->operations->flush(r->context) : r->operations->write(r->context,0,1,bytes);
        CHECK(rc==(kind<2 || kind==10 ? -EPROTO : -EIO));
        CHECK(registered[0].inactive);
        unsigned writes=h.commands[0x2a];
        CHECK(r->operations->write(r->context,0,1,bytes)==-ENODEV);
        CHECK(h.commands[0x2a]==writes && r->info.blocks==4096);
        destroy(&h,s);
    }
}
static void large_writable_boundaries(void)
{
    struct host h; struct usb_storage *s=NULL; uint8_t data[1536]={0}; init_host(&h);
    h.blocks[0]=UINT64_C(0x100000200);
    CHECK(probe(&h,&s)==0); const struct block_device_registration *r=reg(0);
    CHECK(r->operations->read(r->context,UINT64_C(0xfffffffe),2,data)==0);
    CHECK(h.opcode==0x28 && h.last_lba==UINT64_C(0xfffffffe));
    CHECK(r->operations->read(r->context,UINT64_C(0xfffffffe),3,data)==0);
    CHECK(h.opcode==0x88 && h.last_lba==UINT64_C(0xfffffffe) && h.last_count==3);
    CHECK(r->operations->write(r->context,UINT64_C(0x100000001),1,data)==0);
    CHECK(h.opcode==0x8a && h.last_lba==UINT64_C(0x100000001));
    uint8_t *maximum=malloc(65536); CHECK(maximum);
    CHECK(r->operations->read(r->context,0,128,maximum)==0 && maximum[65535]==0xa5);
    CHECK(h.last_count==128); free(maximum);
    unsigned commands=h.commands[0x88]+h.commands[0x28];
    CHECK(r->operations->read(r->context,UINT64_MAX,1,data)==-EINVAL);
    CHECK(r->operations->read(r->context,0,0,data)==-EINVAL);
    CHECK(h.commands[0x88]+h.commands[0x28]==commands);
    destroy(&h,s);
}
static void unavailable_media(void)
{
    struct host h; struct usb_storage *s=NULL; init_host(&h);
    h.tur_not_ready=10; h.tur_asc=0x3a;
    CHECK(probe(&h,&s)==-ENOMEDIUM && reg_count==0 && h.commands[0]==4 && h.commands[3]==4);
    destroy(&h,s);
    init_host(&h); s=NULL; h.max_lun=1; h.tur_not_ready=4; h.tur_asc=0x3a;
    CHECK(probe(&h,&s)==0 && reg_count==1 && reg(0)->info.namespace_id==1);
    destroy(&h,s);
    init_host(&h); s=NULL; h.max_lun=16;
    CHECK(probe(&h,&s)==-EPROTO && s && !reg_count);
    destroy(&h,s);
    init_host(&h); s=NULL; h.max_lun=15;
    CHECK(probe(&h,&s)==0 && reg_count==16 && reg(15)->info.namespace_id==15);
    destroy(&h,s);
    init_host(&h); s=NULL; h.blocks[0]=0;
    CHECK(probe(&h,&s)==-EOVERFLOW && s && !reg_count);
    destroy(&h,s);
}
static void malformed(enum fault fault)
{
    struct host h; struct usb_storage *s=NULL; uint8_t data[512]; init_host(&h);
    CHECK(probe(&h,&s)==0); h.fault=fault; h.fault_opcode=0x28;
    const struct block_device_registration *r=reg(0); unsigned start=h.event_count;
    CHECK(r->operations->read(r->context,0,1,data)<0);
    CHECK(h.resets==1 && h.event_count==start+3);
    CHECK(!memcmp(h.events+start,"RIO",3));
    destroy(&h,s);
}
static void mode_sense_failures_not_ignored(void)
{
    struct host h; struct usb_storage *s=NULL; init_host(&h);
    h.fault=CBW_ERROR; h.fault_opcode=0x1a; h.reset_fail=1;
    CHECK(probe(&h,&s)==-EIO && s && !reg_count && h.resets==1);
    destroy(&h,s);
    init_host(&h); s=NULL; h.fault=CBW_UNSUPPORTED; h.fault_opcode=0x1a;
    CHECK(probe(&h,&s)==-EOPNOTSUPP && s && !reg_count);
    destroy(&h,s);
    init_host(&h); s=NULL; h.max_lun=1; h.fault=CBW_UNSUPPORTED; h.fault_opcode=0x1a;
    h.fault_lun_match=true; h.fault_lun=1;
    CHECK(probe(&h,&s)==-EOPNOTSUPP && s && reg_count==1);
    destroy(&h,s);
    init_host(&h); s=NULL; h.max_lun=1; h.fault=CBW_UNSUPPORTED; h.fault_opcode=0x1a;
    h.fault_lun_match=true; h.fault_lun=1; h.reset_fail=1;
    CHECK(probe(&h,&s)==-EIO && s && reg_count==1);
    destroy(&h,s);
    init_host(&h); s=NULL; h.fail_opcode=0x1a; h.sense_key=3; h.sense_asc=0;
    CHECK(probe(&h,&s)==-EIO && s && !reg_count);
    destroy(&h,s);
    init_host(&h); s=NULL; h.mode_unsupported=1; h.fault=CBW_ERROR; h.fault_opcode=0x03;
    CHECK(probe(&h,&s)==-EIO && s && !reg_count);
    destroy(&h,s);
    init_host(&h); s=NULL; h.mode_unsupported=1;
    CHECK(probe(&h,&s)==0 && reg_count==1);
    destroy(&h,s);
}
static void mode_sense_lun_disappears(void)
{
    for(unsigned kind=0;kind<3;++kind) {
        struct host h; struct usb_storage *s=NULL; uint8_t bytes[512]; init_host(&h);
        h.max_lun=1; h.fail_opcode=0x1a; h.fail_lun_match=true; h.fail_lun=1;
        h.fail_once=true; h.sense_key=kind==2 ? 6 : 2;
        h.sense_asc=kind==0 ? 0x3a : kind==1 ? 0x04 : 0x28;
        CHECK(probe(&h,&s)==0 && s && reg_count==1);
        CHECK(reg(0)->info.namespace_id==0);
        CHECK(reg(0)->operations->read(reg(0)->context,0,1,bytes)==0 && bytes[0]==0xa5);
        destroy(&h,s);
    }
}
static void partial_write_not_resumable(void)
{
    struct host h; struct usb_storage *s=NULL; uint8_t data[1024]={0}; init_host(&h);
    CHECK(probe(&h,&s)==0); const struct block_device_registration *r=reg(0);
    h.gate_busy=1; unsigned writes=h.commands[0x2a];
    CHECK(r->operations->write(r->context,0,2,data)==-EAGAIN && h.commands[0x2a]==writes && !registered[0].inactive);
    h.gate_busy=0; --h.acquire_calls; /* the failed acquire owns no gate */
    h.fault=DATA_STALL; h.fault_opcode=0x2a; h.partial_stall=512;
    h.fail_opcode=0x2a; h.sense_key=2; h.sense_asc=4;
    CHECK(r->operations->write(r->context,0,2,data)==-EIO);
    CHECK(h.commands[0x2a]==writes+1 && h.clears_out==1);
    h.fail_opcode=0; h.fault=CBW_AGAIN; h.fault_used=false;
    CHECK(r->operations->write(r->context,0,2,data)==-EIO);
    CHECK(h.commands[0x2a]==writes+2 && h.resets==1);
    destroy(&h,s);
}
static void failed_recovery_blocks_further_commands(void)
{
    struct host h; struct usb_storage *s=NULL; uint8_t data[512]; init_host(&h);
    CHECK(probe(&h,&s)==0); const struct block_device_registration *r=reg(0);
    h.fault=BAD_SIG; h.fault_opcode=0x28; h.reset_fail=1;
    CHECK(r->operations->read(r->context,0,1,data)<0);
    CHECK(h.event_count>=3 && !memcmp(h.events+h.event_count-3,"RIO",3));
    unsigned commands=h.commands[0x28]; h.reset_fail=0;
    CHECK(registered[0].inactive && r->operations->read(r->context,0,1,data)==-ENODEV);
    CHECK(h.commands[0x28]==commands);
    destroy(&h,s);
}
static void stalls(void)
{
    struct host h; struct usb_storage *s=NULL; uint8_t data[512]; init_host(&h);
    CHECK(probe(&h,&s)==0); const struct block_device_registration *r=reg(0);
    h.fault=CSW_STALL; h.fault_opcode=0x28;
    CHECK(r->operations->read(r->context,1,1,data)==0 && h.clears_in==1 && h.resets==0);
    h.fault=DATA_STALL; h.fault_opcode=0x2a; h.fault_used=false;
    h.fail_opcode=0x2a; h.sense_key=7; h.sense_asc=0x27;
    unsigned writes=h.commands[0x2a];
    CHECK(r->operations->write(r->context,1,1,data)==-EROFS && h.clears_out==1);
    CHECK(h.commands[0x2a]==writes+1);
    h.fault=DATA_STALL; h.fault_opcode=0x28; h.fault_used=false;
    h.fail_opcode=0x28; h.sense_key=2; h.sense_asc=0x04;
    CHECK(r->operations->read(r->context,1,1,data)==-EAGAIN && h.clears_in==2);
    h.fail_opcode=0;
    h.fault=SHORT_DATA; h.fault_opcode=0x2a; h.fault_used=false; writes=h.commands[0x2a];
    CHECK(r->operations->write(r->context,1,1,data)<0 && h.commands[0x2a]==writes+1);
    h.fault=CSW_STALL_TWICE; h.fault_opcode=0x28; h.fault_used=false; h.clears_in=0;
    unsigned start=h.event_count;
    CHECK(r->operations->read(r->context,1,1,data)<0 && h.resets==1);
    CHECK(h.event_count==start+4 && !memcmp(h.events+start,"IRIO",4));
    destroy(&h,s);
}
static void errors_and_disconnect(void)
{
    struct host h; struct usb_storage *s=NULL; uint8_t data[512]; init_host(&h);
    h.mode_unsupported=1; CHECK(probe(&h,&s)==0); const struct block_device_registration *r=reg(0);
    h.sense_key=7; h.sense_asc=0x27; h.sense_len=14; h.fail_opcode=0x2a;
    CHECK(r->operations->write(r->context,0,1,data)==-EROFS);
    h.sense_format=0x72; h.sense_len=8;
    CHECK(r->operations->write(r->context,0,1,data)==-EROFS);
    h.sense_key=5; h.sense_asc=0x21;
    CHECK(r->operations->write(r->context,0,1,data)==-EINVAL);
    h.sense_asc=0x20; h.fail_opcode=0x35;
    CHECK(r->operations->flush(r->context)==-EOPNOTSUPP);
    h.fail_opcode=0; h.gate_busy=1;
    CHECK(r->operations->read(r->context,0,1,data)==-EAGAIN); h.gate_busy=0;
    CHECK(h.acquire_calls==h.release_calls+1); --h.acquire_calls;
    h.dev.disconnected=true; registered[0].busy=1;
    CHECK(usb_storage_disconnect(s)==-EBUSY && registered[0].inactive);
    CHECK(r->operations->read(r->context,0,1,data)==-ENODEV);
    CHECK(r->operations->write(r->context,0,1,data)==-ENODEV);
    CHECK(r->operations->flush(r->context)==-ENODEV);
    registered[0].busy=0; CHECK(usb_storage_disconnect(s)==0); usb_storage_release(s);
    CHECK(h.acquire_calls==h.release_calls);
}
static void capacity_and_short(void)
{
    struct host h; struct usb_storage *s=NULL; uint8_t data[512]; init_host(&h); h.block_size[0]=4096;
    CHECK(probe(&h,&s)==-EOPNOTSUPP && s && reg_count==0); destroy(&h,s);
    init_host(&h); s=NULL; CHECK(probe(&h,&s)==0);
    h.fault=SHORT_DATA; h.fault_opcode=0x28;
    CHECK(reg(0)->operations->read(reg(0)->context,0,1,data)<0);
    destroy(&h,s);
}
#endif

int main(void)
{
    CHECK(usb_storage_probe && usb_storage_disconnect && usb_storage_release);
#ifndef REAL_REGISTRY
    write_attempt_latch(); cacheless_flush(); sync_cache16_fallback(); allpages_cache_discovery(); allpages_invalid_subpage_zero(); cache_diagnostics(); allpages_lun_unavailable(); cache_modes_and_failures(); undecodable_sense_invalidates_all_luns(); normal(); max_lun_stall(); large_and_protected(); media_change_revokes_identity(); untrusted_completion_revokes_identity(); large_writable_boundaries(); unavailable_media();
    malformed(BAD_SIG); malformed(BAD_TAG); malformed(BAD_RESIDUE); malformed(BAD_STATUS);
    malformed(BAD_COUNTS); malformed(SHORT_CSW); malformed(PHASE); malformed(CBW_STALL);
    stalls(); partial_write_not_resumable(); mode_sense_failures_not_ignored(); mode_sense_lun_disappears(); failed_recovery_blocks_further_commands(); errors_and_disconnect(); capacity_and_short();
#else
    struct host h; struct usb_storage *s=NULL; init_host(&h); h.write_protected=1;
    CHECK(probe(&h,&s)==0 && ledger_count==1);
    struct block_device_ref ref={0}; uint64_t cursor=0; uint8_t bytes[512];
    CHECK(block_device_next(&cursor,&ref)==0 && module_refs==1);
    struct block_device_info info={0}; CHECK(block_device_info(&ref,&info)==0);
    CHECK(info.transport==5 && (info.flags & BLOCK_DEVICE_REMOVABLE) && (info.flags & BLOCK_DEVICE_READ_ONLY));
    CHECK(block_device_read(&ref,1,1,bytes)==0 && bytes[0]==0xa5);
    CHECK(block_device_write(&ref,1,1,bytes)==-EROFS);
    CHECK(block_device_flush(&ref)==0 && !h.commands[0x35]);
    h.dev.disconnected=true; CHECK(usb_storage_disconnect(s)==-EBUSY);
    struct block_device_ref other={0}; CHECK(block_device_get(info.device,&other)==-ENODEV);
    struct block_device_info retained={0}; CHECK(block_device_info(&ref,&retained)==0 && retained.blocks==info.blocks);
    CHECK(block_device_read(&ref,1,1,bytes)==-ENODEV);
    CHECK(block_device_write(&ref,1,1,bytes)==-ENODEV);
    CHECK(block_device_flush(&ref)==-ENODEV);
    block_device_put(&ref); CHECK(!module_refs && !executions);
    CHECK(usb_storage_disconnect(s)==0); usb_storage_release(s);
    /* The owner ledger still holds its core entry after class metadata is freed. */
    CHECK(ledger_quiesce[0](ledger_context[0])==0);
    ledger_release[0](ledger_context[0]);
    CHECK(h.acquire_calls==h.release_calls);

    init_host(&h); s=NULL; cursor=0;
    CHECK(probe(&h,&s)==0 && ledger_count==2);
    CHECK(block_device_next(&cursor,&ref)==0 && block_device_info(&ref,&info)==0);
    h.blocks[0]=1024; h.fail_opcode=0x2a; h.fail_once=true; h.sense_key=6; h.sense_asc=0x28;
    unsigned writes=h.commands[0x2a];
    CHECK(block_device_write(&ref,0,1,bytes)==-ENODEV);
    CHECK(block_device_get(info.device,&other)==-ENODEV);
    CHECK(block_device_info(&ref,&retained)==0 && retained.blocks==4096);
    CHECK(block_device_write(&ref,0,1,bytes)==-ENODEV);
    CHECK(block_device_read(&ref,0,1,bytes)==-ENODEV);
    CHECK(block_device_flush(&ref)==-ENODEV && h.commands[0x2a]==writes+1);
    h.dev.disconnected=true; CHECK(usb_storage_disconnect(s)==-EBUSY);
    block_device_put(&ref); CHECK(!module_refs && !executions);
    CHECK(usb_storage_disconnect(s)==0); usb_storage_release(s);
    CHECK(ledger_quiesce[1](ledger_context[1])==0); ledger_release[1](ledger_context[1]);
    CHECK(h.acquire_calls==h.release_calls);

    for(unsigned kind=0;kind<2;++kind) {
        unsigned ledger=ledger_count; init_host(&h); s=NULL; cursor=0;
        CHECK(probe(&h,&s)==0 && ledger_count==ledger+1);
        CHECK(block_device_next(&cursor,&ref)==0 && block_device_info(&ref,&info)==0);
        h.blocks[0]=1024; h.fail_opcode=0x2a; h.fail_once=true; h.sense_key=6; h.sense_asc=0x28;
        h.fault=BAD_SIG; h.fault_opcode=kind ? 0x03 : 0x2a;
        CHECK(block_device_write(&ref,0,1,bytes)==-EPROTO && h.resets==1);
        CHECK(block_device_get(info.device,&other)==-ENODEV);
        CHECK(block_device_info(&ref,&retained)==0 && retained.blocks==4096);
        unsigned attempts=h.commands[0x2a];
        CHECK(block_device_write(&ref,0,1,bytes)==-ENODEV && h.commands[0x2a]==attempts);
        h.dev.disconnected=true; CHECK(usb_storage_disconnect(s)==-EBUSY);
        block_device_put(&ref); CHECK(!module_refs && !executions);
        CHECK(usb_storage_disconnect(s)==0); usb_storage_release(s);
        CHECK(ledger_quiesce[ledger](ledger_context[ledger])==0);
        ledger_release[ledger](ledger_context[ledger]);
        CHECK(h.acquire_calls==h.release_calls);
    }
    {
        unsigned ledger=ledger_count;init_host(&h);s=NULL;cursor=0;h.cache_page=1;
        CHECK(probe(&h,&s)==0 && ledger_count==ledger+1);
        CHECK(block_device_next(&cursor,&ref)==0 && block_device_info(&ref,&info)==0);
        CHECK(block_device_write(&ref,0,1,bytes)==0);
        h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
        CHECK(block_device_flush(&ref)==0 && !h.commands[0x35]);
        h.dev.disconnected=true;CHECK(usb_storage_disconnect(s)==-EBUSY);
        CHECK(block_device_flush(&ref)==-ENODEV);block_device_put(&ref);
        CHECK(usb_storage_disconnect(s)==0);usb_storage_release(s);
        CHECK(ledger_quiesce[ledger](ledger_context[ledger])==0);ledger_release[ledger](ledger_context[ledger]);
        CHECK(h.acquire_calls==h.release_calls);
    }
    {
        unsigned ledger=ledger_count;init_host(&h);s=NULL;cursor=0;
        CHECK(probe(&h,&s)==0 && ledger_count==ledger+1);
        CHECK(block_device_next(&cursor,&ref)==0 && block_device_info(&ref,&info)==0);
        CHECK(!(info.flags&BLOCK_DEVICE_READ_ONLY));
        CHECK(block_device_read(&ref,0,1,bytes)==0);
        h.fail_opcode=0x35;h.sense_key=5;h.sense_asc=0x20;
        CHECK(block_device_flush(&ref)==0 && !h.commands[0x35] && !h.commands[0x91]);
        CHECK(block_device_write(&ref,0,1,bytes)==0);
        CHECK(block_device_flush(&ref)==-EOPNOTSUPP && h.commands[0x35]==1 && h.commands[0x91]==1);
        h.dev.disconnected=true;CHECK(usb_storage_disconnect(s)==-EBUSY);
        CHECK(block_device_flush(&ref)==-ENODEV);block_device_put(&ref);
        CHECK(usb_storage_disconnect(s)==0);usb_storage_release(s);
        CHECK(ledger_quiesce[ledger](ledger_context[ledger])==0);ledger_release[ledger](ledger_context[ledger]);
        CHECK(h.acquire_calls==h.release_calls);
    }
#endif
    puts("USB storage scripted tests passed"); return 0;
}
