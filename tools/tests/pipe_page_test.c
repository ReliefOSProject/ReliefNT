/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/paging.h>
#define PAGE_SIZE_FLAG 0x80u
static uint64_t root[512] __attribute__((aligned(4096)));
static uint64_t upper[512] __attribute__((aligned(4096)));
static uint64_t middle[512] __attribute__((aligned(4096)));
static uint64_t leaves[512] __attribute__((aligned(4096)));
void *mm_phys_to_virt(uint64_t physical)
{ return physical == (uintptr_t)upper ? upper : physical == (uintptr_t)middle ? middle :
         physical == (uintptr_t)leaves ? leaves : NULL; }
#include "pipe_pages.inc"

int main(void)
{
    struct address_space as={.pml4=root};uint64_t va=RELIEFNT_USER_BASE;
    unsigned a=(va>>39)&511u,b=(va>>30)&511u,c=(va>>21)&511u,d=(va>>12)&511u;
    const uint64_t rw=RELIEFNT_PAGE_PRESENT|RELIEFNT_PAGE_USER|RELIEFNT_PAGE_WRITABLE;
    root[a]=(uintptr_t)upper|rw;upper[b]=(uintptr_t)middle|rw;middle[c]=(uintptr_t)leaves|rw;
    leaves[d]=0x100000|rw;
    assert(address_space_user_page_copyable(&as,va,false) && address_space_user_page_copyable(&as,va,true));
    leaves[d]&=~RELIEFNT_PAGE_WRITABLE;leaves[d]|=RELIEFNT_PAGE_WRITE_NOTIFY;
    assert(address_space_user_page_copyable(&as,va,false) && !address_space_user_page_copyable(&as,va,true));
    leaves[d]=(0x100000|rw|RELIEFNT_PAGE_COW)&~RELIEFNT_PAGE_WRITABLE;
    assert(address_space_user_page_copyable(&as,va,false) && !address_space_user_page_copyable(&as,va,true));
    const uint64_t excluded[]={RELIEFNT_PAGE_DEVICE,RELIEFNT_PAGE_PROTNONE};
    for(unsigned i=0;i<2;++i) {
        leaves[d]=0x100000|rw|excluded[i];
        assert(!address_space_user_page_copyable(&as,va,false) && !address_space_user_page_copyable(&as,va,true));
    }
    leaves[d]=RELIEFNT_PAGE_USER|RELIEFNT_PAGE_SWAP;
    assert(!address_space_user_page_copyable(&as,va,false) && !address_space_user_page_copyable(&as,va,true));
    leaves[d]=0x100000|rw;
    uint64_t *branches[]={&root[a],&upper[b],&middle[c]};
    for(unsigned i=0;i<3;++i) {
        uint64_t original=*branches[i];*branches[i]&=~RELIEFNT_PAGE_WRITABLE;
        assert(address_space_user_page_copyable(&as,va,false) && !address_space_user_page_copyable(&as,va,true));
        *branches[i]=original&~RELIEFNT_PAGE_USER;
        assert(!address_space_user_page_copyable(&as,va,false) && !address_space_user_page_copyable(&as,va,true));
        *branches[i]=original;
    }
    assert(!address_space_user_page_copyable(NULL,va,true));
    assert(!address_space_user_page_copyable(&as,RELIEFNT_KERNEL_HOLE_START,false));
    assert(!address_space_user_page_copyable(&as,RELIEFNT_USER_TOP,false));
    puts("PASS nonfaulting RAM permission checks reject COW, first-write traps, devices, swap and protected branches");
    return 0;
}
