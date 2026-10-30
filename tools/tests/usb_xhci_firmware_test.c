/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../../drivers/usb/xhci/internal.h"

struct image { uint32_t words[16384]; unsigned reads, outside_reads; uint32_t bar_end; };
static int read_reg(void *context,uint32_t offset,uint32_t *value)
{
    struct image *image=context;
    if ((offset&3u) || offset>=sizeof(image->words)) return -EIO;
    if(image->bar_end && offset>=image->bar_end)++image->outside_reads;
    *value=image->words[offset/4u];++image->reads;return 0;
}
static int extent_for(struct image *image,uint8_t cap,uint32_t hcs,uint32_t hcc,
                      uint32_t db,uint32_t rt,uint32_t *legacy,uint32_t *extent)
{
    struct xhci_geometry geometry;
    int rc=xhci_geometry_init(cap,hcs,db,rt,0xc0000000,&geometry);
    return rc ? rc : xhci_firmware_extent(read_reg,image,&geometry,hcc,legacy,extent);
}
static int find(struct image *image,uint8_t cap,uint32_t hcs,uint32_t hcc,
                uint32_t db,uint32_t rt,uint32_t *legacy)
{
    uint32_t extent;
    return extent_for(image,cap,hcs,hcc,db,rt,legacy,&extent);
}

int main(void)
{
    static struct image image;uint32_t legacy=0;
    image.words[32]=(4u<<8)|2u;image.words[36]=1u;
    assert(!find(&image,0x40,0x01000140,32u<<16,0x2000,0x1000,&legacy) && legacy==0x90);
    assert(!find(&image,0x40,0x01000140,0,0x2000,0x1000,&legacy) && !legacy);
    /* A one-page-or-smaller controller can advertise a bogus xECP outside
     * its implemented BAR. No read of that address may occur before HALT. */
    image.bar_end=0x800;image.words[0x200]=1u;image.outside_reads=0;
    assert(find(&image,0x40,0x08000140,0x200u<<16,0x600,0x500,&legacy)==-EOPNOTSUPP);
    assert(!image.outside_reads);
    image.words[0x1fe]=1u;image.words[0x1ff]=0;
    assert(!find(&image,0x40,0x08000140,0x1feu<<16,0x600,0x500,&legacy) && legacy==0x7f8);
    image.words[0x1ff]=1u;
    assert(find(&image,0x40,0x08000140,0x1ffu<<16,0x600,0x500,&legacy)==-EOPNOTSUPP);
    assert(!image.outside_reads);
    image.bar_end=0;
    uint32_t minimum=0,extent=0;
    assert(!xhci_firmware_minimum(0x40,0x10001040,&minimum) && minimum==0x800);
    assert(!xhci_firmware_minimum(0x40,0xff000001,&minimum) && minimum==0x2000);
    assert(xhci_firmware_minimum(0x1c,0x10001040,&minimum)==-EINVAL);
    /* QEMU 11 capability snapshot: USB2 ports 9..16, USB3 ports 1..8. */
    image.words[8]=0x02000402;image.words[9]=0x20425355;
    image.words[10]=0x00000809;image.words[11]=0;
    image.words[12]=0x03000002;image.words[13]=0x20425355;
    image.words[14]=0x00000801;image.words[15]=0;
    assert(!extent_for(&image,0x40,0x10001040,0x00087001,0x2000,0x1000,&legacy,&extent));
    assert(!legacy && extent==0x40);
    assert(find(&image,0x40,0x01000140,0xffffu<<16,0x2000,0x1000,&legacy)==-EOPNOTSUPP);
    assert(find(&image,0x1c,0x01000140,32u<<16,0x2000,0x1000,&legacy)==-EINVAL);
    assert(find(&image,0x40,0,32u<<16,0x2000,0x1000,&legacy)==-EINVAL);
    image.words[32]=(4u<<8)|1u;image.words[36]=1u; /* duplicate legacy semaphore */
    assert(find(&image,0x40,0x01000140,32u<<16,0x2000,0x1000,&legacy)==-EINVAL);
    image.words[32]=1u;image.words[33]=0;
    assert(!find(&image,0x40,0x01000140,32u<<16,0x2000,0x1000,&legacy) && legacy==0x80);
    extent=0;
    image.words[32]=(7u<<8)|2u;image.words[34]=3u<<28;image.words[39]=1u;image.words[40]=0;
    assert(!extent_for(&image,0x40,0x01000140,32u<<16,0x2000,0x1000,&legacy,&extent));
    assert(legacy==0x9c && extent==0xa4);
    image.words[32]=(6u<<8)|2u; /* next overlaps the advertised PSI table */
    assert(extent_for(&image,0x40,0x01000140,32u<<16,0x2000,0x1000,&legacy,&extent)==-EINVAL);
    puts("USB xHCI firmware capability bounds tests passed");
}
