/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/partition/internal.h"
#include "../../block/partition/mbr.h"
#include <reliefnt/module.h>
#include <reliefnt/sched.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEDIA_BYTES (512u * 1024u)
static uint8_t media[2][MEDIA_BYTES], expected[MEDIA_BYTES], user[MEDIA_BYTES];
static struct block_device_registration registrations[2];
static struct disk_block_partition_cache partitions[2];
static unsigned reads,writes,read_sectors,write_sectors,allocations;
static unsigned fail_read,fail_write,pending_read,cancels;
static uint32_t pid=7;
static void *pending_buffer;
static uint64_t pending_first;
static uint32_t pending_count;
static unsigned pending_disk;
static bool nested_probe;

uint32_t sched_current_pid(void) {return pid;}
struct task *sched_current_task(void) {return NULL;}
uint64_t time_uptime_ms(void) {return 0;}
void console_log(enum console_log_level level,const char *format,...)
{(void)level;(void)format;}
void *kernel_malloc(size_t size) {void *p=malloc(size);if(p)++allocations;return p;}
void kernel_free(void *p) {if(p){assert(allocations);--allocations;free(p);}}
void kernel_spin_lock(struct kernel_spinlock *lock) {assert(!lock->state);lock->state=1;}
void kernel_spin_unlock(struct kernel_spinlock *lock) {assert(lock->state);lock->state=0;}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{*flags=0;kernel_spin_lock(lock);}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{assert(!flags);kernel_spin_unlock(lock);}
int storage_refresh_disks(void) {return 0;}
int storage_install_identify(struct install_disk_state *disk,uint64_t *sectors)
{(void)disk;(void)sectors;abort();}
int disk_gpt_entries_valid(const struct disk_gpt_table *table) {(void)table;abort();}
int mbr_read_partitions(int (*read)(void *,uint64_t,uint8_t[512]),void *context,uint64_t sectors,
                         struct mbr_partition out[MBR_PARTITION_CAPACITY])
{(void)read;(void)context;(void)sectors;(void)out;abort();}
int storage_optical_read(const struct block_device_ref *ref,uint64_t offset,void *buffer,
                         uint32_t length,uint32_t *done)
{(void)ref;(void)offset;(void)buffer;(void)length;(void)done;abort();}
int storage_disk_block_get(uint32_t disk,struct block_device_ref *ref)
{return disk<2 ? block_device_get(registrations[disk].info.device,ref) : -19;}

/* Only inventory and GPT discovery are held fixed. The real raw wrapper,
 * transaction admission, volume binding, installer helpers, transfer splitting
 * and block registry run in separate units above the sector-accurate provider. */

static int provider_read(void *context,uint64_t first,uint32_t count,void *buffer)
{
    unsigned disk=(unsigned)(uintptr_t)context;
    assert(disk<2 && first+count<=MEDIA_BYTES/512 && count<=registrations[disk].info.max_blocks);
    assert((uintptr_t)buffer<(uintptr_t)user || (uintptr_t)buffer>=(uintptr_t)user+sizeof(user));
    assert(storage_transport_lock.state);
    ++reads;read_sectors+=count;
    if(nested_probe) {
        nested_probe=false;uint32_t done=99;uint8_t byte=0xa5;pid=8;
        /* A competing transaction, even on another disk, must not change the
         * shared staging bytes or issue transport I/O while this PID owns it. */
        assert(storage_disk_block_read(1,-1,0,&byte,1,&done)==-11 && !done && byte==0xa5);
        pid=7;
    }
    if(reads==fail_read)return -5;
    if(pending_read && !--pending_read) {
        pending_buffer=buffer;pending_first=first;pending_count=count;pending_disk=disk;return -11;
    }
    if(pending_buffer) {
        assert(buffer==pending_buffer && first==pending_first && count==pending_count && disk==pending_disk);
        pending_buffer=NULL;
    }
    memcpy(buffer,media[disk]+first*512,count*512);return 0;
}
static int provider_write(void *context,uint64_t first,uint32_t count,const void *buffer)
{
    unsigned disk=(unsigned)(uintptr_t)context;
    assert(disk<2 && first+count<=MEDIA_BYTES/512 && count<=registrations[disk].info.max_blocks);
    assert(storage_transport_lock.state && !storage_async_can_yield());
    ++writes;write_sectors+=count;
    if(writes==fail_write) {
        /* The transport may alter media before returning an error. */
        memcpy(media[disk]+first*512,buffer,512);return -5;
    }
    memcpy(media[disk]+first*512,buffer,count*512);return 0;
}
static void provider_cancel(void *context,uint32_t owner)
{(void)context;assert(owner==7);++cancels;pending_buffer=NULL;}
static const struct block_device_operations operations={
    .read=provider_read,.write=provider_write,.cancel=provider_cancel};

static void reset_counters(void)
{
    reads=writes=read_sectors=write_sectors=fail_read=fail_write=pending_read=0;
    assert(!pending_buffer);
    storage_release_task_io(7);storage_set_io_async_context(false);
}
static void init_media(void)
{
    for(unsigned d=0;d<2;++d)
        for(unsigned i=0;i<MEDIA_BYTES;++i)media[d][i]=(uint8_t)(i*13u+i/511u+d*71u);
    for(unsigned i=0;i<MEDIA_BYTES;++i)user[i]=(uint8_t)(i*37u+i/733u);
}
static void write_case(uint32_t limit,uint64_t offset,uint32_t size)
{
    registrations[0].info.max_blocks=limit;reset_counters();init_media();
    memcpy(expected,media[0],MEDIA_BYTES);
    uint32_t valid=size<MEDIA_BYTES-offset ? size : (uint32_t)(MEDIA_BYTES-offset);
    memcpy(expected+offset,user,valid);
    uint32_t done=99;
    assert(!storage_disk_block_write(0,-1,offset,user,size,&done) && done==valid);
    assert(!memcmp(expected,media[0],MEDIA_BYTES));
    unsigned partial=((offset%512)!=0)+(((offset+valid)%512)!=0);
    if(offset/512==(offset+valid-1)/512 && partial==2)partial=1;
    assert(reads==partial && read_sectors==partial);
    assert(write_sectors==(offset%512+valid+511)/512);
    if(limit==128 && offset==0 && size==131072)assert(writes==2);
    unsigned write_count=writes;
    reset_counters();memset(user,0xcc,MEDIA_BYTES);
    assert(!storage_disk_block_read(0,-1,offset,user,size,&done) && done==valid);
    assert(!memcmp(user,expected+offset,valid));
    for(unsigned i=valid;i<MEDIA_BYTES;++i)assert(user[i]==0xcc);
    assert(reads==write_count && read_sectors==(offset%512+valid+511)/512);
}

int main(int argc,char **argv)
{
    (void)argv;
    for(unsigned d=0;d<2;++d) {
        registrations[d]=(struct block_device_registration){.info={
            .device=block_device_make_number(259,d*256),.blocks=MEDIA_BYTES/512,
            .block_size=512,.max_blocks=128,.flags=BLOCK_DEVICE_WRITE_THROUGH},
            .operations=&operations,.context=(void *)(uintptr_t)d};
        assert(!register_block_device(&registrations[d]));
        g_install_disks[d]=(struct install_disk_state){.present=true,
            .device=registrations[d].info.device,.sector_count=MEDIA_BYTES/512,
            .block_cache=&partitions[d]};
    }
    g_install_disk_count=2;
    if(argc>1) {
        uint32_t done;init_media();reset_counters();
        assert(!storage_disk_block_read(0,-1,0,user,131072,&done) && done==131072);
        unsigned aligned_reads=reads;
        reset_counters();
        assert(!storage_disk_block_write(0,-1,0,user,131072,&done) && done==131072);
        unsigned aligned_writes=writes;
        reset_counters();
        assert(!storage_disk_block_write(0,-1,1,user,131072,&done) && done==131072);
        printf("{\"bytes\":131072,\"native_max_sectors\":128,\"aligned_read_requests\":%u,"
               "\"aligned_write_requests\":%u,\"unaligned_write_requests\":%u,"
               "\"unaligned_preread_requests\":%u,\"unaligned_preread_sectors\":%u}\n",
               aligned_reads,aligned_writes,writes,reads,read_sectors);
        goto cleanup;
    }
    static const uint32_t limits[]={1,2,7,8,64,128,256};
    static const uint32_t offsets[]={0,1,511,512,777,4095};
    static const uint32_t lengths[]={1,2,511,512,513,4095,4096,65535,65536,131072};
    for(unsigned a=0;a<sizeof(limits)/sizeof(limits[0]);++a)
        for(unsigned b=0;b<sizeof(offsets)/sizeof(offsets[0]);++b)
            for(unsigned c=0;c<sizeof(lengths)/sizeof(lengths[0]);++c)
                write_case(limits[a],offsets[b],lengths[c]);
    write_case(128,MEDIA_BYTES-777,4096);

    reset_counters();init_media();registrations[0].info.max_blocks=128;
    uint32_t done=99;
    assert(!storage_disk_block_read(0,-1,MEDIA_BYTES,user,7,&done) && !done && !reads);
    assert(storage_disk_block_write(0,-1,MEDIA_BYTES,user,7,&done)==-28 && !done && !writes);
    assert(!storage_disk_block_read(0,-1,UINT64_MAX,user,7,&done) && !done);
    assert(storage_disk_block_write(0,-1,UINT64_MAX,user,7,&done)==-28 && !done);
    assert(storage_disk_block_read(0,-1,0,NULL,7,&done)==-22 && !done);
    assert(!storage_disk_block_write(0,-1,0,NULL,0,&done) && !done);

    /* Cached partition extents remain valid for partition writes; every byte
     * outside that extent is guarded by the independent expected image. */
    reset_counters();init_media();memcpy(expected,media[0],MEDIA_BYTES);
    partitions[0]=(struct disk_block_partition_cache){.valid=1,.disk_sectors=MEDIA_BYTES/512};
    partitions[0].entries[3]=(struct disk_block_partition_range){.present=1,.first_lba=9,.sector_count=257};
    memcpy(expected+9*512+511,user,65536);
    assert(!storage_disk_block_write(0,3,511,user,65536,&done) && done==65536 && partitions[0].valid);
    assert(!memcmp(expected,media[0],MEDIA_BYTES));
    reset_counters();fail_read=2;
    assert(storage_disk_block_read(0,-1,0,user,131072,&done)==-5 && done==65536 && reads==2);
    reset_counters();fail_write=2;
    assert(storage_disk_block_write(0,-1,0,user,131072,&done)==-5 && done==65536 && writes==2);
    assert(!partitions[0].valid);
    reset_counters();fail_read=1;memcpy(expected,media[0],MEDIA_BYTES);
    assert(storage_disk_block_write(0,-1,1,user,65536,&done)==-5 && !done && !writes);
    assert(!memcmp(expected,media[0],MEDIA_BYTES));
    reset_counters();fail_read=2;
    assert(storage_disk_block_write(0,-1,1,user,513,&done)==-5 && !done && !writes);
    assert(!memcmp(expected,media[0],MEDIA_BYTES));
    reset_counters();registrations[0].info.flags|=BLOCK_DEVICE_READ_ONLY;
    assert(storage_disk_block_write(0,-1,0,user,65536,&done)==-30 && !done && !writes);
    assert(!memcmp(expected,media[0],MEDIA_BYTES));
    registrations[0].info.flags&=~BLOCK_DEVICE_READ_ONLY;

    reset_counters();storage_set_io_async_context(true);pending_read=1;nested_probe=true;
    memset(user,0xa5,MEDIA_BYTES);
    assert(storage_disk_block_read(0,-1,0,user,65536,&done)==-11 && !done && storage_task_io_owner==7);
    assert(user[0]==0xa5 && user[65535]==0xa5);
    assert(!storage_disk_block_read(0,-1,0,user,65536,&done) && done==65536 && !pending_buffer);
    assert(!memcmp(user,media[0],65536));
    storage_drain_task_io(7);assert(cancels==2 && !storage_task_io_owner);
    reset_counters();storage_set_io_async_context(true);pending_read=2;
    memset(user,0xa5,MEDIA_BYTES);
    assert(storage_disk_block_read(0,-1,0,user,131072,&done)==-11 && done==65536);
    assert(!memcmp(user,media[0],65536) && user[65536]==0xa5 && reads==2);
    /* syscall_regular_io saves completed bytes before returning EAGAIN and
     * resumes at exactly that offset with the remaining current CPU buffer. */
    uint32_t rest=99;
    assert(!storage_disk_block_read(0,-1,done,user+done,131072-done,&rest) && rest==65536);
    assert(!memcmp(user,media[0],131072) && !pending_buffer && reads==3);
    storage_drain_task_io(7);
    reset_counters();storage_set_io_async_context(true);pending_read=1;
    assert(storage_disk_block_read(0,-1,0,user,65536,&done)==-11 && !done);
    storage_drain_task_io(7);assert(!pending_buffer && !storage_task_io_owner);
    assert(!storage_disk_block_read(1,-1,0,user,65536,&done) && !memcmp(user,media[1],65536));
    storage_release_task_io(7);
cleanup:
    for(unsigned d=0;d<2;++d)assert(!unregister_block_device(&registrations[d]));
    assert(!allocations);
    if(argc==1)puts("PASS raw disk: 421 geometry/content cases; 128KiB aligned read/write uses 2 requests; partial writes read only boundary sectors; native caps, EOF, partition guards, errors, EAGAIN identity, owner exclusion and cancellation");
}
