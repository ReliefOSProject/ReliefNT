/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/swap_internal.h>
#include <reliefnt/swap.h>
#include "../../mm/swap/internal.h"
/* Keep the real admission function while retaining this fixture's cache hook. */
#define storage_filesystem_cache_event_device unused_fixture_cache_event_device
#include "../../block/core/storage_state.c"
#undef storage_filesystem_cache_event_device

struct swap_backend;
extern int swap_backend_create(struct swap_backend **,struct block_device_ref *,uint64_t,uint64_t)
    __attribute__((weak));
extern int swap_backend_add_extent(struct swap_backend *,uint64_t,uint64_t,uint64_t)
    __attribute__((weak));
extern void swap_backend_destroy(struct swap_backend *) __attribute__((weak));
extern int swap_backend_page_io(struct swap_area *,uint32_t,uint64_t,bool) __attribute__((weak));
static unsigned calls, freed, transfers, live, execution, transport_inside, cache_events;static int injected;
static uint32_t fixture_pid=3;
static bool test_nested_admission;
extern int storage_acquire_task_io(void);
static uint8_t disk[64*512];
uint32_t storage_task_io_owner;
bool storage_io_async_context,storage_io_write_started;
struct kernel_spinlock storage_transport_lock;
/** @brief Allocate zeroed fixture metadata. @param n Size. @return Memory. */
void *kernel_malloc(size_t n) { void *p=calloc(1,n);if(p)++live;return p; }
/** @brief Release fixture metadata. @param p Owned memory. */
void kernel_free(void *p) { if(p){assert(live);--live;free(p);} }
/** @brief Give the current task identity. @return Fixture PID. */
uint32_t sched_current_pid(void) { return fixture_pid; }
bool kernel_execution_is_outermost(void) { return execution==1; }
/** @brief Enter the execution transaction. @param flags Saved flags. */
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags=execution++; }
/** @brief Leave the execution transaction. @param flags Saved flags. */
void kernel_execution_unlock_irqrestore(uint64_t flags) { assert(execution==flags+1);--execution; }
/** @brief Lock fixture transport. @param lock Expected lock. */
void kernel_spin_lock(struct kernel_spinlock *lock) { assert(lock==&storage_transport_lock && !transport_inside);transport_inside=1; }
/** @brief Unlock fixture transport. @param lock Expected lock. */
void kernel_spin_unlock(struct kernel_spinlock *lock) { assert(lock==&storage_transport_lock && transport_inside);transport_inside=0; }
/** @brief Check alias notification occurs before transport submission.
 * @param device Borrowed fixture identity. @param first First sector.
 * @param count Nonzero sector count.
 */
void storage_filesystem_cache_event_device(const struct block_device *device,uint64_t first,uint32_t count)
{ assert(execution && !transport_inside && device==(void *)(uintptr_t)1 && count && first+count<=64);++cache_events; }
/** @brief Return immutable fixture geometry. @param ref Device. @param info Output.
 * @return Zero. */
int block_device_info(const struct block_device_ref *ref,struct block_device_info *info)
{ assert(ref && ref->device);*info=(struct block_device_info){.blocks=64,.block_size=512,.max_blocks=2,.flags=BLOCK_DEVICE_SWAP_SAFE};return 0; }
/** @brief Retire the fake owned device reference. @param ref Owned token. */
void block_device_put(struct block_device_ref *ref) { assert(ref && ref->device);ref->device=NULL;++freed; }
/** @brief Read current fixture disk blocks. @param ref Device. @param first LBA.
 * @param count Blocks. @param out Destination. @return Zero or injected errno. */
int block_device_read(const struct block_device_ref *ref,uint64_t first,uint32_t count,void *out)
{ assert(execution && ref && ref->device && count<=2 && first+count<=64);
  if(test_nested_admission) {struct swap_storage_token other={0};assert(swap_storage_begin(&other)==-EBUSY);
    assert(storage_acquire_task_io()==-EBUSY);}
  ++calls;if(injected && calls==2)return injected;
  memcpy(out,disk+first*512,(size_t)count*512);transfers+=count;return 0; }
/** @brief Write current fixture disk blocks. @param ref Device. @param first LBA.
 * @param count Blocks. @param in Source. @return Zero or injected errno. */
int block_device_write(const struct block_device_ref *ref,uint64_t first,uint32_t count,const void *in)
{ assert(execution && ref && ref->device && count<=2 && first+count<=64);++calls;if(injected && calls==2)return injected;
  memcpy(disk+first*512,in,(size_t)count*512);transfers+=count;return 0; }
/** @brief Translate the fixture physical page. @param phys Aligned address. @return Pointer. */
void *mm_phys_to_virt(uint64_t phys) { return (void *)(uintptr_t)phys; }

/** @brief Transfer fragmented fixed extents while honoring per-command limits.
 * @return Zero when completion, failure and ownership rules hold.
 */
int main(void)
{
    if(!swap_backend_create || !swap_backend_add_extent || !swap_backend_destroy || !swap_backend_page_io) {
        fputs("FAIL: fixed swap backend is absent\n",stderr);return 1;
    }
    struct block_device_ref ref={.device=(void *)(uintptr_t)1};struct swap_backend *backend=NULL;
    assert(!swap_backend_create(&backend,&ref,0,64) && !ref.device);
    assert(!swap_backend_add_extent(backend,0,4,5));
    assert(swap_backend_add_extent(backend,5,7,3)==-EINVAL); /* physical alias */
    assert(swap_backend_add_extent(backend,6,20,3)==-EINVAL); /* logical gap */
    assert(!swap_backend_add_extent(backend,5,20,3));
    struct swap_area area={.backend=backend};void *page=aligned_alloc(4096,4096);assert(page);
    storage_task_io_owner=77;
    assert(swap_backend_page_io(&area,0,(uintptr_t)page,false)==SWAP_STORAGE_CONTENDED);
    assert(storage_task_io_owner==77 && !calls && !execution);
    storage_task_io_owner=3;
    assert(swap_backend_page_io(&area,0,(uintptr_t)page,false)==-EBUSY);
    assert(storage_task_io_owner==3 && !calls && !execution);
    storage_task_io_owner=0;
    memset(page,0x5a,4096);assert(!swap_backend_page_io(&area,0,(uintptr_t)page,true));
    assert(calls==5 && transfers==8 && cache_events==5);calls=transfers=0;memset(page,0,4096);
    assert(!swap_backend_page_io(&area,0,(uintptr_t)page,false));
    for(unsigned i=0;i<4096;++i)assert(((uint8_t *)page)[i]==0x5a);
    calls=transfers=0;injected=-EIO;
    assert(swap_backend_page_io(&area,0,(uintptr_t)page,false)==-EIO);
    assert(calls==2 && transfers==2); /* no replay of an already completed command */
    assert(!storage_task_io_owner);
    fixture_pid=0;execution=1;calls=transfers=0;injected=0;
    assert(!swap_storage_cleanup_enter());
    storage_task_io_owner=3;
    assert(swap_backend_page_io(&area,0,(uintptr_t)page,false)==SWAP_STORAGE_CONTENDED);
    assert(storage_task_io_owner==3 && !calls && execution==1);
    storage_task_io_owner=0;test_nested_admission=true;
    assert(!swap_backend_page_io(&area,0,(uintptr_t)page,false));
    assert(execution==1 && !storage_task_io_owner && calls==5);
    calls=transfers=0;injected=-EIO;
    assert(swap_backend_page_io(&area,0,(uintptr_t)page,false)==-EIO);
    assert(calls==2 && execution==1 && !storage_task_io_owner);
    swap_storage_cleanup_leave();execution=0;test_nested_admission=false;
    assert(swap_backend_page_io(&area,0,(uintptr_t)page,false)==-ESRCH);
    swap_backend_destroy(backend);free(page);
    assert(freed==1 && !live);puts("swap fixed-block I/O: ok");return 0;
}
