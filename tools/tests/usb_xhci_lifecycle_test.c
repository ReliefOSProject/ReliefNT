/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../../drivers/usb/xhci/internal.h"

struct hardware { int unregister_error, halt_error, master_error; unsigned trace; void *dma; };
static int unregister_host(void *p) {struct hardware *h=p;h->trace=h->trace*10+1;return h->unregister_error;}
static void request_halt(void *p) {struct hardware *h=p;h->trace=h->trace*10+2;}
static int wait_halt(void *p) {struct hardware *h=p;h->trace=h->trace*10+3;return h->halt_error;}
static int disable_master(void *p) {struct hardware *h=p;h->trace=h->trace*10+4;return h->master_error;}

int main(void)
{
    const struct xhci_stop_ops ops={unregister_host,request_halt,wait_halt,disable_master};
    int memory=7;struct hardware h={.halt_error=-ETIMEDOUT,.dma=&memory};
    struct xhci_stop_state s={.registered=true};
    assert(xhci_quiesce(&s,&ops,&h)==-ETIMEDOUT);
    assert(h.trace==1234 && h.dma==&memory && s.failed && !s.stopped && !s.registered);
    h.trace=0;h.halt_error=0;h.master_error=-EIO;
    assert(xhci_quiesce(&s,&ops,&h)==-EIO && h.trace==234 && h.dma==&memory && !s.stopped);
    h.trace=0;h.master_error=0;
    assert(!xhci_quiesce(&s,&ops,&h) && h.trace==234 && s.stopped && h.dma==&memory);
    h.trace=0;assert(!xhci_quiesce(&s,&ops,&h) && !h.trace);
    s=(struct xhci_stop_state){.registered=true};h.unregister_error=-EBUSY;h.trace=0;
    assert(xhci_quiesce(&s,&ops,&h)==-EBUSY && h.trace==1 && !s.stopped && s.registered);
    puts("USB xHCI stop and DMA ownership tests passed");
}
