/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <reliefnt/swap.h>
#include <reliefnt/syscall.h>

/** @brief Disabled interfaces must not report storage or I/O success.
 * @return Zero if disabled results remain safe and truthful.
 */
int main(void)
{
    struct swap_stats stats={1,1,1};swap_get_stats(&stats);
    assert(!stats.total && !stats.free && !stats.used && !swap_slot_alloc());
    assert(swap_slot_get(1)==-ENOSYS && swap_slot_submit_read(1,4096)==-ENOSYS &&
           swap_slot_submit_write(1,4096)==-ENOSYS);
    assert(syscall_mm_swapon(0,0x80000)==-ENOSYS && syscall_mm_swapoff(0)==-ENOSYS);
    swap_slot_put(1);swap_io_poll(1);puts("swap disabled interfaces: ok");return 0;
}
