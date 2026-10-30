/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../../drivers/usb/xhci/internal.h"

int main(void)
{
    struct xhci_trb memory[4] = {{0}};
    struct xhci_ring ring;
    assert(xhci_ring_init(&ring,memory,0x100010,4)==-EINVAL);
    assert(xhci_ring_init(&ring,memory,0x10ffc0,8)==-EINVAL); /* 64KiB crossing */
    assert(!xhci_ring_init(&ring,memory,0x100000,4));
    struct xhci_trb td[2]={{.lo=0x11,.status=4,.control=(1u<<10)|(1u<<4)},
                            {.lo=0x22,.status=16,.control=1u<<10}};
    struct xhci_pending pending={.event_type=32,.slot=4,.endpoint=3,.length=20};
    assert(!xhci_ring_submit(&ring,td,2,&pending));
    assert(pending.count==2 && pending.trbs[0]==0x100000 && pending.trbs[1]==0x100010);
    assert(memory[0].lo==0x11 && memory[1].lo==0x22);
    assert((memory[0].control&1u) && (memory[1].control&1u));
    assert(ring.used==2 && ring.producer==2 && ring.cycle==1);
    assert(xhci_ring_submit(&ring,td,2,&pending)==-ENOSPC);
    assert(ring.used==2 && ring.producer==2);
    assert(!xhci_ring_reclaim(&ring,2,false));
    assert(!xhci_ring_submit(&ring,td,2,&pending));
    assert(pending.trbs[0]==0x100020 && pending.trbs[1]==0x100000);
    assert(memory[2].control&1u);
    assert(!(memory[0].control&1u));
    assert(memory[3].lo==0x100000 && !memory[3].hi);
    assert((memory[3].control & ((0x3fu<<10)|0x13u))==((6u<<10)|0x13u));
    assert(ring.used==2 && ring.producer==1 && ring.cycle==0);

    struct xhci_trb event={.lo=0x100000,.status=(1u<<24),
        .control=(4u<<24)|(3u<<16)|(32u<<10)};
    assert(xhci_event_match(&pending,&event));
    event.lo=0x100010;assert(!xhci_event_match(&pending,&event));
    event.lo=0x100001;assert(!xhci_event_match(&pending,&event));
    event.lo=0x100000;event.control^=1u<<16;assert(!xhci_event_match(&pending,&event));
    event.control^=1u<<16;event.control|=1u<<2;assert(!xhci_event_match(&pending,&event));
    event.control&=~(1u<<2);event.status=(1u<<24)|21;assert(!xhci_event_match(&pending,&event));
    event.status=(1u<<24)|17;assert(!xhci_event_match(&pending,&event)); /* residual exceeds this TRB */
    event.status=1u<<24;event.control|=(1u<<1)|(0x7fu<<3)|(7u<<21);
    assert(xhci_event_match(&pending,&event)); /* reserved read padding is ignored */
    event.control|=1u<<2;assert(!xhci_event_match(&pending,&event)); /* ED is defined */
    event.control&=~(1u<<2);
    event.status=0;assert(!xhci_event_match(&pending,&event));

    struct xhci_pending command={.trbs={0x100000},.count=1,.event_type=33};
    event=(struct xhci_trb){.lo=0x100000,.status=1u<<24,.control=33u<<10};
    assert(xhci_event_match(&command,&event));
    event.control|=1u<<16;assert(!xhci_event_match(&command,&event)); /* no virtual functions */
    event.control&=~(1u<<16);event.control|=0x1ffu<<1;event.lo|=15u;
    assert(xhci_event_match(&command,&event)); /* control and pointer RsvdZ */
    event.lo=0x10001f;assert(!xhci_event_match(&command,&event)); /* different TRB */
    event.lo=0x10000f;event.control|=1u<<16;assert(!xhci_event_match(&command,&event));

    xhci_ring_quarantine(&ring);
    assert(xhci_ring_submit(&ring,td,1,&pending)==-EBUSY);
    assert(xhci_ring_reclaim(&ring,2,false)==-EBUSY);
    assert(ring.used==2 && ring.quarantined);
    assert(!xhci_ring_reclaim(&ring,2,true));
    assert(!ring.used && !ring.quarantined);
    assert(xhci_ring_reclaim(&ring,1,true)==-EINVAL);
    assert(!xhci_ring_init(&ring,memory,0x100000,4));
    assert(!xhci_ring_submit(&ring,td,2,&pending));
    assert(!xhci_ring_reclaim(&ring,2,false));
    td[0].control&=~(1u<<4); /* control stages are distinct TDs */
    assert(!xhci_ring_submit(&ring,td,2,&pending));
    assert(!(memory[3].control&(1u<<4)));
    puts("USB xHCI ring ownership and event tests passed");
}
