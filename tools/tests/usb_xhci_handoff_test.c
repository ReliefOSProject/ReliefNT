/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../../drivers/usb/xhci/internal.h"

struct firmware {uint32_t value;unsigned reads,byte_writes,control_writes;bool release;};
static int read_reg(void *p,uint32_t offset,uint32_t *value)
{
    struct firmware *f=p;assert(offset==0x80);++f->reads;
    if(f->release && f->reads>=3)f->value&=~(1u<<16);
    *value=f->value;return 0;
}
static int write8(void *p,uint32_t offset,uint8_t value)
{
    struct firmware *f=p;assert(offset==0x83 && value==(f->byte_writes ? 0xa0 : 0xa1));++f->byte_writes;
    f->value=(f->value&0x00ffffffu)|((uint32_t)value<<24);return 0;
}
static int write32(void *p,uint32_t offset,uint32_t value)
{struct firmware *f=p;assert(offset==0x84 && value==0xe0000000u);++f->control_writes;return 0;}
int xhci_timer_start(struct xhci_timer *timer){*timer=(struct xhci_timer){0};return 0;}
bool xhci_timer_expired(struct xhci_timer *timer,uint64_t us)
{assert(us==1000000);return ++timer->ticks>5;}

int main(void)
{
    struct firmware f={.value=0xa0010001u,.release=true};bool acquired=false;
    assert(!xhci_firmware_handoff(read_reg,write8,write32,&f,0x80,&acquired) && acquired);
    assert(f.byte_writes==1 && f.control_writes==1 && !(f.value&(1u<<16)) && (f.value&(1u<<24)));
    f=(struct firmware){.value=0xa0010001u};
    assert(xhci_firmware_handoff(read_reg,write8,write32,&f,0x80,&acquired)==-EBUSY && !acquired);
    assert(f.byte_writes==2 && !f.control_writes && (f.value&(1u<<16)) && !(f.value&(1u<<24)));
    puts("USB xHCI firmware ownership handshake tests passed");
}
