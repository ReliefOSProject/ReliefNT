/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../arch/x86_64/mm/pat_internal.h"

static const unsigned fixed[]={0x250,0x258,0x259,0x268,0x269,0x26a,
    0x26b,0x26c,0x26d,0x26e,0x26f};
static struct cpu {
    uint64_t msrs[0x400];
    uint32_t features;
    uint8_t width;
} bsp, ap, *current=&bsp;
static unsigned reads,writes,begins,ends,ignored;
static bool updating;

/** @brief Admit only feature-present MTRR registers in the simulated CPU.
 * @param msr Register being accessed. @return True for an implemented register.
 */
static bool implemented(unsigned msr)
{
    assert(current->features & (1u<<12));
    if (msr==0xfe || msr==0x2ff) return true;
    if (msr>=0x200 && msr<0x200+2*(current->msrs[0xfe]&0xff)) return true;
    if (current->msrs[0xfe] & (1u<<8))
        for (unsigned i=0;i<11;++i) if (msr==fixed[i]) return true;
    return false;
}

uint32_t x86_cache_native_features(void) { return current->features; }
uint8_t paging_physical_bits(void) { return current->width; }
uint64_t x86_cache_native_read(uint32_t msr)
{
    assert(implemented(msr)); ++reads; return current->msrs[msr];
}
void x86_cache_native_write(uint32_t msr,uint64_t value)
{
    assert(current==&ap && updating && implemented(msr) && msr!=0xfe);
    /* Every range update must happen with MTRRs disabled, inside the local
     * cache/TLB serialization boundary, before the final default is restored. */
    if (msr!=0x2ff) assert(!(current->msrs[0x2ff] & 0xc00));
    else if (!writes) assert(!(value & 0xc00));
    ++writes;
    if (msr!=ignored) current->msrs[msr]=value;
}
uint64_t x86_cache_native_begin(void)
{
    assert(current==&ap && !updating); updating=true; ++begins; return 0x80000011;
}
void x86_cache_native_end(uint64_t cr0)
{
    assert(updating && cr0==0x80000011); updating=false; ++ends;
}

/** @brief Exercise firmware retention, AP reset recovery and refusal boundaries.
 * @param argc Must name one independent scenario. @param argv Scenario name.
 * @return Zero only for exact readback or the expected safe refusal.
 */
int main(int argc,char **argv)
{
    assert(argc==2);
    bsp.features=ap.features=(1u<<12)|(1u<<16);
    bsp.width=ap.width=46;
    bsp.msrs[0xfe]=ap.msrs[0xfe]=0x508;
    bsp.msrs[0x2ff]=0xc06;
    for (unsigned i=0;i<8;++i) {
        bsp.msrs[0x200+2*i]=((uint64_t)i<<28)|6;
        bsp.msrs[0x201+2*i]=0x3ffff0000800ULL;
    }
    for (unsigned i=0;i<11;++i) bsp.msrs[fixed[i]]=0x0606060606060606ULL;
    if (!strcmp(argv[1],"unsupported") || !strcmp(argv[1],"ap-only-mtrr")) {
        bsp.features=0;
        if (!strcmp(argv[1],"unsupported")) ap.features=0;
        x86_mtrr_capture_boot();current=&ap;
        assert(x86_mtrr_restore_ap()==!ap.features && !reads && !writes && !begins);
        goto done;
    }
    if (!strcmp(argv[1],"no-fixed")) {
        bsp.msrs[0xfe]=ap.msrs[0xfe]=0x408;
        bsp.msrs[0x2ff]=0x806;
    }
    if (!strcmp(argv[1],"disabled")) bsp.msrs[0x2ff]=0;
    struct cpu original=bsp;
    x86_mtrr_capture_boot();
    assert(!memcmp(&bsp,&original,sizeof(bsp)) && !writes && !begins);
    current=&ap;
    if (!strcmp(argv[1],"ap-unsupported")) {
        ap.features=0; unsigned before=reads;
        assert(!x86_mtrr_restore_ap() && reads==before && !writes && !begins);
        goto done;
    }
    if (!strcmp(argv[1],"capability")) --ap.msrs[0xfe];
    if (!strcmp(argv[1],"physical-width")) ++ap.width;
    if (!strcmp(argv[1],"capability") || !strcmp(argv[1],"physical-width")) {
        assert(!x86_mtrr_restore_ap() && !writes && !begins);
        goto done;
    }
    if (!strcmp(argv[1],"matching")) ap=bsp;
    if (!strcmp(argv[1],"fixed-only") || !strcmp(argv[1],"variable-only")) {
        ap=bsp;
        ap.msrs[!strcmp(argv[1],"fixed-only") ? 0x26f : 0x20f]=0;
    }
    if (!strcmp(argv[1],"write-failure")) ignored=0x200;
    bool passed=x86_mtrr_restore_ap();
    assert(!updating && begins==ends);
    assert(!memcmp(&bsp,&original,sizeof(bsp)));
    if (ignored) { assert(!passed && begins==1); goto done; }
    assert(passed);
    if (!strcmp(argv[1],"matching")) assert(!writes && !begins);
    else assert(begins==1 && writes==18+((bsp.msrs[0xfe] & (1u<<8)) ? 11 : 0));
    for (unsigned msr=0;msr<0x400;++msr)
        if (implemented(msr)) assert(ap.msrs[msr]==bsp.msrs[msr]);
    unsigned before=writes;
    assert(x86_mtrr_restore_ap() && writes==before);
done:
    printf("PASS MTRR boot/AP policy: %s\n",argv[1]);
    return 0;
}
