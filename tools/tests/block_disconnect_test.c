/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <reliefnt/blkdev.h>
#include <reliefnt/module.h>
#include <reliefnt/lock.h>
#include <linux/errno.h>

static unsigned allocations,callbacks;
static const struct block_device_registration *during_registration;
static int (*quiesce)(void *);
static void (*release)(void *);
static void *resource;
void *kernel_malloc(size_t bytes) {void *p=calloc(1,bytes);if(p)++allocations;return p;}
void kernel_free(void *p) {if(p){assert(allocations);--allocations;free(p);}}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{assert(!lock->state);lock->state=1;*flags=0;}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{assert(lock->state && !flags);lock->state=0;}
bool try_module_get(struct module *owner) {(void)owner;return true;}
void module_put(struct module *owner) {(void)owner;}
bool module_execution_get(struct module *owner) {(void)owner;return true;}
void module_execution_put(struct module *owner) {(void)owner;}
int module_add_resource(struct module *owner,int (*stop)(void *),void (*free_resource)(void *),void *context)
{
    assert(owner && !resource);quiesce=stop;release=free_resource;resource=context;
    assert(during_registration && !disconnect_block_device(during_registration));return 0;
}
static int read_blocks(void *context,uint64_t first,uint32_t count,void *bytes)
{(void)context;(void)first;(void)count;(void)bytes;++callbacks;return 0;}
int main(void)
{
    const struct block_device_operations ops={.read=read_blocks};
    struct block_device_registration registration={.info={.device=0x10300,.blocks=64,
        .block_size=512,.max_blocks=4,.flags=BLOCK_DEVICE_READ_ONLY},.operations=&ops};
    struct block_device_ref held={0},other={0};uint8_t bytes[512];
    assert(!register_block_device(&registration));
    assert(!block_device_get(registration.info.device,&held));
    assert(!disconnect_block_device(&registration));
    assert(block_device_get(registration.info.device,&other)==-ENODEV);
    assert(block_device_read(&held,0,1,bytes)==-ENODEV);
    assert(block_device_write(&held,0,1,bytes)==-ENODEV);
    assert(block_device_flush(&held)==-ENODEV && !callbacks);
    struct block_device_info info;
    assert(!block_device_info(&held,&info) && info.blocks==64);
    assert(unregister_block_device(&registration)==-EBUSY);
    block_device_put(&held);assert(!unregister_block_device(&registration));
    assert(!allocations);
    struct module owner={0};registration.owner=&owner;during_registration=&registration;
    assert(register_block_device(&registration)==-ENODEV);
    assert(block_device_get(registration.info.device,&other)==-ENODEV);
    assert(unregister_block_device(&registration)==-ENOENT);
    assert(resource && !quiesce(resource));release(resource);
    assert(!allocations);
    puts("PASS physical block removal revokes readonly IO/flush, retains held metadata and wins publication race");
}
