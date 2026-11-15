/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the real receive dispatcher against finite NIC completion queues. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../net/internal.h"
#include <reliefnt/net_ipv6.h>

extern void net_poll_rx_budget(uint32_t) __attribute__((weak));
static unsigned frames[2], pending[2], delivered[2], attempts, timers;
static unsigned dropped;
static bool down;
uint32_t netdev_rx_take(uint32_t after)
{
    for (unsigned pass=0;pass<2;++pass) for (unsigned i=0;i<2;++i)
        if (pending[i] && (pass || i+2>after)) { pending[i]=0; return i+2; }
    return 0;
}
void netdev_rx_notify(uint32_t id) { assert(id>=2 && id<=3); pending[id-2]=1; }
uint32_t netdev_next(uint32_t after) { return after<2 ? 2 : after==2 ? 3 : 0; }
bool net_interface_is_up(uint32_t id) { return id>=2 && id<=3 && !down; }
int netdev_poll(uint32_t id,void *frame,uint32_t capacity,uint32_t *length)
{
    assert(id>=2 && id<=3 && capacity>=64); ++attempts; *length=0;
    if (!frames[id-2]) return 0;
    --frames[id-2];
    if (dropped) { --dropped; return -74; }
    memset(frame,0,64); *length=64; return 1;
}
int netdev_poll_notified(uint32_t id,void *frame,uint32_t capacity,uint32_t *length)
{ return netdev_poll(id,frame,capacity,length); }
void net_packet_input_on(uint32_t id,const void *frame,uint32_t length)
{ assert(frame && length==64); ++delivered[id-2]; }
int net_frame_for_us_on(uint32_t id,const uint8_t *frame) { (void)id;(void)frame;return 0; }
uint16_t net_get_u16(const uint8_t *data) { return (uint16_t)data[0]<<8|data[1]; }
void net_handle_arp_on(uint32_t id,const uint8_t *p,uint32_t n,struct net_arp_wait *w)
{ (void)id;(void)p;(void)n;(void)w; }
void net_handle_ipv4_on(uint32_t id,const uint8_t *p,uint32_t n,struct net_ping_wait *a,
    struct net_udp_wait *b,struct net_tcp_wait *c) { (void)id;(void)p;(void)n;(void)a;(void)b;(void)c; }
void net_ipv6_input_on(uint32_t id,const uint8_t *p,uint32_t n) { (void)id;(void)p;(void)n; }
void net_tcp_service_sockets(void) { ++timers; }
void net_ipv6_poll(void) {}
void net_device_retry_registration(void) {}
bool net_device_registration_pending(void) { return false; }
bool net_tcp_maintenance_pending(void) { return false; }
bool net_ipv6_maintenance_pending(void) { return false; }
uint64_t time_ticks(void) { return 0; }
int main(void)
{
    if (!net_poll_rx_budget) { fputs("FAIL: budgeted IRQ receive service is unavailable\n",stderr); return 1; }
    frames[0]=frames[1]=100; pending[0]=pending[1]=1;
    net_poll_rx_budget(32);
    assert(attempts==32 && delivered[0] && delivered[1]);
    assert(delivered[0]+delivered[1]==32 && pending[0] && pending[1]);
    assert(timers && timers<=32);
    dropped=100; unsigned before=attempts;
    net_poll_rx_budget(32); assert(attempts-before==32);
    dropped=0; frames[0]=frames[1]=0;
    before=attempts; net_poll_rx_budget(32);
    assert(attempts-before==2 && !pending[0] && !pending[1]);
    before=attempts; net_poll_rx_budget(32); assert(attempts==before);
    net_poll_rx_budget(0); assert(attempts==before);
    down=true; frames[0]=1; pending[0]=1;
    unsigned old_delivered=delivered[0]; before=attempts;
    net_poll_rx_budget(32);
    assert(attempts-before==2 && !pending[0] && delivered[0]==old_delivered);
    puts("PASS network IRQ polling: bounded bursts, NIC fairness, drops and idle completion");
    return 0;
}
