/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/usb/xhci/internal.h"

/* The callbacks represent the MMIO boundary; the chain and register geometry
 * are interpreted by the production walker. Record every attempted access. */
struct image { uint32_t words[0x5000 / 4]; uint32_t limit; unsigned reads, outside; };
static int read_reg(void *context,uint32_t offset,uint32_t *value)
{
    struct image *image=context;
    assert(!(offset&3u) && offset<sizeof(image->words));
    ++image->reads;
    if(offset>=image->limit)++image->outside;
    *value=image->words[offset/4u];return 0;
}
static int walk(struct image *image,uint8_t cap,uint32_t hcs,uint32_t first,
                uint32_t db,uint32_t rt,uint32_t *legacy,uint32_t *extent)
{
    struct xhci_geometry geometry;
    int rc=xhci_geometry_init(cap,hcs,db,rt,0xc0000000,&geometry);
    return rc ? rc : xhci_firmware_extent(read_reg,image,&geometry,(first/4u)<<16,legacy,extent);
}
static void vmware(bool noisy)
{
    struct image image={.limit=0x1000};uint32_t legacy=0,extent=0;
    image.words[0xe20/4]=0x03200402;image.words[0xe24/4]=0x20425355;
    image.words[0xe28/4]=0x00000401;image.words[0xe2c/4]=0;
    image.words[0xe30/4]=0x02000002;image.words[0xe34/4]=0x20425355;
    image.words[0xe38/4]=0x00000405;image.words[0xe3c/4]=0;
    uint32_t db=noisy ? 0x823 : 0x820,rt=noisy ? 0xa3f : 0xa20;
    struct xhci_geometry geometry;
    assert(!xhci_geometry_init(0x20,0x08001f7f,db,rt,0xc0000000,&geometry));
    assert(geometry.port_end==0x4a0 && geometry.doorbells==0x820 && geometry.doorbell_end==0xa20);
    assert(geometry.runtime==0xa20 && geometry.runtime_end==0xe20 && geometry.limit==0x1000);
    assert(!walk(&image,0x20,0x08001f7f,0xe20,db,rt,&legacy,&extent));
    assert(!legacy && extent==0xe40 && image.reads==4 && !image.outside);
    /* The last dword is inside the minimum; the next is not, even when
     * xECP advertises it. Unknown one-dword capabilities need no neighbor. */
    image.words[0xffc/4]=0xff;image.reads=0;
    assert(!walk(&image,0x20,0x08001f7f,0xffc,db,rt,&legacy,&extent));
    assert(extent==0x1000 && image.reads==1 && !image.outside);
    image.reads=0;
    assert(walk(&image,0x20,0x08001f7f,0x1000,db,rt,&legacy,&extent)==-EOPNOTSUPP);
    assert(!image.reads && !image.outside);
}
static void qemu(void)
{
    struct image image={.limit=0x4000};uint32_t legacy=0,extent=0;
    struct xhci_geometry geometry;
    assert(!xhci_geometry_init(0x40,0x10001040,0x2000,0x1000,0xc0000000,&geometry));
    assert(geometry.port_end==0x540 && geometry.doorbell_end==0x2104 &&
           geometry.runtime_end==0x1220 && geometry.limit==0x4000);
    image.words[0x20/4]=0x02000402;image.words[0x24/4]=0x20425355;
    image.words[0x28/4]=0x00000809;image.words[0x2c/4]=0;
    image.words[0x30/4]=0x03000002;image.words[0x34/4]=0x20425355;
    image.words[0x38/4]=0x00000801;image.words[0x3c/4]=0;
    assert(!walk(&image,0x40,0x10001040,0x20,0x2000,0x1000,&legacy,&extent));
    assert(!legacy && extent==0x40 && image.reads==4 && !image.outside);
    image.words[0x3ffc/4]=0xff;image.reads=0;
    assert(!walk(&image,0x40,0x10001040,0x3ffc,0x2000,0x1000,&legacy,&extent));
    assert(extent==0x4000 && image.reads==1 && !image.outside);
    image.reads=0;
    assert(walk(&image,0x40,0x10001040,0x4000,0x2000,0x1000,&legacy,&extent)==-EOPNOTSUPP);
    assert(!image.reads && !image.outside);
}
static void reserved(void)
{
    struct image image={.limit=0x4000};uint32_t legacy=0,extent=0;
    image.words[0x20/4]=0x02000002;image.words[0x28/4]=0x00000809;
    assert(!walk(&image,0x40,0x10001040,0x20,0x2003,0x101f,&legacy,&extent));
    assert(!legacy && extent==0x30 && image.reads==2 && !image.outside);
    vmware(true);
}
static void small(void)
{
    struct image image={.limit=0x800};uint32_t legacy=0,extent=0;
    struct xhci_geometry geometry;
    assert(!xhci_geometry_init(0x40,0x08000140,0x600,0x500,0xc0000000,&geometry));
    assert(geometry.port_end==0x4c0 && geometry.doorbell_end==0x704 &&
           geometry.runtime_end==0x540 && geometry.limit==0x800);
    assert(walk(&image,0x40,0x08000140,0x800,0x600,0x500,&legacy,&extent)==-EOPNOTSUPP);
    assert(!image.reads && !image.outside);
    image.words[0x7fc/4]=1;
    assert(walk(&image,0x40,0x08000140,0x7fc,0x600,0x500,&legacy,&extent)==-EOPNOTSUPP);
    assert(image.reads==1 && !image.outside);
    image.words[0x7f8/4]=1;image.words[0x7fc/4]=0;image.reads=0;
    assert(!walk(&image,0x40,0x08000140,0x7f8,0x600,0x500,&legacy,&extent));
    assert(legacy==0x7f8 && extent==0x800 && image.reads==2 && !image.outside);
}
static void aliases(void)
{
    struct image image={.limit=0x1000};uint32_t legacy=0,extent=0;
    /* VMware's final interrupter register ends immediately before 0xe20. */
    assert(walk(&image,0x20,0x08001f7f,0xe1c,0x820,0xa20,&legacy,&extent)==-EINVAL);
    assert(!image.reads && !image.outside);
    memset(&image,0,sizeof(image));image.limit=0x800;image.words[0x5fc/4]=1;
    assert(walk(&image,0x40,0x08000140,0x5fc,0x600,0x500,&legacy,&extent)==-EINVAL);
    assert(image.reads==1 && !image.outside); /* adjacent legacy control aliases doorbell */
    memset(&image,0,sizeof(image));image.limit=0x800;
    image.words[0x4e0/4]=2;image.words[0x4e8/4]=4u<<28;
    assert(!walk(&image,0x40,0x08000140,0x4e0,0x600,0x500,&legacy,&extent));
    assert(extent==0x500 && image.reads==2); /* full PSI extent ends at runtime */
    memset(&image,0,sizeof(image));image.limit=0x800;
    image.words[0x4f0/4]=2;image.words[0x4f8/4]=4u<<28;
    assert(walk(&image,0x40,0x08000140,0x4f0,0x600,0x500,&legacy,&extent)==-EINVAL);
    assert(image.reads==2 && !image.outside); /* PSI extent crosses runtime */
    memset(&image,0,sizeof(image));image.limit=0x800;
    image.words[0x5f0/4]=2;image.words[0x5f8/4]=2u<<28;
    assert(walk(&image,0x40,0x08000140,0x5f0,0x600,0x500,&legacy,&extent)==-EINVAL);
    assert(image.reads==2 && !image.outside); /* PSI extent crosses doorbells */
}
static void invalid(void)
{
    struct image image={.limit=0x4000};uint32_t legacy=0,extent=0;
    struct xhci_geometry geometry;
    assert(walk(&image,0x40,0x10001000,0x20,0x2000,0x1000,&legacy,&extent)==-EINVAL);
    assert(!image.reads);
    assert(walk(&image,0x40,0x10000040,0x20,0x2000,0x1000,&legacy,&extent)==-EINVAL);
    assert(!image.reads);
    assert(walk(&image,0x40,0x10040140,0x20,0x2000,0x1000,&legacy,&extent)==-EINVAL);
    assert(!image.reads); /* MaxIntrs > 1024 */
    assert(walk(&image,0x40,0x10001040,0x20,0x1000,0x1000,&legacy,&extent)==-EINVAL);
    assert(!image.reads);
    assert(walk(&image,0x40,0x10001040,0x20,0x1000000,0x1000,&legacy,&extent)==-EOPNOTSUPP);
    assert(!image.reads && !image.outside);
    assert(xhci_geometry_init(0x40,0x10001040,0x2000,0x1000,0xc0001000,&geometry)==-EOPNOTSUPP);
    assert(xhci_geometry_init(0x40,0x10001040,0x2000,0x1000,UINT64_MAX-0x3fffu,&geometry)==-EOPNOTSUPP);
    assert(xhci_geometry_init(0x40,0x10001040,0xfffffffcu,0x1000,0xc0000000,&geometry)==-EOPNOTSUPP);
    assert(!xhci_geometry_init(0x40,0x08040040,0x600,0x800,0xc0000000,&geometry));
    assert(geometry.runtime_end==0x8820 && geometry.limit==0x10000); /* MaxIntrs=1024 */
    assert(!xhci_geometry_init(0x40,0x08000140,0x600,0xff0000,0xc0000000,&geometry));
    assert(geometry.limit==0x1000000); /* maximum supported aperture */
}
int main(int argc,char **argv)
{
    assert(argc==2);
    if(!strcmp(argv[1],"vmware"))vmware(false);
    else if(!strcmp(argv[1],"reserved"))reserved();
    else if(!strcmp(argv[1],"qemu"))qemu();
    else if(!strcmp(argv[1],"small"))small();
    else if(!strcmp(argv[1],"aliases"))aliases();
    else if(!strcmp(argv[1],"invalid"))invalid();
    else assert(0);
    puts("USB xHCI firmware geometry case passed");return 0;
}
