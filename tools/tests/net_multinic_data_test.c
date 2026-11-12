/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../net/internal.h"
#include "../../net/utility.c"
#include "../../net/link/ethernet.c"
#include "../../net/link/arp.c"
#include "../../net/ipv4/udp.c"

const uint8_t net_broadcast_mac[6] = {255,255,255,255,255,255};
uint16_t net_ipv4_id;
static uint64_t clock_ms;
static bool ready[2] = {true,true};
static const uint8_t device_macs[2][6] = {{2,0,0,0,0,1},{2,0,0,0,0,2}};
static const uint8_t peer_macs[2][6] = {{2,1,0,0,0,1},{2,1,0,0,0,2}};
static struct net_interface devices[2] = {
    {.index=2,.up=true,.config={.local_ip=0x0a00020f,.subnet_mask=0xffffff00}},
    {.index=3,.up=true,.config={.local_ip=0x0a00030f,.subnet_mask=0xffffff00}}
};
static uint8_t wire[1514];
static uint32_t wire_length, wire_index, transmissions;
uint64_t time_uptime_ms(void) { return clock_ms; }
uint32_t netdev_next(uint32_t after) { return after < 2 ? 2 : after == 2 ? 3 : 0; }
const uint8_t *netdev_mac(uint32_t index) { assert(index==2 || index==3); return device_macs[index-2]; }
bool net_interface_is_ready(uint32_t index) { return index>=2 && index<=3 && ready[index-2]; }
struct net_interface *net_interface_get(uint32_t index) { return index>=2 && index<=3 ? &devices[index-2] : NULL; }
int net_interface_get_config(uint32_t index, struct reliefos_net_config *out)
{
    struct net_interface *device=net_interface_get(index);
    if (!device) return -LINUX_ENODEV;
    *out=device->config;
    return 0;
}
bool net_interface_name(uint32_t index, char out[16])
{ if (index<2 || index>3) return false; strcpy(out,index==2 ? "eth0" : "eth1"); return true; }
int netdev_send(uint32_t index, const void *frame, uint32_t length)
{
    assert(net_interface_is_ready(index) && length<=sizeof(wire));
    wire_index=index; wire_length=length; ++transmissions;
    memcpy(wire,frame,length);
    return 0;
}
int net_ipv4_route_lookup(uint32_t destination, uint32_t source, uint32_t scope, struct net_ipv4_path *out)
{
    uint32_t index=scope ? scope : (destination & 0xffffff00)==0x0a000300 ? 3 : 2;
    if (!net_interface_is_ready(index)) return -LINUX_ENETDOWN;
    *out=(struct net_ipv4_path){.ifindex=index,.source=source ? source : devices[index-2].config.local_ip,
        .destination=destination,.next_hop=destination};
    return 0;
}
void net_poll_once(struct net_arp_wait *arp, struct net_ping_wait *ping,
                    struct net_udp_wait *udp, struct net_tcp_wait *tcp)
{ (void)arp; (void)ping; (void)udp; (void)tcp; ++clock_ms; }
void net_udp_input(uint32_t source, uint32_t destination, uint16_t source_port,
                    uint16_t destination_port, const void *data, uint32_t length, int index)
{ (void)source; (void)destination; (void)source_port; (void)destination_port; (void)data; (void)length; (void)index; }

static void neighbor(uint32_t index, uint32_t ip, const uint8_t mac[6], uint16_t operation)
{
    uint8_t frame[42]={0};
    net_write_eth_on(index,frame,device_macs[index-2],ETH_TYPE_ARP);
    net_put_u16(frame+14,ARP_HTYPE_ETHERNET); net_put_u16(frame+16,ETH_TYPE_IPV4);
    frame[18]=6; frame[19]=4; net_put_u16(frame+20,operation);
    memcpy(frame+22,mac,6); net_put_u32(frame+28,ip);
    net_put_u32(frame+38,devices[index-2].config.local_ip);
    net_handle_arp_on(index,frame,sizeof(frame),NULL);
}
int main(void)
{
    /* The same next-hop address is valid on two separate Ethernet links. */
    const uint32_t peer=0x0a000002;
    neighbor(2,peer,peer_macs[0],ARP_OPER_REPLY);
    neighbor(3,peer,peer_macs[1],ARP_OPER_REPLY);
    uint8_t mac[6];
    assert(!net_resolve_mac_on(2,peer,20,mac) && !memcmp(mac,peer_macs[0],6));
    assert(!net_resolve_mac_on(3,peer,20,mac) && !memcmp(mac,peer_macs[1],6));
    assert(!transmissions);
    net_arp_cache_clear_on(2);
    assert(!net_resolve_mac_on(3,peer,20,mac) && !memcmp(mac,peer_macs[1],6));
    assert(!transmissions);
    neighbor(3,peer,peer_macs[1],ARP_OPER_REQUEST);
    assert(wire_index==3 && wire_length==42 && net_get_u16(wire+20)==ARP_OPER_REPLY);
    assert(!memcmp(wire+6,device_macs[1],6));
    assert(net_get_u32(wire+28)==devices[1].config.local_ip);
    neighbor(3,0x0a000302,peer_macs[1],ARP_OPER_REPLY);
    assert(!net_ipv4_send_udp_on(3,0,0x0a000302,55000,55001,"second",6));
    assert(wire_index==3 && wire_length==48 && !memcmp(wire,peer_macs[1],6));
    assert(!memcmp(wire+6,device_macs[1],6) && net_get_u32(wire+26)==devices[1].config.local_ip);
    uint32_t before=transmissions;
    ready[0]=false;
    assert(net_resolve_mac_on(2,peer,20,mac)==-LINUX_ENETDOWN);
    assert(!net_ipv4_send_udp_on(3,0,0x0a000302,55000,55001,"ok",2));
    assert(transmissions==before+1 && wire_index==3);
    puts("PASS per-interface ARP neighbors, reply identity, UDP output and independent link failure");
    return 0;
}
