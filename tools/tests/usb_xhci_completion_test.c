/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../../drivers/usb/xhci/internal.h"

int main(void)
{
    struct xhci_completion c={.pending={.trbs={0x1000,0x1010,0x1020},
        .trb_lengths={8,16,0},.length=16,.count=3,.event_type=32,.slot=3,.endpoint=1},
        .control=true,.input=true};
    struct xhci_trb event={.lo=0x1010,.status=(13u<<24)|5,
        .control=(3u<<24)|(1u<<16)|(32u<<10)};
    assert(xhci_complete_event(&c,&event));
    assert(!c.done && c.short_seen && c.actual==11);
    event.lo=0x1020;event.status=1u<<24;
    assert(xhci_complete_event(&c,&event));
    assert(c.done && !c.result && c.actual==11 && !c.recovery);
    c=(struct xhci_completion){.pending={.trbs={0x1000,0x1010,0x1020},
        .trb_lengths={8,16,0},.length=16,.count=3,.event_type=32,.slot=3,.endpoint=1},
        .control=true,.input=true};
    assert(xhci_complete_event(&c,&event) && c.done && c.actual==16);
    c.done=false;event.lo=0x1000;event.status=6u<<24;
    assert(xhci_complete_event(&c,&event) && c.done && c.result==-EPIPE && c.recovery);
    c=(struct xhci_completion){.pending={.trbs={0x1000,0x1010},
        .trb_lengths={8,0},.length=0,.count=2,.event_type=32,.slot=3,.endpoint=1},.control=true};
    event.lo=0x1000;event.status=(6u<<24)|8;
    assert(xhci_complete_event(&c,&event) && c.done && c.result==-EPIPE);

    c=(struct xhci_completion){.pending={.trbs={0x2000,0x2010},
        .trb_lengths={4,16},.length=20,.count=2,.event_type=32,.slot=3,.endpoint=5},.input=true};
    event=(struct xhci_trb){.lo=0x2010,.status=(13u<<24)|7,
        .control=(3u<<24)|(5u<<16)|(32u<<10)};
    assert(xhci_complete_event(&c,&event) && c.done && !c.result && c.actual==13);
    c.done=false;c.short_seen=false;c.actual=0;event.lo=0x2000;event.status=1u<<24;
    assert(!xhci_complete_event(&c,&event) && !c.done);
    event.lo=0x2010;event.status=1u<<24;
    assert(xhci_complete_event(&c,&event) && c.done && c.actual==20);
    puts("USB xHCI control and bulk completion tests passed");
}
