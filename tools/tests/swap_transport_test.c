/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/blkdev.h>
#include <reliefnt/lock.h>
#include <reliefnt/swap_internal.h>

#ifndef BLOCK_DEVICE_SWAP_SAFE
#define BLOCK_DEVICE_SWAP_SAFE 16u
#endif

/* Weak symbols make an absent implementation a behavioral RED, not a link failure. */
extern int swap_storage_begin(struct swap_storage_token *) __attribute__((weak));
extern void swap_storage_end(struct swap_storage_token *) __attribute__((weak));
extern int swap_storage_flush(const struct swap_storage_token *,const struct block_device_ref *) __attribute__((weak));
extern int swap_storage_transfer(const struct swap_storage_token *, const struct block_device_ref *,
                                 uint64_t, uint32_t, void *, bool) __attribute__((weak));

uint32_t storage_task_io_owner;
bool storage_io_async_context, storage_io_write_started;
struct kernel_spinlock storage_transport_lock;
static uint32_t fake_pid=7;
static int inside, calls, result, cache_events;
static uint64_t cache_first;
static uint32_t cache_count;
static unsigned char disk[8*4096];
static struct block_device_info geometry={.blocks=8,.block_size=512,.max_blocks=3,
                                          .flags=BLOCK_DEVICE_SWAP_SAFE};

/** @brief Return the current fixture task identity. @return Fixture PID. */
uint32_t sched_current_pid(void) { return fake_pid; }
/** @brief Track the production transport lock. @param lock Expected lock. */
void kernel_spin_lock(struct kernel_spinlock *lock)
{ assert(lock==&storage_transport_lock && !inside); inside=1; }
/** @brief Release the fixture transport lock. @param lock Expected lock. */
void kernel_spin_unlock(struct kernel_spinlock *lock)
{ assert(lock==&storage_transport_lock && inside); inside=0; }
/** @brief Observe alias invalidation before each attempted provider write.
 * @param device Borrowed fixture identity. @param first First physical sector.
 * @param count Nonzero sector count.
 */
void storage_filesystem_cache_event_device(const struct block_device *device,uint64_t first,uint32_t count)
{ assert(device==(void *)(uintptr_t)1 && !inside && count && first+count<=64);
  cache_first=first;cache_count=count;++cache_events; }
/** @brief Supply the immutable provider geometry.
 * @param ref Borrowed identity. @param out Required geometry. @return Zero.
 */
int block_device_info(const struct block_device_ref *ref,struct block_device_info *out)
{ assert(ref && out); *out=geometry; return 0; }
/** @brief Perform one fake provider read with real transaction assertions.
 * @param ref Device. @param first Starting block. @param count Block count.
 * @param buffer Writable bytes. @return Injected result or complete success.
 */
int block_device_read(const struct block_device_ref *ref,uint64_t first,uint32_t count,void *buffer)
{
    assert(ref && inside && !storage_io_async_context && storage_io_write_started);
    assert(storage_task_io_owner==fake_pid); ++calls;
    if(result)return result;
    memcpy(buffer,disk+first*geometry.block_size,count*geometry.block_size);return 0;
}
/** @brief Flush while holding the shared transport. @param ref Held device.
 * @return Injected errno or zero. */
int block_device_flush(const struct block_device_ref *ref)
{ assert(ref && inside && !storage_io_async_context);++calls;return result; }
/** @brief Perform one fake provider write with real transaction assertions.
 * @param ref Device. @param first Starting block. @param count Block count.
 * @param buffer Readable bytes. @return Injected result or complete success.
 */
int block_device_write(const struct block_device_ref *ref,uint64_t first,uint32_t count,const void *buffer)
{
    assert(ref && inside && !storage_io_async_context && storage_io_write_started);
    assert(storage_task_io_owner==fake_pid); ++calls;
    if(result)return result;
    memcpy(disk+first*geometry.block_size,buffer,count*geometry.block_size);return 0;
}

/** @brief Check exclusion, bounded command geometry and terminal provider errors.
 * @return Zero when all assertions hold.
 */
int main(void)
{
    if(!swap_storage_begin || !swap_storage_end || !swap_storage_transfer || !swap_storage_flush) {
        fputs("FAIL: production swap storage transaction is absent\n",stderr); return 1;
    }
    struct swap_storage_token token={0}; struct block_device_ref ref={.device=(void *)(uintptr_t)1};
    unsigned char data[4096]; memset(data,0x5a,sizeof(data));
    storage_io_async_context=true; storage_io_write_started=false; storage_task_io_owner=9;
    assert(swap_storage_begin(&token)==-EBUSY && storage_task_io_owner==9 && storage_io_async_context);
    storage_task_io_owner=0;
    assert(!swap_storage_begin(&token));
    assert(token.active && storage_task_io_owner==fake_pid && !storage_io_async_context);
    assert(!swap_storage_transfer(&token,&ref,2,3,data,true));
    assert(cache_events==1);
    memset(data,0,sizeof(data)); assert(!swap_storage_transfer(&token,&ref,2,3,data,false));
    assert(cache_events==1);
    for(unsigned i=0;i<1536;++i)assert(data[i]==0x5a);
    int oldcalls=calls;
    assert(swap_storage_transfer(&token,&ref,0,4,data,false)<0 && calls==oldcalls);
    assert(swap_storage_transfer(&token,&ref,7,2,data,false)<0 && calls==oldcalls);
    geometry.flags=0;
    assert(swap_storage_transfer(&token,&ref,0,1,data,false)==-EOPNOTSUPP && calls==oldcalls);
    geometry.flags=BLOCK_DEVICE_SWAP_SAFE; result=-EAGAIN;
    assert(swap_storage_transfer(&token,&ref,0,1,data,false)!=-EAGAIN);
    int oldevents=cache_events;
    assert(swap_storage_transfer(&token,&ref,0,1,data,true)==-EIO && cache_events==oldevents+1);
    result=-ENODEV; assert(swap_storage_transfer(&token,&ref,0,1,data,false)==-ENODEV);
    assert(swap_storage_transfer(&token,&ref,0,1,data,true)==-ENODEV && cache_events==oldevents+2);
    assert(swap_storage_flush(&token,&ref)==-ENODEV);
    result=0;assert(!swap_storage_flush(&token,&ref));
    geometry.block_size=4096;
    assert(!swap_storage_transfer(&token,&ref,1,1,data,true));
    assert(cache_first==8 && cache_count==8);
    oldevents=cache_events;oldcalls=calls;geometry.blocks=UINT64_MAX;
    assert(swap_storage_transfer(&token,&ref,UINT64_MAX/8+1,1,data,true)==-EOVERFLOW);
    assert(cache_events==oldevents && calls==oldcalls);
    swap_storage_end(&token);
    assert(!token.active && !storage_task_io_owner && storage_io_async_context && !storage_io_write_started);
    assert(swap_storage_transfer(&token,&ref,0,1,data,false)<0);
    puts("swap transport transaction: ok"); return 0;
}
