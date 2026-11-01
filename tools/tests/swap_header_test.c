/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The weak declaration lets the pre-feature fixture run and report the
 * missing behavior instead of treating a link failure as behavioral RED. */
extern int swap_header_validate(const void *, uint64_t, uint64_t, uint32_t *, uint32_t *)
    __attribute__((weak));

/** @brief Store a 32-bit value in the swap v1 disk byte order.
 * @param p Writable four-byte destination. @param n Value to store.
 */
static void put32(unsigned char *p, uint32_t n)
{
    p[0]=(unsigned char)n; p[1]=(unsigned char)(n>>8);
    p[2]=(unsigned char)(n>>16); p[3]=(unsigned char)(n>>24);
}

/** @brief Build a minimal valid swap v1 header.
 * @param h Writable 4096-byte header. @param last Last page index.
 */
static void valid(unsigned char *h, uint32_t last)
{
    memset(h,0,4096); put32(h+1024,1); put32(h+1028,last);
    memcpy(h+4086,"SWAPSPACE2",10);
}

/** @brief Exercise header validation against real v1 offsets and capacity.
 * @return Zero when all assertions hold.
 */
int main(void)
{
    unsigned char h[4096]; uint32_t last=99, bad=99;
    if (!swap_header_validate) {
        fputs("FAIL: production swap_header_validate is absent\n",stderr);
        return 1;
    }
    valid(h,15);
    assert(swap_header_validate(h,4096,16,&last,&bad)==0 && last==15 && bad==0);
    assert(swap_header_validate(h,4095,16,&last,&bad)<0);
    assert(swap_header_validate(h,4096,15,&last,&bad)<0);
    h[4086]='X'; assert(swap_header_validate(h,4096,16,&last,&bad)<0); h[4086]='S';
    put32(h+1024,0x01000000); assert(swap_header_validate(h,4096,16,&last,&bad)<0);
    put32(h+1024,2); assert(swap_header_validate(h,4096,16,&last,&bad)<0);
    put32(h+1024,1); put32(h+1028,0); assert(swap_header_validate(h,4096,16,&last,&bad)<0);
    put32(h+1028,15); put32(h+1032,638); assert(swap_header_validate(h,4096,16,&last,&bad)<0);
    put32(h+1032,2); put32(h+1536,3); put32(h+1540,7);
    assert(swap_header_validate(h,4096,16,&last,&bad)==0 && bad==2);
    put32(h+1540,3); assert(swap_header_validate(h,4096,16,&last,&bad)<0);
    put32(h+1540,0); assert(swap_header_validate(h,4096,16,&last,&bad)<0);
    put32(h+1540,16); assert(swap_header_validate(h,4096,16,&last,&bad)<0);
    valid(h,1); put32(h+1032,1); put32(h+1536,1);
    assert(swap_header_validate(h,4096,2,&last,&bad)<0);
    assert(swap_header_validate(NULL,4096,2,&last,&bad)<0);
    puts("swap header validation: ok"); return 0;
}
