/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <reliefnt/swap_internal.h>

extern int swap_storage_claim_range(struct swap_storage_claim *,const struct block_device_ref *,
                                    uint64_t,uint64_t,bool) __attribute__((weak));
extern void swap_storage_release_range(struct swap_storage_claim *) __attribute__((weak));
extern int swap_storage_raw_busy(const struct block_device_ref *,uint64_t,uint64_t,bool)
    __attribute__((weak));

/** @brief Give both fake devices identical geometry with distinct identities.
 * @param ref Borrowed device. @param out Required result. @return Zero.
 */
int block_device_info(const struct block_device_ref *ref,struct block_device_info *out)
{ assert(ref && ref->device); *out=(struct block_device_info){.blocks=1000,.block_size=512}; return 0; }

/** @brief Exercise overlapping raw aliases while allowing same-filesystem swap files.
 * @return Zero when range lifetime and device identity behave correctly.
 */
int main(void)
{
    if(!swap_storage_claim_range || !swap_storage_release_range || !swap_storage_raw_busy) {
        fputs("FAIL: production swap block-range claims are absent\n",stderr); return 1;
    }
    struct block_device_ref a={.device=(void *)(uintptr_t)1},b={.device=(void *)(uintptr_t)2};
    struct swap_storage_claim file1={0},file2={0},part={0};
    assert(!swap_storage_claim_range(&file1,&a,100,200,true));
    assert(!swap_storage_claim_range(&file2,&a,100,200,true));
    assert(swap_storage_claim_range(&part,&a,150,30,false)==-EBUSY);
    assert(!swap_storage_claim_range(&part,&a,400,30,false));
    assert(swap_storage_raw_busy(&a,100,1,false));
    assert(swap_storage_raw_busy(&a,299,1,false));
    assert(!swap_storage_raw_busy(&a,300,1,false));
    assert(swap_storage_raw_busy(&a,0,1,true));
    assert(!swap_storage_raw_busy(&b,100,1,true));
    swap_storage_release_range(&file1);
    assert(swap_storage_raw_busy(&a,100,1,false));
    swap_storage_release_range(&file2);
    assert(!swap_storage_raw_busy(&a,100,1,false));
    assert(swap_storage_raw_busy(&a,400,1,false));
    swap_storage_release_range(&part);
    assert(!swap_storage_raw_busy(&a,0,1,true));
    puts("swap block range claims: ok"); return 0;
}
