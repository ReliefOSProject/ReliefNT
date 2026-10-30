/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/usb/xhci/internal.h"

static void transfer(void)
{
    struct xhci_pending pending={.trbs={0x100000},.trb_lengths={16},.count=1,
        .event_type=XHCI_EVENT_TRANSFER,.slot=4,.endpoint=3};
    struct xhci_trb event={.lo=0x100000,.status=(1u<<24)|4,
        .control=(4u<<24)|(3u<<16)|(32u<<10)|(1u<<1)|(0x7fu<<3)|(7u<<21)};
    assert(xhci_event_match(&pending,&event));
    event.control|=1u<<2;assert(!xhci_event_match(&pending,&event));event.control&=~(1u<<2);
    event.control^=1u<<24;assert(!xhci_event_match(&pending,&event));event.control^=1u<<24;
    event.control^=1u<<16;assert(!xhci_event_match(&pending,&event));event.control^=1u<<16;
    event.control^=1u<<10;assert(!xhci_event_match(&pending,&event));event.control^=1u<<10;
    event.lo=0x100001;assert(!xhci_event_match(&pending,&event));
    event.lo=0x100010;assert(!xhci_event_match(&pending,&event));
    event.lo=0x100000;event.status=(1u<<24)|17;assert(!xhci_event_match(&pending,&event));
    event.status=0;assert(!xhci_event_match(&pending,&event));
}
static void command(void)
{
    struct xhci_pending pending={.trbs={0x100000},.count=1,
        .event_type=XHCI_EVENT_COMMAND,.slot=4};
    struct xhci_trb event={.lo=0x10000f,.status=1u<<24,
        .control=(4u<<24)|(33u<<10)|(0x1ffu<<1)};
    assert(xhci_event_match(&pending,&event));
    event.control|=1u<<16;assert(!xhci_event_match(&pending,&event));event.control&=~(1u<<16);
    event.control^=1u<<24;assert(!xhci_event_match(&pending,&event));event.control^=1u<<24;
    event.control^=1u<<10;assert(!xhci_event_match(&pending,&event));event.control^=1u<<10;
    event.lo=0x10001f;assert(!xhci_event_match(&pending,&event));
}
int main(int argc,char **argv)
{
    assert(argc==2);
    if(!strcmp(argv[1],"transfer"))transfer();
    else if(!strcmp(argv[1],"command"))command();
    else assert(0);
    puts("USB xHCI event reserved padding case passed");return 0;
}
