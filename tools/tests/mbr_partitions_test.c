/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/partition/mbr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <linux/errno.h>
static uint8_t disk[1000][512];
static unsigned reads;
static int read_sector(void *context,uint64_t sector,uint8_t output[512])
{assert(context==disk);++reads;if(sector>=1000)return -EIO;memcpy(output,disk[sector],512);return 0;}
static void entry(unsigned sector,unsigned index,uint8_t type,uint32_t first,uint32_t count)
{
    uint8_t *p=&disk[sector][446+16*index];memset(p,0,16);p[4]=type;
    for(unsigned i=0;i<4;++i){p[8+i]=first>>(8*i);p[12+i]=count>>(8*i);}
    disk[sector][510]=0x55;disk[sector][511]=0xaa;
}
int main(void)
{
    struct mbr_partition partitions[128];
    assert(mbr_read_partitions(read_sector,disk,1000,partitions)==-ENOENT);
    entry(0,0,0x0c,10,100);entry(0,1,0x07,200,100);
    assert(!mbr_read_partitions(read_sector,disk,1000,partitions));
    assert(partitions[0].first==10 && partitions[0].count==100 && partitions[1].first==200);
    entry(0,2,0x0f,400,500);entry(400,0,0x83,1,99);entry(400,1,0x0f,200,300);
    entry(600,0,0x07,1,99);
    assert(!mbr_read_partitions(read_sector,disk,1000,partitions));
    assert(!partitions[2].type && partitions[4].first==401 && partitions[5].first==601);
    entry(600,1,0x0f,200,300);reads=0;
    assert(mbr_read_partitions(read_sector,disk,1000,partitions)==-EINVAL && reads<=4);
    for(unsigned i=0;i<128;++i)assert(!partitions[i].type);
    entry(600,1,0,0,0);entry(600,0,0x07,1,400);
    assert(mbr_read_partitions(read_sector,disk,1000,partitions)==-EINVAL);
    memset(disk,0,sizeof(disk));entry(0,0,0x0c,950,100);
    assert(mbr_read_partitions(read_sector,disk,1000,partitions)==-EINVAL);
    entry(0,0,0x0c,10,100);entry(0,1,0x07,50,200);
    assert(mbr_read_partitions(read_sector,disk,1000,partitions)==-EINVAL);
    memset(disk,0,sizeof(disk));entry(0,0,0xee,1,999);
    assert(mbr_read_partitions(read_sector,disk,1000,partitions)==-EOPNOTSUPP);
    /* A zero-length protective entry must still prevent hybrid fallback, even
     * after an earlier valid data entry was tentatively decoded. */
    memset(disk,0,sizeof(disk));entry(0,0,0x0c,10,100);entry(0,1,0xee,1,0);
    assert(mbr_read_partitions(read_sector,disk,1000,partitions)==-EOPNOTSUPP);
    for(unsigned i=0;i<128;++i)assert(!partitions[i].type);
    puts("PASS MBR primary slots, EBR-relative logical numbering, overlaps, container bounds, cycles, protective GPT and rollback");
}
