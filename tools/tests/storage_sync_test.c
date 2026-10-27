#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../block/core/storage_internal.h"
#include "../../block/core/storage_state_data.c"
#include "../../block/device/registry.c"
#include "../../drivers/storage/ide/storage_ide.c"
static unsigned commands,last_command,transport_calls;
static int transport_result;
static unsigned char port_status=0x40;
void *kernel_malloc(size_t n){return malloc(n);}
void kernel_free(void *p){free(p);}
void storage_memzero(void *p,size_t n){memset(p,0,n);}
uint8_t x86_64_inb(uint16_t port){(void)port;return port_status;}
void x86_64_outb(uint8_t value,uint16_t port)
{if((port&7)==7){++commands;last_command=value;}}
void kernel_execution_lock_irqsave(uint64_t *flags){*flags=0;}
void kernel_execution_unlock_irqrestore(uint64_t flags){(void)flags;}
void kernel_spin_lock(struct kernel_spinlock *lock){assert(!lock->state);lock->state=1;}
void kernel_spin_unlock(struct kernel_spinlock *lock){assert(lock->state);lock->state=0;}
void kernel_spin_lock_irqsave(struct kernel_spinlock *l,uint64_t *f){*f=0;kernel_spin_lock(l);}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *l,uint64_t f){(void)f;kernel_spin_unlock(l);}
#include "../../block/core/storage_sync.c"
static int unreachable_read(void *c,uint64_t first,uint32_t count,void *buffer)
{(void)c;(void)first;(void)count;(void)buffer;abort();}
static int unreachable_write(void *c,uint64_t first,uint32_t count,const void *buffer)
{(void)c;(void)first;(void)count;(void)buffer;abort();}
static int ide_sync(void *context)
{assert(!storage_io_async_context);return ide_flush_cache(context);}
static int fault_sync(void *context)
{assert(context && !storage_io_async_context && storage_transport_lock.state);++transport_calls;return transport_result;}
int main(void)
{
    struct ide_device_info ide={.present=1,.command_base=0x1f0,.control_base=0x3f6};
    const struct block_device_operations ide_ops={.read=unreachable_read,.write=unreachable_write,.flush=ide_sync};
    const struct block_device_operations fault_ops={.read=unreachable_read,.write=unreachable_write,.flush=fault_sync};
    const struct block_device_registration devices[]={
        {.info={.device=0x801,.blocks=8,.block_size=512,.max_blocks=8},.operations=&ide_ops,.context=&ide},
        {.info={.device=0x802,.blocks=8,.block_size=512,.max_blocks=8},.operations=&fault_ops,.context=&ide}};
    for(unsigned i=0;i<2;++i)assert(!register_block_device(&devices[i]));
    storage_io_async_context=true;
    g_volumes[0]=(struct storage_volume){.ready=true,.kind=STORAGE_VOLUME_IDE,.transport=STORAGE_TRANSPORT_IDE_PIO};
    assert(!block_device_get(0x801,&g_volumes[0].device_ref));
    assert(storage_sync_volume(0)==0 && commands==1 && last_command==0xe7 && storage_io_async_context);
    port_status=0x41;assert(storage_sync_volume(0)==-5 && commands==1 && storage_io_async_context);
    block_device_put(&g_volumes[0].device_ref);assert(!block_device_get(0x802,&g_volumes[0].device_ref));
    transport_result=-5;assert(storage_sync_volume(0)==-5 && transport_calls==1 && storage_io_async_context);
    transport_result=-110;assert(storage_sync_volume(0)==-110 && transport_calls==2);
    assert(storage_sync_volume(1)==-19 && storage_sync_volume(STORAGE_MAX_VOLUMES)==-19);
    g_volumes[1]=(struct storage_volume){.ready=true,.kind=STORAGE_VOLUME_NVME};
    assert(!block_device_get(0x802,&g_volumes[1].device_ref));
    assert(storage_sync_all()==-110 && transport_calls==4);
    for(unsigned i=0;i<2;++i){block_device_put(&g_volumes[i].device_ref);assert(!unregister_block_device(&devices[i]));}
    puts("PASS real registered sync: actual ATA command, errors, all-volume attempts, separate references and async restoration");
}
