/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/pat.h>
static bool supported=true, ignore_write;
static uint64_t reg=0x0007040600070406ULL;
static unsigned reads,writes,flushes;
static uint64_t irq=0x200;
uint64_t kernel_irq_save(void){uint64_t old=irq;irq=0;return old;}
void kernel_irq_restore(uint64_t flags){irq=flags;}
bool x86_pat_native_supported(void){return supported;}
uint64_t x86_pat_native_read(void){assert(!irq);++reads;return reg;}
void x86_pat_native_install(uint64_t value){assert(!irq);++writes;if(!ignore_write)reg=value;}
void x86_pat_native_flush(void){assert(!irq);++flushes;}
int main(int argc,char **argv)
{
    assert(argc==2);
    const uint64_t firmware=reg, wc=0x0007040100070406ULL;
    if(!strcmp(argv[1],"nopat") || !strcmp(argv[1],"unsupported")) {
        bool enabled=strcmp(argv[1],"nopat")!=0;
        if(enabled)supported=false;
        assert(!arch_pat_boot_init(enabled));
        assert(!arch_pat_writecombine_ready() && reg==firmware && !writes && !reads);
        assert(arch_pat_ap_init() && !writes && !reads && irq==0x200);
        assert(!arch_pat_boot_init(true));
    } else if(!strcmp(argv[1],"layout")) {
        reg=0x0007040600070404ULL; /* RAM slot zero is not WB. */
        assert(!arch_pat_boot_init(true) && !writes && !arch_pat_writecombine_ready());
    } else if(!strcmp(argv[1],"write-failure")) {
        ignore_write=true;
        assert(!arch_pat_boot_init(true));
        assert(reg==firmware && writes==2 && !arch_pat_writecombine_ready());
    } else {
        assert(arch_pat_boot_init(true) && arch_pat_writecombine_ready());
        assert(reg==wc && (reg & ~(0xffULL<<32))==(firmware & ~(0xffULL<<32)));
        assert(irq==0x200 && writes==1);
        assert(arch_pat_boot_init(true) && writes==1);
        if(!strcmp(argv[1],"ap-unsupported")) {
            supported=false;assert(!arch_pat_ap_init());
        } else if(!strcmp(argv[1],"ap-write-failure")) {
            reg=firmware;ignore_write=true;assert(!arch_pat_ap_init());
        } else {
            reg=firmware;assert(arch_pat_ap_init() && reg==wc && writes==2);
            assert(arch_pat_ap_init() && writes==2);
            irq=0;arch_pat_flush_boot();assert(flushes==1);irq=0x200;
        }
    }
    assert(irq==0x200);
    printf("PASS PAT boot/AP policy: %s\n",argv[1]);
    return 0;
}
