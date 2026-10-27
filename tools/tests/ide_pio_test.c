/* SPDX-License-Identifier: Apache-2.0 */
#include "../../drivers/storage/ide/storage_ide.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
static unsigned count_writes,words,native_words,commands;
static uint8_t count_bytes[2],address_bytes[3][2],address_writes[3],select_byte,control;
static uint64_t wanted_lba;
static uint32_t wanted_count;
static uint8_t media[128*512],output[128*512+1];
static bool identify_mode;
static uint16_t identity[256];
static unsigned bulk_reads,bulk_writes,scalar_reads,scalar_writes;
void storage_memzero(void *p,size_t n){memset(p,0,n);}
uint8_t x86_64_inb(uint16_t port)
{
    assert(port==0x1f7 || port==0x3f6);
    return words<native_words ? IDE_STATUS_DRQ|IDE_STATUS_DRDY : IDE_STATUS_DRDY;
}
void x86_64_outb(uint8_t value,uint16_t port)
{
    if(port==0x3f6){control=value;return;}
    assert(port>=0x1f0 && port<=0x1f7);
    switch(port-0x1f0) {
    case 2:assert(count_writes<2);count_bytes[count_writes++]=value;break;
    case 3:case 4:case 5:{unsigned i=port-0x1f3;assert(address_writes[i]<2);address_bytes[i][address_writes[i]++]=value;break;}
    case 6:select_byte=value;break;
    case 7:
        ++commands;words=0;identify_mode=value==ATA_CMD_IDENTIFY_DEVICE;
        if(identify_mode){assert(control&2u);native_words=256;break;}
        assert(value==IDE_CMD_READ_PIO_EXT || value==IDE_CMD_WRITE_PIO_EXT || value==IDE_CMD_READ_PIO);
        if(value==IDE_CMD_READ_PIO_EXT || value==IDE_CMD_WRITE_PIO_EXT) {
            assert(count_writes==2);
            unsigned encoded=(unsigned)count_bytes[0]<<8|count_bytes[1];
            assert(encoded==wanted_count); /* Regression: old low byte was shifted twice. */
            assert((select_byte&0xe0u)==0xe0u);
            for(unsigned i=0;i<3;++i)assert(address_writes[i]==2 &&
                address_bytes[i][0]==((wanted_lba>>(24+i*8))&255) &&
                address_bytes[i][1]==((wanted_lba>>(i*8))&255));
        }
        native_words=wanted_count*256;break;
    default:break;
    }
}
uint16_t x86_64_inw(uint16_t port)
{
    assert(port==0x1f0 && words<native_words);
    if(identify_mode)return identity[words++];
    ++scalar_reads;
    unsigned index=words++*2;return media[index]|(uint16_t)media[index+1]<<8;
}
void x86_64_outw(uint16_t value,uint16_t port)
{
    ++scalar_writes;
    assert(port==0x1f0 && words<native_words);unsigned index=words++*2;
    assert(media[index]==(uint8_t)value && media[index+1]==(uint8_t)(value>>8));
}
/* The hardware boundary accepts one DRQ sector at a time. Emulate the whole
 * repeated-port operation, including an arbitrarily aligned CPU destination.
 * Keep scalar counters independent so a per-word VM-exit loop fails the test.
 */
void x86_64_insw(uint16_t port,void *buffer,size_t count)
{
    assert(port==0x1f0 && count==256 && words+count<=native_words);
    ++bulk_reads;
    memcpy(buffer,media+words*2,count*2);words+=(unsigned)count;
}
void x86_64_outsw(uint16_t port,const void *buffer,size_t count)
{
    assert(port==0x1f0 && count==256 && words+count<=native_words);
    ++bulk_writes;
    assert(!memcmp(buffer,media+words*2,count*2));words+=(unsigned)count;
}
static void begin(uint64_t lba,uint32_t count)
{
    count_writes=words=native_words=0;memset(address_writes,0,sizeof(address_writes));
    bulk_reads=bulk_writes=scalar_reads=scalar_writes=0;
    wanted_lba=lba;wanted_count=count;
}
int main(void)
{
    struct ide_device_info device={.present=1,.lba48=1,.command_base=0x1f0,.control_base=0x3f6,.sector_count=1ull<<48};
    for(unsigned i=0;i<sizeof(media);++i)media[i]=(uint8_t)(i*13+i/257);
    for(unsigned count=1;count<=128;count*=2) {
        begin(0x123456789aull,count);memset(output,0xa5,sizeof(output));
        assert(!ide_pio_transfer(&device,wanted_lba,count,output+1,0));
        assert(output[0]==0xa5 && !memcmp(output+1,media,count*512));
        assert(bulk_reads==count && !scalar_reads);
        if(count<128)assert(output[count*512+1]==0xa5);
        begin(0x123456789aull,count);
        assert(!ide_pio_transfer(&device,wanted_lba,count,output+1,1));
        assert(bulk_writes==count && !scalar_writes);
    }
    unsigned previous=commands;
    assert(ide_pio_transfer(&device,1ull<<48,1,output,0)==-22 && commands==previous);
    device.lba48=0;device.sector_count=0;
    assert(ide_pio_transfer(&device,0x0fffffffull,2,output,0)==-22 && commands==previous);
    begin(0,1);identity[83]=1u<<10;identity[100]=4096;identity[106]=0x4000;
    assert(!ide_identify_device(&device) && device.present && device.lba48 && device.sector_count==4096 && (control&2u));
    puts("PASS actual IDE PIO: LBA48 high/low count and address, unaligned CPU buffers, masks/ranges and polling IRQ disable");
}
