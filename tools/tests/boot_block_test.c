/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <reliefnt/boot_block.h>
#include <reliefnt/blkdev.h>
#include <reliefnt/multiboot2.h>
#include "../../kernel/module/internal.h"

static unsigned char *source;
static const uint64_t physical=0x18000000,source_bytes=1536ULL*1024*1024;
static size_t allocations;
static bool reject_mapping,fail_heap;
void *kernel_malloc(size_t bytes)
{assert(bytes<4096);if(fail_heap)return NULL;void *p=calloc(1,bytes);if(p)++allocations;return p;}
void kernel_free(void *p){if(p){assert(allocations);--allocations;free(p);}}
void module_memory_free(struct module_memory *memory){assert(!memory);}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{assert(!lock->state);lock->state=1;*flags=0;}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{assert(lock->state && !flags);lock->state=0;}
bool paging_kernel_direct_map_range(uint64_t start,uint64_t length)
{return !reject_mapping && start==physical && length==source_bytes;}
void *paging_kernel_direct_map(uint64_t start)
{assert(start==physical);return reject_mapping ? NULL : source;}

int main(void)
{
    /* A 1.5 GiB virtual payload has only two accessible pages. Registration
     * must neither read nor copy the body; ASan/guard pages catch an attempt. */
    source=mmap(NULL,source_bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
    assert(source!=MAP_FAILED);
    assert(!mprotect(source,4096,PROT_READ|PROT_WRITE));
    assert(!mprotect(source+source_bytes-4096,4096,PROT_READ|PROT_WRITE));
    memset(source,0x42,4096);memset(source+source_bytes-4096,0x71,4096);
    uint64_t device=9;assert(!block_boot_image_register(NULL,&device) && !device && !allocations);
    struct boot_info boot={.module_count=1,.modules={{physical,physical+source_bytes,"initramfs"}}};
    assert(!block_boot_image_register(&boot,&device) && !device);
    boot.module_count=17;assert(block_boot_image_register(&boot,&device)==-22 && !device);
    boot.module_count=2;boot.modules[0].name="reliefos-installer-root";
    boot.modules[1]=(struct boot_module){physical,physical+source_bytes,"leonos-installer-root"};
    assert(block_boot_image_register(&boot,&device)==-17 && !device && !allocations);
    boot.module_count=1;boot.modules[0].end=physical;
    assert(block_boot_image_register(&boot,&device)==-8 && !device);
    boot.modules[0].end=physical+source_bytes;
    reject_mapping=true;assert(block_boot_image_register(&boot,&device)==-22 && !device);reject_mapping=false;
    fail_heap=true;assert(block_boot_image_register(&boot,&device)==-12 && !device && !allocations);fail_heap=false;
    assert(block_boot_image_register(&boot,&device)==1 && device==0x100 && allocations==1);
    assert(block_boot_image_register(&boot,&device)==1 && device==0x100 && allocations==1);
    struct block_device_ref ref={0};assert(!block_device_get(device,&ref));
    struct block_device_info info;assert(!block_device_info(&ref,&info));
    assert(info.device==0x100 && info.blocks==source_bytes/512 && info.block_size==512 &&
        info.transport==BLOCK_TRANSPORT_MEMORY && (info.flags&BLOCK_DEVICE_WRITE_THROUGH));
    unsigned char data[1024];assert(!block_device_read(&ref,0,2,data));
    for(unsigned i=0;i<sizeof(data);++i)assert(data[i]==0x42);
    memset(data,0xa5,512);
    assert(!block_device_write(&ref,info.blocks-1,1,data) && source[source_bytes-512]==0xa5);
    assert(source[source_bytes-513]==0x71 && source[4095]==0x42);
    memset(data,0,sizeof(data));assert(!block_device_read(&ref,info.blocks-1,1,data));
    for(unsigned i=0;i<512;++i)assert(data[i]==0xa5);
    assert(block_device_read(&ref,info.blocks-1,2,data)==-22);
    assert(block_device_write(&ref,UINT64_MAX,1,data)==-22);
    assert(!block_device_flush(&ref));block_device_put(&ref);
    uint64_t other=9;assert(block_boot_image_bind(physical+4096,source_bytes,&other)==-16 && !other);
    assert(!block_boot_image_bind(physical,source_bytes,&other) && other==device && allocations==1);
    puts("PASS reserved 1.5GiB boot image uses one small registration, no payload copy, independent Linux ram dev_t and bounded coherent block I/O");
}
