/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../drivers/usb/xhci/internal.h"

static struct xhci_device *current;
static unsigned allocations, frees, commands, clear_requests;
static int command_error;
static uint64_t next_dma=0x400000;
int xhci_dma_alloc(struct xhci_controller *controller,struct xhci_dma *dma,uint32_t pages)
{
    (void)controller;assert(pages==1 && !dma->physical);
    void *cpu=aligned_alloc(4096,4096);assert(cpu);memset(cpu,0,4096);
    *dma=(struct xhci_dma){.cpu=cpu,.physical=next_dma,.pages=1};next_dma+=4096;++allocations;
    return 0;
}
void xhci_dma_release(struct xhci_dma *dma)
{if(dma->physical){free(dma->cpu);++frees;}*dma=(struct xhci_dma){0};}
int xhci_command(struct xhci_controller *controller,struct xhci_trb trb,uint8_t *slot)
{
    (void)slot;assert(((trb.control>>10)&63u)==12 && (trb.control>>24)==current->slot_id);
    uint32_t *input=current->input.cpu;unsigned dci=3;
    assert(trb.lo==current->input.physical && input[0]==(1u<<dci));
    assert((input[1]&((1u<<dci)|1u))==((1u<<dci)|1u));
    assert((input[4u*(controller->stride/4u)+2]&~15u)!=current->endpoints[dci].dma.physical);
    ++commands;
    if(command_error)controller->stop.failed=true;
    return command_error;
}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{(void)lock;*flags=0;}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{(void)lock;(void)flags;}
int xhci_timer_start(struct xhci_timer *timer){*timer=(struct xhci_timer){0};return 0;}
bool xhci_timer_expired(struct xhci_timer *timer,uint64_t us){(void)timer;(void)us;return false;}
void xhci_write(struct xhci_controller *controller,uint32_t offset,uint32_t value)
{(void)controller;(void)offset;(void)value;}
void xhci_events(struct xhci_controller *controller)
{
    (void)controller;struct xhci_completion *c=&current->endpoints[1].completion;
    assert(current->endpoints[1].active && c->pending.count==2);
    volatile struct xhci_trb *setup=current->endpoints[1].ring.trbs;
    assert(setup[0].lo==0x102 && setup[0].hi==0x81);++clear_requests;
    uint64_t address=c->pending.trbs[1];
    struct xhci_trb event={.lo=(uint32_t)address,.hi=(uint32_t)(address>>32),.status=1u<<24,
        .control=((uint32_t)current->slot_id<<24)|(1u<<16)|(32u<<10)};
    assert(xhci_complete_event(c,&event));
}

int main(void)
{
    static uint32_t input[1024] __attribute__((aligned(4096)));
    static uint32_t output[1024] __attribute__((aligned(4096)));
    struct xhci_controller controller={.stride=32,.dma_mask=UINT32_MAX,.doorbells=0x1000};
    struct xhci_device device={.controller=&controller,.slot_id=7,
        .input={.cpu=input,.physical=0x300000,.pages=1},
        .output={.cpu=output,.physical=0x301000,.pages=1},
        .usb={.operations=&xhci_device_operations,.speed=USB_SPEED_HIGH}};
    current=&device;device.usb.host_context=&device;
    assert(!xhci_dma_alloc(&controller,&device.endpoints[1].dma,1));
    assert(!xhci_ring_init(&device.endpoints[1].ring,device.endpoints[1].dma.cpu,
                           device.endpoints[1].dma.physical,64));
    device.endpoints[1].configured=true;
    struct xhci_endpoint_state *ep=&device.endpoints[3];
    ep->endpoint=(struct usb_endpoint){.address=0x81,.max_packet=512};ep->configured=true;
    assert(!xhci_dma_alloc(&controller,&ep->dma,1));
    assert(!xhci_ring_init(&ep->ring,ep->dma.cpu,ep->dma.physical,64));
    uint64_t old=ep->dma.physical;unsigned before=frees;
    assert(!device.usb.operations->clear_halt(&device.usb,&ep->endpoint));
    assert(allocations==3 && commands==1 && clear_requests==1 && frees==before+1 && ep->dma.physical!=old);
    device.usb.disconnected=true;
    assert(device.usb.operations->clear_halt(&device.usb,&ep->endpoint)==-ENODEV);
    assert(allocations==3 && commands==1 && clear_requests==1);
    device.usb.disconnected=false;
    old=ep->dma.physical;before=frees;command_error=-ETIMEDOUT;
    assert(device.usb.operations->clear_halt(&device.usb,&ep->endpoint)==-ETIMEDOUT);
    assert(ep->dma.physical==old && ep->replacement.physical && frees==before &&
           clear_requests==1 && controller.stop.failed);
    xhci_dma_release(&ep->replacement);
    xhci_dma_release(&device.endpoints[1].dma);xhci_dma_release(&ep->dma);
    puts("USB xHCI idle halt reset and uncertain command ownership tests passed");
}
