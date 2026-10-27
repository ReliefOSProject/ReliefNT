/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/blkdev.h>
#include "../../kernel/module/internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct module owner;
static size_t allocations;
static int budget=-1;
static unsigned reads,writes,flushes,stops,releases,exits;
static int read_result,flush_result;
static bool stop_ready=true;
static unsigned char media[4096];

void *kernel_malloc(size_t bytes)
{
    if(!budget)return NULL;if(budget>0)--budget;
    void *result=calloc(1,bytes);if(result)++allocations;return result;
}
void kernel_free(void *pointer) {if(pointer){assert(allocations);--allocations;free(pointer);}}
void module_memory_free(struct module_memory *memory) {assert(!memory);}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{assert(!lock->state);lock->state=1;*flags=0;}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{assert(lock->state && !flags);lock->state=0;}
static int read_blocks(void *context,uint64_t first,uint32_t count,void *output);
static int write_blocks(void *context,uint64_t first,uint32_t count,const void *input);
static int flush_blocks(void *context);
static int read_optical(void *context,uint64_t first,uint32_t count,void *output)
{assert(context==media);memcpy(output,media+first*2048,count*2048);return 0;}
static const struct block_device_operations operations={.read=read_blocks,.write=write_blocks,.flush=flush_blocks};
static const struct block_device_operations optical_operations={.read=read_optical};
static const struct block_device_registration registration={.owner=&owner,
    .info={.device=0x801,.blocks=8,.block_size=512,.max_blocks=4},.operations=&operations,.context=media};
static const struct block_device_registration builtin={
    .info={.device=0xb00,.blocks=2,.block_size=2048,.max_blocks=2,
           .flags=BLOCK_DEVICE_READ_ONLY|BLOCK_DEVICE_OPTICAL},.operations=&optical_operations,.context=media};

static int read_blocks(void *context,uint64_t first,uint32_t count,void *output)
{
    assert(context==media && owner.runtime->users && owner.runtime->executing);
    assert(module_unload("test_block",0)==-EWOULDBLOCK);
    /* Real registration from inside I/O proves the registry lock is not held. */
    assert(!register_block_device(&builtin));assert(!unregister_block_device(&builtin));
    ++reads;if(read_result)return read_result;
    memcpy(output,media+first*512,count*512);return 0;
}
static int write_blocks(void *context,uint64_t first,uint32_t count,const void *input)
{assert(context==media && owner.runtime->executing);++writes;memcpy(media+first*512,input,count*512);return 0;}
static int flush_blocks(void *context)
{assert(context==media && owner.runtime->executing);++flushes;return flush_result;}
static int stop_controller(void *context)
{assert(context==media && !owner.runtime->users);++stops;return stop_ready ? 0 : -EIO;}
static void release_controller(void *context) {assert(context==media && stop_ready);++releases;}
static void exit_owner(void) {assert(stop_ready);++exits;}
static int init_owner(void) {return 0;}
static struct module_runtime *start_owner(void)
{
    struct module_runtime *runtime=kernel_malloc(sizeof(*runtime));assert(runtime);
    strcpy(runtime->name,"test_block");strcpy(owner.name,"test_block");
    owner.init=init_owner;owner.exit=exit_owner;owner.runtime=runtime;
    runtime->owner=&owner;runtime->state=MODULE_COMING;module_list=runtime;
    return runtime;
}

int main(void)
{
    uint64_t disk_number=block_device_allocate_number(false);
    uint64_t another_number=block_device_allocate_number(false);
    uint64_t optical_number=block_device_allocate_number(true);
    assert(disk_number && another_number && disk_number!=another_number);
    assert(optical_number==((uint64_t)11<<8));
    assert((disk_number&0xfff00u)==((uint64_t)259<<8));
    struct block_device_ref ref={0},second={0};struct block_device_info info;
    unsigned char bytes[2048];memset(media,0x42,sizeof(media));
    assert(block_device_get(0x801,&ref)==-ENODEV && !ref.device);
    assert(register_block_device(NULL)==-EINVAL);
    struct block_device_registration invalid=registration;invalid.info.block_size=513;
    assert(register_block_device(&invalid)==-EINVAL);
    invalid=registration;invalid.info.max_blocks=0;assert(register_block_device(&invalid)==-EINVAL);
    invalid=registration;invalid.info.flags=0x80000000u;assert(register_block_device(&invalid)==-EINVAL);
    struct module_runtime *runtime=start_owner();
    assert(!module_add_resource(&owner,stop_controller,release_controller,media));
    assert(!register_block_device(&registration) && !runtime->users && !runtime->executing);
    assert(register_block_device(&registration)==-EBUSY);
    assert(block_device_get(0x801,&ref)==-ENODEV && !ref.device);
    uint64_t cursor=0;assert(block_device_next(&cursor,&ref)==-ENOENT && !cursor);
    runtime->state=MODULE_LIVE;runtime->init_succeeded=true;
    assert(!block_device_get(0x801,&ref) && runtime->users==1);
    assert(block_device_get(0x801,&ref)==-EBUSY && runtime->users==1);
    assert(!block_device_info(&ref,&info) && info.blocks==8 && info.block_size==512);
    assert(unregister_block_device(&registration)==-EBUSY);
    assert(module_unload("test_block",0)==-EWOULDBLOCK);
    assert(!block_device_next(&cursor,&second) && cursor && runtime->users==2);
    block_device_put(&second);assert(block_device_next(&cursor,&second)==-ENOENT);
    assert(block_device_read(&ref,8,1,bytes)==-EINVAL);
    assert(block_device_read(&ref,7,2,bytes)==-EINVAL);
    assert(block_device_read(&ref,0,5,bytes)==-EINVAL);
    assert(block_device_read(&ref,0,0,bytes)==-EINVAL);
    assert(block_device_read(&ref,UINT64_MAX,2,bytes)==-EINVAL);
    assert(block_device_read(&ref,0,1,NULL)==-EINVAL && !reads);
    assert(!block_device_read(&ref,0,4,bytes) && reads==1 && !runtime->executing);
    for(unsigned i=0;i<sizeof(bytes);++i)assert(bytes[i]==0x42);
    memset(bytes,0x73,512);assert(!block_device_write(&ref,2,1,bytes) && writes==1);
    assert(media[1023]==0x42 && media[1024]==0x73 && media[1535]==0x73 && media[1536]==0x42);
    read_result=-EAGAIN;assert(block_device_read(&ref,0,1,bytes)==-EAGAIN && !runtime->executing);read_result=0;
    assert(!block_device_flush(&ref) && flushes==1);
    flush_result=-EIO;assert(block_device_flush(&ref)==-EIO && !runtime->executing);flush_result=0;
    block_device_put(&ref);block_device_put(&ref);assert(!runtime->users);
    stop_ready=false;assert(module_unload("test_block",0)==-EBUSY && stops==1 && !releases && !exits && allocations);
    assert(block_device_get(0x801,&ref)==-ENODEV);
    stop_ready=true;assert(!module_unload("test_block",0) && releases==1 && exits==1 && stops==2 && !allocations);
    assert(block_device_get(0x801,&ref)==-ENODEV);
    assert(!register_block_device(&builtin));assert(!block_device_get(0xb00,&ref));
    assert(!block_device_read(&ref,0,1,bytes) && !memcmp(bytes,media,2048));
    assert(!block_device_flush(&ref));
    assert(block_device_write(&ref,0,1,bytes)==-EROFS);
    assert(unregister_block_device(&builtin)==-EBUSY);
    block_device_put(&ref);assert(!unregister_block_device(&builtin) && !allocations);
    assert(unregister_block_device(&builtin)==-ENOENT);
    for(int failure=0;;++failure) {
        runtime=start_owner();budget=failure;int result=register_block_device(&registration);budget=-1;
        runtime->state=MODULE_GOING;assert(!runtime->users);
        if(!result){assert(!module_unload("test_block",0) && !allocations);break;}
        assert(result==-ENOMEM && !runtime->resources);
        assert(block_device_get(0x801,&ref)==-ENODEV);
        assert(!module_unload("test_block",0) && !allocations && failure<10);
    }
    puts("PASS block registry: actual module/I/O pins, COMING, bounds, read-only, callbacks unlocked, STOP retry and OOM cleanup");
}
