/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/lock.h>

struct swap_area;
struct swap_stats { uint64_t total, free, used; };
extern int swap_area_create(const void *,uint64_t,uint64_t,int32_t,struct swap_area **) __attribute__((weak));
extern int swap_area_create_checkpoint(const void *,uint64_t,uint64_t,int32_t,struct swap_area **,
                                       int (*)(void *),void *) __attribute__((weak));
extern int swap_area_publish(struct swap_area *) __attribute__((weak));
extern int swap_area_unpublish(struct swap_area *) __attribute__((weak));
extern void swap_area_destroy(struct swap_area *) __attribute__((weak));
extern uint64_t swap_slot_alloc(void) __attribute__((weak));
extern int swap_slot_get(uint64_t) __attribute__((weak));
extern void swap_slot_put(uint64_t) __attribute__((weak));
extern int swap_slot_submit_write(uint64_t,uint64_t) __attribute__((weak));
extern int swap_slot_submit_read(uint64_t,uint64_t) __attribute__((weak));
extern void swap_get_stats(struct swap_stats *) __attribute__((weak));
static struct swap_area *first,*second,*low;
static uint8_t media[3][8][4096];
static int fail_io;
static unsigned live_allocations, fail_after;
static unsigned checkpoints;
/** @brief Force rollback at the first preparation checkpoint.
 * @param context Borrowed fixture state. @return Injected terminal error.
 */
static int fail_checkpoint(void *context) { (void)context;++checkpoints;return -EINTR; }

/** @brief Allocate zeroed fixture metadata. @param size Byte size. @return Memory or NULL. */
void *kernel_malloc(size_t size)
{ if(fail_after && !--fail_after)return NULL; void *p=calloc(1,size);if(p)++live_allocations;return p; }
/** @brief Free owned fixture metadata. @param p Prior allocation, nullable. */
void kernel_free(void *p) { if(p){assert(live_allocations);--live_allocations;free(p);} }
/** @brief Supply an IRQ-safe fixture lock. @param lock Lock. @param flags Saved state. */
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags) { (void)lock; *flags=1; }
/** @brief Release fixture lock. @param lock Lock. @param flags Saved state. */
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags) { (void)lock;(void)flags; }
/** @brief Run the area backend through an injected fixed-page disk.
 * @param area Borrowed area. @param page Logical page index.
 * @param physical Stable aligned CPU address in this fixture. @param write Direction.
 * @return Zero or injected final errno.
 */
int swap_backend_page_io(struct swap_area *area,uint32_t page,uint64_t physical,bool write)
{
    unsigned index=area==first ? 0 : area==second ? 1 : 2;
    assert(area==first || area==second || area==low); assert(page<8);
    if(fail_io)return fail_io;
    if(write)memcpy(media[index][page],(void *)(uintptr_t)physical,4096);
    else memcpy((void *)(uintptr_t)physical,media[index][page],4096);
    return 0;
}
/** @brief Encode one little-endian header word. @param p Output. @param n Value. */
static void word(uint8_t *p,uint32_t n)
{ p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24; }
/** @brief Construct one v1 header reserving page two. @param h Output page. */
static void header(uint8_t *h)
{ memset(h,0,4096);word(h+1024,1);word(h+1028,4);word(h+1032,1);word(h+1536,2);memcpy(h+4086,"SWAPSPACE2",10); }

/** @brief Verify slot accounting, fair priority and data preservation on failure.
 * @return Zero when all assertions hold.
 */
int main(void)
{
    if(!swap_area_create || !swap_area_publish || !swap_area_unpublish || !swap_area_destroy ||
       !swap_slot_alloc || !swap_slot_get || !swap_slot_put || !swap_slot_submit_write ||
       !swap_slot_submit_read || !swap_get_stats) {
        fputs("FAIL: production swap area and slot lifecycle is absent\n",stderr);return 1;
    }
    uint8_t h[4096];header(h);struct swap_area *failed=NULL;
    if(!swap_area_create_checkpoint){fputs("FAIL: bounded preparation is absent\n",stderr);return 1;}
    word(h+1028,9000);word(h+1032,0);
    assert(swap_area_create_checkpoint(h,4096,9001,1,&failed,fail_checkpoint,NULL)==-EINTR);
    assert(checkpoints>=1 && !failed && !live_allocations);header(h);
    fail_after=2;assert(swap_area_create(h,4096,5,10,&failed)==-ENOMEM && !failed && !live_allocations);
    fail_after=0;
    assert(!swap_area_create(h,4096,5,10,&first));assert(!swap_area_publish(first));
    assert(!swap_area_create(h,4096,5,10,&second));assert(!swap_area_publish(second));
    assert(!swap_area_create(h,4096,5,1,&low));assert(!swap_area_publish(low));
    struct swap_stats stats;swap_get_stats(&stats);assert(stats.total==9 && stats.free==9 && !stats.used);
    uint64_t a=swap_slot_alloc(),b=swap_slot_alloc();assert(a && b && (a>>32)!=(b>>32));
    swap_get_stats(&stats);assert(stats.free==7 && stats.used==2);
    assert(swap_area_unpublish(first)==-EBUSY);
    void *page=aligned_alloc(4096,4096);assert(page);memset(page,0x5a,4096);
    fail_io=-EIO;assert(swap_slot_submit_write(a,(uintptr_t)page)==-EIO);
    fail_io=0;assert(!swap_slot_submit_write(a,(uintptr_t)page));
    memset(page,0xa5,4096);assert(swap_slot_submit_write(a,(uintptr_t)page)==-EBUSY);
    fail_io=-EIO;assert(swap_slot_submit_read(a,(uintptr_t)page)==-EIO);
    fail_io=0;assert(!swap_slot_submit_read(a,(uintptr_t)page));
    for(unsigned i=0;i<4096;++i)assert(((uint8_t *)page)[i]==0x5a);
    assert(!swap_slot_get(a));swap_slot_put(a);assert(swap_area_unpublish(first)==-EBUSY);
    swap_slot_put(a);swap_slot_put(b);assert(!swap_area_unpublish(first));
    assert(swap_slot_get(a)<0);assert(!swap_area_unpublish(second));assert(!swap_area_unpublish(low));
    swap_area_destroy(first);swap_area_destroy(second);swap_area_destroy(low);
    assert(!live_allocations);free(page);
    swap_get_stats(&stats);assert(!stats.total && !stats.free && !stats.used);
    puts("swap slot lifecycle: ok");return 0;
}
