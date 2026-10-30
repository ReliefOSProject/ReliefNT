/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/usb/xhci/internal.h"

struct image {uint32_t words[1024];unsigned reads,writes;};
static int read_reg(void *ctx,uint32_t offset,uint32_t *value)
{
    struct image *image=ctx;assert(!(offset&3u) && offset<sizeof(image->words));
    ++image->reads;*value=image->words[offset/4u];return 0;
}
static int write8(void *ctx,uint32_t offset,uint8_t value)
{
    struct image *image=ctx;assert(offset<sizeof(image->words));++image->writes;
    ((uint8_t *)image->words)[offset]=value;return 0;
}
static int write32(void *ctx,uint32_t offset,uint32_t value)
{
    struct image *image=ctx;assert(!(offset&3u) && offset<sizeof(image->words));
    ++image->writes;image->words[offset/4u]=value;return 0;
}
int xhci_timer_start(struct xhci_timer *timer){*timer=(struct xhci_timer){0};return 0;}
bool xhci_timer_expired(struct xhci_timer *timer,uint64_t microseconds)
{(void)timer;(void)microseconds;return true;}
static int extent_for(struct image *image,uint32_t hcc,uint32_t db,uint32_t rt,
                      uint32_t *legacy,uint32_t *extent)
{
    struct xhci_geometry geometry;
    int rc=xhci_geometry_init(0x40,0x10001040,db,rt,0xc0000000,&geometry);
    return rc ? rc : xhci_firmware_extent(read_reg,image,&geometry,hcc,legacy,extent);
}
static void rejected(uint32_t offset,uint32_t header,uint32_t doorbells,uint32_t runtime,unsigned allowed_reads)
{
    struct image image={0};uint32_t legacy=0,extent=0;bool acquired=false;
    image.words[offset/4u]=header;
    int rc=extent_for(&image,(offset/4u)<<16,doorbells,runtime,&legacy,&extent);
    if(!rc)rc=xhci_firmware_handoff(read_reg,write8,write32,&image,legacy,&acquired);
    assert(rc==-EINVAL && !acquired && image.reads==allowed_reads && !image.writes);
}
int main(void)
{
    rejected(0x3c,0x01000001,0x2000,0x1000,1); /* legacy control would overwrite USBCMD */
    rejected(0x440,0x01000001,0x2000,0x1000,0); /* header is PORTSC itself */
    rejected(0x38,0x02000002,0x2000,0x1000,1); /* supported-protocol range overlaps USBCMD */
    rejected(0x560,0x01000001,0x560,0x700,0); /* doorbell header */
    rejected(0x55c,0x01000001,0x560,0x700,1); /* legacy straddles doorbell */
    rejected(0x700,0x01000001,0x560,0x700,0); /* runtime header */
    rejected(0x6fc,0x01000001,0x560,0x700,1); /* legacy straddles runtime */
    rejected(0x780,0x01000001,0x560,0x700,0); /* later implemented interrupter */
    struct image image={0};uint32_t legacy=0,extent=0;bool acquired=false;
    image.words[0x42c/4u]=0x02000002;
    image.words[0x434/4u]=4u<<28; /* full PSI table straddles PORTSC */
    int rc=extent_for(&image,0x10bu<<16,0x2000,0x1000,&legacy,&extent);
    if(!rc)rc=xhci_firmware_handoff(read_reg,write8,write32,&image,legacy,&acquired);
    assert(rc==-EINVAL && image.reads==2 && !image.writes && !acquired);
    memset(&image,0,sizeof(image));image.words[0x80/4u]=0x01000001;
    assert(!extent_for(&image,0x20u<<16,0x2000,0x1000,&legacy,&extent));
    assert(legacy==0x80 && !xhci_firmware_handoff(read_reg,write8,write32,&image,legacy,&acquired));
    assert(acquired && image.writes==2); /* adjacent non-overlapping legacy remains usable */
    puts("USB xHCI firmware capability records avoid live operational registers");
}
