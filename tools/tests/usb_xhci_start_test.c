/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/usb/xhci/internal.h"

enum action { BME_WRITE,BME_READ,DCBAAP,CRCR,CONFIG,IMAN,ERSTSZ,ERDP,ERSTBA,RUN };
static enum action actions[32];static unsigned count;static bool mastering,deny_master,fail_erst;
static uint32_t hardware_status=1u, inject_before_run, inject_during_wait;
static unsigned wait_reads,timer_checks;
static bool running,force_timeout,clear_on_fatal;
static unsigned halt_requests,master_disables;
static int halt_result;
static struct xhci_controller *current;
static void record(enum action a){assert(count<32);actions[count++]=a;}
void pci_config_write16(uint8_t bus,uint8_t slot,uint8_t function,uint8_t offset,uint16_t value)
{
    assert(bus==0 && slot==4 && function==0 && offset==4 && (value&6u)==6u);
    assert(current->owned && !current->stop.stopped);
    mastering=!deny_master;record(BME_WRITE);
}
uint16_t pci_config_read16(uint8_t bus,uint8_t slot,uint8_t function,uint8_t offset)
{assert(bus==0 && slot==4 && function==0 && offset==4);record(BME_READ);return mastering ? 6u : 2u;}
void xhci_write(struct xhci_controller *c,uint32_t offset,uint32_t value)
{
    assert(c==current);
    if(offset==c->operational+0x38u){assert(value==c->slots);record(CONFIG);}
    else if(offset==c->runtime+0x20u){assert(!value);record(IMAN);}
    else if(offset==c->runtime+0x28u){assert(value==1);record(ERSTSZ);}
    else {assert(offset==c->operational && value==1);record(RUN);running=true;
        if(!force_timeout && !inject_during_wait)hardware_status&=~1u;}
}
void xhci_write64(struct xhci_controller *c,uint32_t offset,uint64_t value)
{
    assert(c==current);
    if(offset==c->operational+0x30u){assert(value==c->dcbaa.physical);record(DCBAAP);}
    else if(offset==c->operational+0x18u){assert(value==(c->commands.physical|1u));record(CRCR);}
    else if(offset==c->runtime+0x38u){assert(value==c->events.physical);record(ERDP);}
    else {assert(offset==c->runtime+0x30u && value==c->erst.physical);record(ERSTBA);
        if(fail_erst)c->stop.failed=true;hardware_status|=inject_before_run;}
}
uint32_t xhci_read(struct xhci_controller *c,uint32_t offset)
{
    assert(c==current && offset==c->operational+4u);
    if(running && inject_during_wait && ++wait_reads==2u) {
        hardware_status|=inject_during_wait;
        if(clear_on_fatal)hardware_status&=~1u;
    }
    return hardware_status;
}
int xhci_timer_start(struct xhci_timer *timer){*timer=(struct xhci_timer){0};timer_checks=0;return 0;}
bool xhci_timer_expired(struct xhci_timer *timer,uint64_t microseconds)
{(void)timer;(void)microseconds;return force_timeout || ++timer_checks==5u;}
static int unregister_host(void *ctx){(void)ctx;assert(0);return 0;}
static void request_halt(void *ctx){assert(ctx==current);++halt_requests;}
static int wait_halt(void *ctx){assert(ctx==current);return halt_result;}
static int disable_master(void *ctx){assert(ctx==current);++master_disables;mastering=false;return 0;}
static const struct xhci_stop_ops stop_ops={unregister_host,request_halt,wait_halt,disable_master};
static struct xhci_controller controller(void)
{
    return (struct xhci_controller){.pci={.slot=4},.owned=true,.operational=0x40,.runtime=0x1000,.slots=64,
        .dcbaa={.physical=0x3bf000},.commands={.physical=0x3c0000},
        .events={.physical=0x3c1000},.erst={.physical=0x3c2000},.stop={.stopped=true,.failed=true}};
}
int main(void)
{
    struct xhci_controller c=controller();current=&c;
    assert(!xhci_start_rings(&c,2));
    const enum action expected[]={BME_WRITE,BME_READ,DCBAAP,CRCR,CONFIG,IMAN,ERSTSZ,ERDP,ERSTBA,RUN};
    assert(count==sizeof(expected)/sizeof(expected[0]) && !memcmp(actions,expected,sizeof(expected)));
    assert(!c.stop.stopped && !c.stop.failed && mastering);
    c=controller();count=0;mastering=false;deny_master=true;
    assert(xhci_start_rings(&c,2)==-EIO);
    assert(count==2 && actions[0]==BME_WRITE && actions[1]==BME_READ);
    assert(!c.stop.stopped && c.stop.failed && !mastering);
    c=controller();count=0;mastering=false;deny_master=false;fail_erst=true;
    assert(xhci_start_rings(&c,2)==-EIO && !c.stop.stopped && c.stop.failed);
    for(unsigned i=0;i<count;++i)assert(actions[i]!=RUN);
    halt_result=-ETIMEDOUT;
    assert(xhci_quiesce(&c.stop,&stop_ops,&c)==-ETIMEDOUT);
    assert(!c.stop.stopped && c.stop.failed && !mastering && halt_requests==1 && master_disables==1);
    halt_result=0;
    assert(!xhci_quiesce(&c.stop,&stop_ops,&c) && c.stop.stopped && master_disables==2);
    c=controller();count=0;mastering=false;fail_erst=false;hardware_status=1u;running=false;force_timeout=true;
    assert(xhci_start_rings(&c,2)==-ETIMEDOUT && !c.stop.stopped && c.stop.failed);
    assert(actions[count-1]==RUN);
    force_timeout=false;
    for(unsigned bit=2;bit<=12;bit+=10) {
        c=controller();count=0;mastering=false;running=false;hardware_status=1u;
        inject_before_run=1u<<bit;inject_during_wait=0;wait_reads=0;
        assert(xhci_start_rings(&c,2)==-EIO && c.stop.failed && !c.stop.stopped);
        for(unsigned i=0;i<count;++i)assert(actions[i]!=RUN);
        halt_result=-ETIMEDOUT;
        assert(xhci_quiesce(&c.stop,&stop_ops,&c)==-ETIMEDOUT);
        assert(!c.stop.stopped && c.stop.failed && !mastering);
        halt_result=0;
        assert(!xhci_quiesce(&c.stop,&stop_ops,&c) && c.stop.stopped && !mastering);
        for(unsigned clear=0;clear<2;++clear) {
            c=controller();count=0;mastering=false;running=false;hardware_status=1u;
            inject_before_run=0;inject_during_wait=1u<<bit;wait_reads=0;clear_on_fatal=clear;
            assert(xhci_start_rings(&c,2)==-EIO && c.stop.failed && !c.stop.stopped);
            assert(wait_reads==2u && timer_checks<5u); /* fatal with HCH set or clear */
            assert(!xhci_quiesce(&c.stop,&stop_ops,&c) && c.stop.stopped && !mastering);
        }
    }
    inject_before_run=inject_during_wait=0;
    c=controller();count=0;c.owned=false;
    assert(xhci_start_rings(&c,2)==-EINVAL && !count && c.stop.stopped);
    puts("USB xHCI owned DMA starts with BME and dequeue before ERSTBA");
}
