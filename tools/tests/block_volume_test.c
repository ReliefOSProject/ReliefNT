/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_core_api.h"
#include <reliefnt/module.h>
#include <assert.h>
#include <reliefnt/console.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
void console_log(enum console_log_level level,const char *format,...) {(void)level;(void)format;}
static unsigned allocations,reads,writes,cancels;
static int pending;
static unsigned char media[8192];
struct storage_volume g_volumes[STORAGE_MAX_VOLUMES];
struct storage_volume *g_active_volume=&g_volumes[0];
static struct install_disk_state fixture_disks[STORAGE_INSTALL_DISK_INITIAL_CAPACITY];
struct install_disk_state *g_install_disks=fixture_disks;
uint32_t g_install_disk_count=1;
struct kernel_spinlock storage_transport_lock=KERNEL_SPINLOCK_INIT;
void *kernel_malloc(size_t bytes){void *p=calloc(1,bytes);if(p)++allocations;return p;}
void kernel_free(void *p){if(p){assert(allocations);--allocations;free(p);}}
void kernel_spin_lock(struct kernel_spinlock *l){assert(!l->state);l->state=1;}
void kernel_spin_unlock(struct kernel_spinlock *l){assert(l->state);l->state=0;}
void kernel_spin_lock_irqsave(struct kernel_spinlock *l,uint64_t *f){*f=0;kernel_spin_lock(l);}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *l,uint64_t f){assert(!f);kernel_spin_unlock(l);}
bool try_module_get(struct module *m){assert(!m);return true;}
void module_put(struct module *m){assert(!m);}
bool module_execution_get(struct module *m){assert(!m);return true;}
void module_execution_put(struct module *m){assert(!m);}
int module_add_resource(struct module *m,int(*stop)(void*),void(*release)(void*),void *c)
{(void)m;(void)stop;(void)release;(void)c;abort();}
void storage_memzero(void *p,size_t n){memset(p,0,n);}
void storage_memcpy(void *p,const void *q,size_t n){memcpy(p,q,n);}
void storage_copy_text(char *p,uint32_t n,const char *q){snprintf(p,n,"%s",q);}
void storage_filesystem_cache_event(const struct storage_volume *v,uint64_t l,uint32_t n)
{(void)v;(void)l;(void)n;assert(!storage_transport_lock.state);}
static int rd(void *c,uint64_t l,uint32_t n,void *p)
{assert(c==media && storage_transport_lock.state);++reads;if(pending){pending=0;return -11;}memcpy(p,media+l*512,n*512);return 0;}
static int wr(void *c,uint64_t l,uint32_t n,const void *p)
{assert(c==media && storage_transport_lock.state);++writes;memcpy(media+l*512,p,n*512);return 0;}
static int flush(void *c){assert(c==media);return 0;}
static void cancel(void *c,uint32_t pid){assert(c==media && pid==7);++cancels;}
static const struct block_device_operations operations={.read=rd,.write=wr,.flush=flush,.cancel=cancel};
int main(void)
{
    struct block_device_registration r={.info={.device=block_device_allocate_number(false),.blocks=16,
      .block_size=512,.max_blocks=2,.transport=STORAGE_TRANSPORT_NVME,.bus=5,.namespace_id=9},
      .operations=&operations,.context=media};
    strcpy(r.info.model,"owned transport");assert(!register_block_device(&r));
    struct install_disk_state *d=&g_install_disks[0];d->present=true;d->device=r.info.device;
    struct storage_volume v={0};assert(!storage_volume_from_install_disk(&v,d));
    assert(v.device_ref.device && v.transport==STORAGE_TRANSPORT_NVME && v.source_disk_id==0);
    assert(!strcmp(v.device_model,"owned transport") && v.bus==5 && v.nvme_nsid==9);
    assert(unregister_block_device(&r)==-16);
    assert(storage_volume_from_install_disk(&v,d)==-16);
    unsigned char input[2048],output[2048];memset(input,0x5a,sizeof(input));
    assert(!storage_write_device(&v,2,4,input) && writes==2);
    pending=1;assert(storage_read_device(&v,2,2,output)==-11);
    assert(!storage_read_device(&v,2,4,output) && reads==3 && !memcmp(input,output,2048));
    assert(storage_read_device(&v,15,2,output)==-22);
    block_device_cancel_all(7);assert(cancels==1);
    struct storage_volume second={0};assert(!storage_volume_from_install_disk(&second,d));
    storage_volume_put_device(&v);assert(!v.device_ref.device && unregister_block_device(&r)==-16);
    storage_volume_put_device(&second);assert(!unregister_block_device(&r));assert(!allocations);
    assert(storage_volume_from_install_disk(&v,d)==-19 && !v.device_ref.device);
    puts("PASS real volume binding: independent noncopyable refs, chunked I/O, EAGAIN, bounds, cancellation and exact cleanup");
}
