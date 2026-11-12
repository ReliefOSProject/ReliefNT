/* SPDX-License-Identifier: Apache-2.0 */
/** @file Packet-level IPv6 fixture: only NIC, clock, heap and transport are mocked. */
#include <reliefnt/net_ipv6.h>
#include <reliefnt/net_device.h>
#include <reliefnt/net_interface.h>
#include <reliefnt/random.h>
#include "../../net/internal.h"
#include <linux/errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fixture_interface_up = true;
bool *net_interface_legacy_up(void) { return &fixture_interface_up; }
static uint64_t now_ms;
static uint32_t random_word;
static bool random_unavailable;
static bool nic_ready = true;
static bool first_nic = true;
static uint8_t nic_mac[6] = {0x52,0x54,0x00,0x12,0x34,0x56};
static bool second_nic;
static uint32_t second_index=3;
static uint32_t extra_nics;
static unsigned failed_allocations;
static uint8_t second_mac[6] = {0x52,0x54,0x00,0x12,0x34,0x57};
static uint8_t sent[128][1536];
static uint32_t sent_len[128];
static uint32_t sent_ifindex[128];
static unsigned sent_count, udp_count, raw_count;
static uint32_t last_udp_ifindex, last_error_ifindex;
static unsigned socket_error_count;
static uint8_t queued_frame[1536];
static uint32_t queued_length;
const uint8_t net_broadcast_mac[6]={255,255,255,255,255,255};
static uint8_t received[65536];
static uint32_t received_len;

/** @brief Return the fake monotonic clock. @return Uptime in milliseconds. */
uint64_t time_uptime_ms(void) { return now_ms; }
/** @brief Supply controlled random words or model an absent hardware generator.
 * @param buffer Output storage. @param length Output bytes. @return Zero or an unavailable error. */
int kernel_random_fill(void *buffer,size_t length)
{ if(random_unavailable)return -LINUX_EIO;assert(length==sizeof(random_word));memcpy(buffer,&random_word,length);return 0; }
/** @brief Report the fixture NIC state. @return One if available. */
int e1000_is_ready(void) { return nic_ready; }
/** @brief Return the fixture Ethernet address. @return Borrowed six bytes. */
const uint8_t *e1000_mac(void) { return nic_mac; }
/** @brief Capture a transmitted frame. @param frame Borrowed bytes. @param len Length.
 * @return Zero when captured. */
int e1000_send(const void *frame, uint32_t len)
{ assert(sent_count < 128 && len <= 1536); memcpy(sent[sent_count],frame,len); sent_len[sent_count++]=len; return 0; }
uint32_t netdev_next(uint32_t after)
{
    uint32_t next=0;
    if(after<2 && first_nic)next=2;
    if(second_nic && after<second_index && (!next || second_index<next))next=second_index;
    uint32_t extra=after<100?100:after+1;
    if(extra_nics && extra>=100 && extra-100<extra_nics && (!next || extra<next))next=extra;
    return next;
}
int netdev_is_ready(uint32_t index)
{ return index==2?(first_nic && nic_ready):((index==second_index && second_nic) || (index>=100 && index-100<extra_nics)); }
const uint8_t *netdev_mac(uint32_t index) { return index==second_index?second_mac:nic_mac; }
int netdev_send(uint32_t index,const void *frame,uint32_t len)
{ assert(index==2 || (index==second_index && second_nic));assert(sent_count<128);
  sent_ifindex[sent_count]=index;return e1000_send(frame,len); }
bool net_interface_exists(uint32_t index)
{ return index==1 || (index==2 && first_nic) || (index==second_index && second_nic) || (index>=100 && index-100<extra_nics); }
bool net_interface_is_up(uint32_t index) { return index==1 || (net_interface_exists(index) && net_interface_up); }
bool net_interface_is_ready(uint32_t index) { return index==1 || (net_interface_is_up(index) && netdev_is_ready(index)); }
bool net_device_registration_pending(void) { return false; }
void net_device_retry_registration(void) { }
/** @brief Allocate kernel fixture memory. @param size Requested bytes. @return Owned memory. */
void *kernel_malloc(size_t size)
{ if(failed_allocations){--failed_allocations;return NULL;}return calloc(1,size); }
/** @brief Free fixture memory. @param memory Owned allocation or NULL. */
void kernel_free(void *memory) { free(memory); }
/** @brief Announce a state change to the fixture. */
void netlink_ipv6_changed(void) { }
/** @brief Capture UDP delivery. @param s Source. @param d Destination. @param sp Source port.
 * @param dp Destination port. @param data Borrowed bytes. @param len Length.
 * @param ifindex Interface. @param hops Hop limit. @return True for a listening port. */
bool net_udp6_input(const struct in6_addr *s,const struct in6_addr *d,uint16_t sp,
                    uint16_t dp,const void *data,uint32_t len,uint32_t ifindex,uint8_t hops)
{ (void)s;(void)d;(void)sp;(void)hops; if(dp!=9000)return false;
  assert(len<=sizeof(received));memcpy(received,data,len);received_len=len;last_udp_ifindex=ifindex;++udp_count;return true; }
/** @brief Ignore checked TCP. @param s Source. @param d Destination. @param p Bytes.
 * @param n Length. @param i Interface. */
void net_tcp6_input(const struct in6_addr *s,const struct in6_addr *d,const void *p,uint32_t n,uint32_t i)
{ (void)s;(void)d;(void)p;(void)n;(void)i; }
/** @brief Count checked raw delivery. @param s Source. @param d Destination. @param p Protocol.
 * @param b Bytes. @param n Length. @param i Interface. @param h Hop limit. */
void net_raw6_input(const struct in6_addr *s,const struct in6_addr *d,uint8_t p,const void *b,
                    uint32_t n,uint32_t i,uint8_t h)
{ (void)s;(void)d;(void)p;(void)b;(void)n;(void)i;(void)h;++raw_count; }
/** @brief Ignore socket errors. @param p Quote. @param n Length. @param e Error. @param m MTU. */
void net_ipv6_socket_error(const uint8_t *p,uint32_t n,int e,uint32_t m)
{ (void)p;(void)n;(void)e;(void)m; }
void net_ipv6_socket_error_on(uint32_t i,const uint8_t *p,uint32_t n,int e,uint32_t m)
{ last_error_ifindex=i;++socket_error_count;net_ipv6_socket_error(p,n,e,m); }
/** @brief Copy bytes. @param d Destination. @param s Source. @param n Length. */
void net_memcpy(void *d,const void *s,uint32_t n) { memcpy(d,s,n); }
/** @brief Clear bytes. @param d Destination. @param n Length. */
void net_memzero(void *d,uint32_t n) { memset(d,0,n); }
/** @brief Compare bytes. @param a First. @param b Second. @param n Length. @return Equality. */
int net_memeq(const uint8_t *a,const uint8_t *b,uint32_t n) { return !memcmp(a,b,n); }
/** @brief Read a wire integer. @param p Two bytes. @return Host-order integer. */
uint16_t net_get_u16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
/** @brief Read a wire integer. @param p Four bytes. @return Host-order integer. */
uint32_t net_get_u32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
/** @brief Write a wire integer. @param p Two bytes. @param v Host-order value. */
void net_put_u16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v; }
/** @brief Write a wire integer. @param p Four bytes. @param v Host-order value. */
void net_put_u32(uint8_t *p,uint32_t v) { p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v; }
/** @brief Accumulate network-order words. @param sum Initial sum. @param p Bytes. @param n Length.
 * @return Unfolded checksum accumulator. */
uint32_t net_checksum_partial(uint32_t sum,const uint8_t *p,uint32_t n)
{ while(n>1){sum+=((uint32_t)p[0]<<8)|p[1];p+=2;n-=2;}if(n)sum+=(uint32_t)p[0]<<8;return sum; }
/** @brief Fold the checksum. @param sum Accumulator. @return Network numeric checksum. */
uint16_t net_checksum_finish(uint32_t sum)
{ while(sum>>16)sum=(sum&65535)+(sum>>16);return (uint16_t)~sum; }
/** @brief Return fake timer ticks. @return Tick count. */
uint64_t time_ticks(void) { return now_ms/10; }
/** @brief Fetch one queued fixture NIC frame. @param frame Output. @param capacity Bytes available.
 * @param len Output length. @return One for a frame. */
int e1000_poll(void *frame,uint32_t capacity,uint32_t *len)
{ if(!queued_length){*len=0;return 0;}assert(queued_length<=capacity);memcpy(frame,queued_frame,queued_length);
  *len=queued_length;queued_length=0;return 1; }
int netdev_poll(uint32_t index,void *frame,uint32_t capacity,uint32_t *len)
{ if(index==2)return e1000_poll(frame,capacity,len);*len=0;return 0; }
/** @brief Ignore packet socket mirroring. @param frame Bytes. @param length Byte length. */
void net_packet_input(const void *frame,uint32_t length) { (void)frame;(void)length; }
void net_packet_input_on(uint32_t index,const void *frame,uint32_t length)
{ (void)index;net_packet_input(frame,length); }
/** @brief Ignore ARP for this fixture. @param frame Bytes. @param length Length. @param wait Waiter. */
void net_handle_arp(const uint8_t *frame,uint32_t length,struct net_arp_wait *wait)
{ (void)frame;(void)length;(void)wait; }
void net_handle_arp_on(uint32_t index,const uint8_t *frame,uint32_t length,struct net_arp_wait *wait)
{ (void)index;net_handle_arp(frame,length,wait); }
/** @brief Ignore IPv4 for this fixture. @param frame Bytes. @param length Length.
 * @param ping Waiter. @param udp_wait Waiter. @param tcp Waiter. */
void net_handle_ipv4(const uint8_t *frame,uint32_t length,struct net_ping_wait *ping,
                     struct net_udp_wait *udp_wait,struct net_tcp_wait *tcp)
{ (void)frame;(void)length;(void)ping;(void)udp_wait;(void)tcp; }
void net_handle_ipv4_on(uint32_t index,const uint8_t *frame,uint32_t length,struct net_ping_wait *ping,
                        struct net_udp_wait *udp_wait,struct net_tcp_wait *tcp)
{ (void)index;net_handle_ipv4(frame,length,ping,udp_wait,tcp); }
/** @brief No IPv4 TCP work is active. @return False. */
bool net_tcp_maintenance_pending(void) { return false; }
/** @brief Ignore empty IPv4 TCP maintenance. */
void net_tcp_service_sockets(void) { }

#ifdef IPV6_NO_IMPLEMENTATION
/** @brief Exercise the expected absent implementation failure. */
void net_ipv6_init(void) { }
/** @brief Return no configured addresses. @param out Optional output. @param n Capacity. @return Zero. */
uint32_t net_ipv6_addresses(struct net_ipv6_address *out,uint32_t n) { (void)out;(void)n;return 0; }
#endif

#ifndef IPV6_NO_IMPLEMENTATION
/** @brief Set a 16-byte address. @param p Destination. @param bytes Source bytes. */
static void address(struct in6_addr *p,const uint8_t bytes[16]) { memcpy(p->s6_addr,bytes,16); }
/** @brief Build an IPv6 UDP message with a valid mandatory checksum.
 * @param out Output message. @param source Source. @param dest Destination.
 * @param port Destination port. @param data Payload. @param n Payload bytes. */
static void udp(uint8_t *out,const struct in6_addr *source,const struct in6_addr *dest,
                uint16_t port,const uint8_t *data,uint32_t n)
{ net_put_u16(out,7000);net_put_u16(out+2,port);net_put_u16(out+4,(uint16_t)(n+8));
  net_put_u16(out+6,0);if(out+8!=data)memcpy(out+8,data,n);uint16_t c=net_ipv6_checksum(source,dest,17,out,n+8);
  net_put_u16(out+6,c?c:0xffff); }
/** @brief Build an Ethernet IPv6 packet. @param out Output frame. @param src Source.
 * @param dst Destination. @param next Next header. @param hop Hop limit.
 * @param payload Payload. @param n Payload bytes. @return Ethernet length. */
static uint32_t packet(uint8_t *out,const struct in6_addr *src,const struct in6_addr *dst,
                       uint8_t next,uint8_t hop,const uint8_t *payload,uint32_t n)
{ memset(out,0,54+n);memcpy(out,nic_mac,6);out[6]=0x02;out[11]=1;net_put_u16(out+12,0x86dd);
  out[14]=0x60;net_put_u16(out+18,(uint16_t)n);out[20]=next;out[21]=hop;
  memcpy(out+22,src->s6_addr,16);memcpy(out+38,dst->s6_addr,16);memcpy(out+54,payload,n);return 54+n; }
/** @brief Find the latest sent ICMPv6 message of a type. @param type ICMPv6 type.
 * @return Captured index or -1. */
static int last_icmp(uint8_t type)
{ for(int i=(int)sent_count-1;i>=0;--i)if(sent_len[i]>=55 && sent[i][20]==58 && sent[i][54]==type)return i;return -1; }
/** @brief Find a captured MLDv2 record for a group after an index. @param first First index.
 * @param group Group address. @param type Record type. @return Index or -1. */
static int mld_record(unsigned first,const struct in6_addr *group,uint8_t type)
{ for(unsigned i=first;i<sent_count;i++)if(sent_len[i]>=90 && sent[i][20]==0 && sent[i][54]==58 &&
    sent[i][62]==143 && sent[i][70]==type && !memcmp(sent[i]+74,group->s6_addr,16))return (int)i;
  return -1; }
/** @brief Find a neighbor's published NUD state. @param target Address. @return Linux NUD state or zero. */
static uint32_t neighbor_state(const struct in6_addr *target)
{ struct net_ipv6_neighbor neighbors[16];uint32_t n=net_ipv6_neighbors(neighbors,16);
  for(uint32_t i=0;i<n && i<16;i++)if(!memcmp(&neighbors[i].address,target,16))return neighbors[i].state;
  return 0; }

/** @brief Count probes for a particular neighbor on a particular link. */
static unsigned probes_since(unsigned first,uint32_t ifindex,const struct in6_addr *target)
{
    unsigned count=0;
    for(unsigned i=first;i<sent_count;i++)
        if(sent_ifindex[i]==ifindex && sent_len[i]>=78 && sent[i][20]==58 && sent[i][54]==135 &&
           !memcmp(sent[i]+62,target->s6_addr,16))++count;
    return count;
}

/** @brief Find an MLD report for a group and interface. */
static bool saw_mld(uint32_t ifindex,const struct in6_addr *group,uint8_t type)
{
    for(unsigned i=0;i<sent_count;i++){
        if(sent_ifindex[i]!=ifindex || sent_len[i]<86 || sent[i][20]!=0 || sent[i][54]!=58 || sent[i][62]!=type)continue;
        unsigned group_offset=type==143?74:70;
        if(sent_len[i]>=group_offset+16 && !memcmp(sent[i]+group_offset,group->s6_addr,16))return true;
    }
    return false;
}

/** @brief Exercise the real IPv6 core with duplicate scoped addresses on two NICs. */
static void test_multiple_interfaces(void)
{
    const uint8_t text[]="hello";
    const uint8_t mac2[6]={0x02,0,0,0,0,2},mac3[6]={0x02,0,0,0,0,3};
    struct in6_addr shared={.s6_addr={0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0x12,0x34}};
    struct in6_addr peer={.s6_addr={0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0x42}};
    struct in6_addr peer2=peer;peer2.s6_addr[15]=0x43;
    struct in6_addr all={.s6_addr={0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0,1}};
    uint8_t frame[1536],message[64],na[32]={136,0,0,0,0x60},fragment[32];
    struct net_ipv6_path path2,path3;
    second_nic=true;first_nic=true;nic_ready=true;random_word=0;
    net_ipv6_init();sent_count=0;net_ipv6_poll();
    uint8_t early_ra[16]={134};net_put_u16(early_ra+2,net_ipv6_checksum(&peer,&all,58,early_ra,sizeof(early_ra)));
    uint32_t len=packet(frame,&peer,&all,58,255,early_ra,sizeof(early_ra));net_ipv6_input_on(2,frame,len);
    now_ms+=1000;net_ipv6_poll();

    /* DAD and MLD frames must use each NIC's own source MAC. */
    bool saw2=false,saw3=false,rs2=false,rs3=false;
    for(unsigned i=0;i<sent_count;i++){
        if(sent_ifindex[i]==2){assert(!memcmp(sent[i]+6,nic_mac,6));saw2=true;}
        if(sent_ifindex[i]==3){assert(!memcmp(sent[i]+6,second_mac,6));saw3=true;}
        if(sent_len[i]>=55 && sent[i][20]==58 && sent[i][54]==133){
            if(sent_ifindex[i]==2)rs2=true;
            if(sent_ifindex[i]==3)rs3=true;
        }
    }
    assert(saw2 && saw3 && !rs2 && rs3);
    struct net_ipv6_address initial[16];uint32_t initial_count=net_ipv6_addresses(initial,16);
    struct in6_addr auto3={0};bool found_auto3=false;
    for(uint32_t i=0;i<initial_count && i<16;i++)if(initial[i].ifindex==3 && initial[i].address.s6_addr[0]==0xfe){
        auto3=initial[i].address;found_auto3=true;
    }
    assert(found_auto3);
    assert(net_ipv6_route(&peer,0,&auto3,&path3)==0 && path3.ifindex==3);
    assert(net_ipv6_route(&all,0,&auto3,&path3)==0 && path3.ifindex==3);
    struct in6_addr interface_group={.s6_addr={0xff,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0x77}};
    assert(net_ipv6_multicast_change(&interface_group,3,1)==0);
    assert(net_ipv6_route(&interface_group,3,&auto3,&path3)==0);
    udp(message,&auto3,&interface_group,9000,text,5);
    sent_count=0;unsigned interface_delivered=udp_count;
    assert(net_ipv6_output(&path3,17,message,13,1,0,false,true)==13);
    net_ipv6_poll();assert(udp_count==interface_delivered+1 && last_udp_ifindex==3);
    assert(sent_count==0); /* interface-local multicast never goes on the wire */
    udp(message,&peer,&interface_group,9000,text,5);
    len=packet(frame,&peer,&interface_group,17,1,message,13);net_ipv6_input_on(3,frame,len);
    assert(udp_count==interface_delivered+1);
    struct net_ipv6_address local={0};local.address=shared;local.prefix_length=64;
    local.flags=NET_IPV6_ADDR_NODAD;local.preferred_until=local.valid_until=UINT64_MAX;
    local.ifindex=2;assert(net_ipv6_address_update(&local,false)==0);
    local.ifindex=3;assert(net_ipv6_address_update(&local,false)==0);
    assert(net_ipv6_route(&shared,2,NULL,&path2)==0 && path2.ifindex==2);
    assert(net_ipv6_route(&shared,3,NULL,&path3)==0 && path3.ifindex==3);
    assert(net_ipv6_route(&peer,2,&shared,&path2)==0);
    assert(net_ipv6_route(&peer,3,&shared,&path3)==0);

    /* A neighbor advertisement must flush only the matching link's queue. */
    udp(message,&shared,&peer,9000,text,5);sent_count=0;
    assert(net_ipv6_output(&path2,17,message,13,64,0,false,false)==13);
    assert(net_ipv6_output(&path3,17,message,13,64,0,false,false)==13);
    assert(probes_since(0,2,&peer)==1 && probes_since(0,3,&peer)==1);
    memcpy(na+8,peer.s6_addr,16);na[24]=2;na[25]=1;memcpy(na+26,mac3,6);
    net_put_u16(na+2,net_ipv6_checksum(&peer,&shared,58,na,sizeof(na)));
    len=packet(frame,&peer,&shared,58,255,na,sizeof(na));
    unsigned before=sent_count;net_ipv6_input_on(3,frame,len);
    assert(sent_count==before+1 && sent_ifindex[before]==3);
    assert(!memcmp(sent[before],mac3,6) && !memcmp(sent[before]+6,second_mac,6));
    struct net_ipv6_neighbor neighbors[16];uint32_t count=net_ipv6_neighbors(neighbors,16);
    bool pending2=false,resolved3=false;
    for(uint32_t i=0;i<count && i<16;i++)if(!memcmp(&neighbors[i].address,&peer,16)){
        if(neighbors[i].ifindex==2 && neighbors[i].state==1)pending2=true;
        if(neighbors[i].ifindex==3 && neighbors[i].state==2)resolved3=true;
    }
    assert(pending2 && resolved3);
    memcpy(na+26,mac2,6);na[2]=na[3]=0;
    net_put_u16(na+2,net_ipv6_checksum(&peer,&shared,58,na,sizeof(na)));
    len=packet(frame,&peer,&shared,58,255,na,sizeof(na));before=sent_count;
    net_ipv6_input_on(2,frame,len);
    assert(sent_count==before+1 && sent_ifindex[before]==2 && !memcmp(sent[before],mac2,6));

    /* Fragment IDs remain unique for identical endpoints sent over both NICs. */
    uint8_t large[2000]={0};sent_count=0;
    assert(net_ipv6_output(&path2,17,large,sizeof(large),64,0,false,false)==(int)sizeof(large));
    assert(net_ipv6_output(&path3,17,large,sizeof(large),64,0,false,false)==(int)sizeof(large));
    assert(sent_count==4 && sent_ifindex[0]==2 && sent_ifindex[2]==3);
    assert(net_get_u32(sent[0]+58)!=net_get_u32(sent[2]+58));

    /* Same source, destination and fragment ID must form two separate datagrams. */
    udp(message,&peer,&shared,9000,text,5);memset(fragment,0,sizeof(fragment));
    fragment[0]=17;net_put_u16(fragment+2,1);net_put_u32(fragment+4,0x12345678);
    memcpy(fragment+8,message,8);len=packet(frame,&peer,&shared,44,64,fragment,16);
    unsigned delivered=udp_count;net_ipv6_input_on(2,frame,len);
    net_put_u16(fragment+2,8);memcpy(fragment+8,message+8,5);
    len=packet(frame,&peer,&shared,44,64,fragment,13);net_ipv6_input_on(3,frame,len);
    assert(udp_count==delivered);
    net_put_u16(fragment+2,1);memcpy(fragment+8,message,8);
    len=packet(frame,&peer,&shared,44,64,fragment,16);net_ipv6_input_on(3,frame,len);
    assert(udp_count==delivered+1 && last_udp_ifindex==3);
    net_put_u16(fragment+2,8);memcpy(fragment+8,message+8,5);
    len=packet(frame,&peer,&shared,44,64,fragment,13);net_ipv6_input_on(2,frame,len);
    assert(udp_count==delivered+2 && last_udp_ifindex==2);

    /* RA MTU and retransmission values, and later PMTU, are link-local state. */
    uint8_t ra[24]={134,0,0,0};ra[16]=5;ra[17]=1;
    net_put_u32(ra+12,2500);net_put_u32(ra+20,1500);
    net_put_u16(ra+2,net_ipv6_checksum(&peer,&all,58,ra,sizeof(ra)));
    len=packet(frame,&peer,&all,58,255,ra,sizeof(ra));net_ipv6_input_on(2,frame,len);
    net_put_u32(ra+12,1000);net_put_u32(ra+20,1400);ra[2]=ra[3]=0;
    net_put_u16(ra+2,net_ipv6_checksum(&peer,&all,58,ra,sizeof(ra)));
    len=packet(frame,&peer,&all,58,255,ra,sizeof(ra));net_ipv6_input_on(3,frame,len);
    assert(net_ipv6_route(&peer2,2,&shared,&path2)==0 && path2.mtu==1500);
    assert(net_ipv6_route(&peer2,3,&shared,&path3)==0 && path3.mtu==1400);
    uint8_t ptb[48]={2};net_put_u32(ptb+4,1280);ptb[8]=0x60;
    memcpy(ptb+16,shared.s6_addr,16);memcpy(ptb+32,peer2.s6_addr,16);
    net_put_u16(ptb+2,net_ipv6_checksum(&peer,&shared,58,ptb,sizeof(ptb)));
    len=packet(frame,&peer,&shared,58,64,ptb,sizeof(ptb));net_ipv6_input_on(3,frame,len);
    assert(net_ipv6_route(&peer2,2,&shared,&path2)==0 && path2.mtu==1500);
    assert(net_ipv6_route(&peer2,3,&shared,&path3)==0 && path3.mtu==1280);

    /* SLAAC constructs the address and installs its prefix route on the ingress NIC. */
    uint8_t prefix_ra[48]={134};prefix_ra[16]=3;prefix_ra[17]=4;prefix_ra[18]=64;prefix_ra[19]=0xc0;
    net_put_u32(prefix_ra+20,1200);net_put_u32(prefix_ra+24,600);
    prefix_ra[32]=0x20;prefix_ra[33]=1;prefix_ra[34]=0x0d;prefix_ra[35]=0xb8;prefix_ra[37]=3;
    net_put_u16(prefix_ra+2,net_ipv6_checksum(&peer,&all,58,prefix_ra,sizeof(prefix_ra)));
    len=packet(frame,&peer,&all,58,255,prefix_ra,sizeof(prefix_ra));net_ipv6_input_on(3,frame,len);
    struct net_ipv6_address addresses[16];count=net_ipv6_addresses(addresses,16);
    bool found_slaac=false;
    for(uint32_t i=0;i<count && i<16;i++)if(!memcmp(addresses[i].address.s6_addr,prefix_ra+32,8)){
        assert(addresses[i].ifindex==3 && addresses[i].address.s6_addr[8]==(uint8_t)(second_mac[0]^2));
        assert(addresses[i].address.s6_addr[15]==second_mac[5]);found_slaac=true;
    }
    assert(found_slaac);
    struct net_ipv6_route routes[24];count=net_ipv6_routes(routes,24);bool found_route=false;
    for(uint32_t i=0;i<count && i<24;i++)if(routes[i].prefix_length==64 && routes[i].ifindex==3 &&
        !memcmp(routes[i].destination.s6_addr,prefix_ra+32,8))found_route=true;
    assert(found_route);

    /* MLDv1 compatibility learned on one NIC does not change the other NIC's reports. */
    uint8_t query[32]={58,0,5,2,0,0,1,0,130};
    net_put_u16(query+10,net_ipv6_checksum(&peer,&all,58,query+8,24));
    len=packet(frame,&peer,&all,0,1,query,sizeof(query));net_ipv6_input_on(2,frame,len);
    struct in6_addr group={.s6_addr={0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0x77,0x77}};
    assert(net_ipv6_multicast_change(&group,2,1)==0);
    assert(net_ipv6_multicast_change(&group,3,1)==0);
    sent_count=0;net_ipv6_poll();
    assert(saw_mld(2,&group,131) && saw_mld(3,&group,143));

    /* Failure for the same next hop on one link must not fail the other queue. */
    struct in6_addr failing_peer=peer;failing_peer.s6_addr[15]=0x44;
    assert(net_ipv6_route(&failing_peer,2,&shared,&path2)==0);
    assert(net_ipv6_route(&failing_peer,3,&shared,&path3)==0);
    udp(message,&shared,&failing_peer,9000,text,5);sent_count=0;
    assert(net_ipv6_output(&path2,17,message,13,64,0,false,false)==13);
    assert(net_ipv6_output(&path3,17,message,13,64,0,false,false)==13);
    unsigned errors=socket_error_count;
    for(unsigned i=0;i<3;i++){now_ms+=1000;net_ipv6_poll();}
    assert(socket_error_count==errors+1 && last_error_ifindex==3);
    memcpy(na+8,failing_peer.s6_addr,16);memcpy(na+26,mac2,6);na[2]=na[3]=0;
    net_put_u16(na+2,net_ipv6_checksum(&failing_peer,&shared,58,na,sizeof(na)));
    len=packet(frame,&failing_peer,&shared,58,255,na,sizeof(na));before=sent_count;
    net_ipv6_input_on(2,frame,len);
    assert(sent_count==before+1 && sent_ifindex[before]==2);

    assert(net_ipv6_route(&peer2,2,&shared,&path2)==0);
    assert(net_ipv6_route(&peer2,3,&shared,&path3)==0);
    udp(message,&shared,&peer2,9000,text,5);sent_count=0;
    assert(net_ipv6_output(&path2,17,message,13,64,0,false,false)==13);
    assert(net_ipv6_output(&path3,17,message,13,64,0,false,false)==13);
    before=sent_count;now_ms+=1000;net_ipv6_poll();
    assert(probes_since(before,2,&peer2)==0 && probes_since(before,3,&peer2)==1);

    /* Unplugging one device fails only its queue and retains the other link and reassembly. */
    udp(message,&peer,&shared,9000,text,5);memset(fragment,0,sizeof(fragment));
    fragment[0]=17;net_put_u16(fragment+2,1);net_put_u32(fragment+4,0x87654321);
    memcpy(fragment+8,message,8);len=packet(frame,&peer,&shared,44,64,fragment,16);
    net_ipv6_input_on(2,frame,len);net_ipv6_input_on(3,frame,len);delivered=udp_count;
    errors=socket_error_count;first_nic=false;net_ipv6_detached_on(2);
    assert(socket_error_count==errors+1 && last_error_ifindex==2);
    assert(net_ipv6_route(&peer2,3,&shared,&path3)==0 && path3.mtu==1280);
    net_put_u16(fragment+2,8);memcpy(fragment+8,message+8,5);
    len=packet(frame,&peer,&shared,44,64,fragment,13);net_ipv6_input_on(3,frame,len);
    assert(udp_count==delivered+1 && last_udp_ifindex==3);
    memcpy(na+8,peer2.s6_addr,16);memcpy(na+26,mac3,6);na[2]=na[3]=0;
    net_put_u16(na+2,net_ipv6_checksum(&peer2,&shared,58,na,sizeof(na)));
    len=packet(frame,&peer2,&shared,58,255,na,sizeof(na));before=sent_count;
    net_ipv6_input(frame,len); /* legacy wrapper now selects the first surviving NIC */
    assert(sent_count==before+1 && sent_ifindex[before]==3);

    /* Stable NIC indices are not array offsets and need not be dense. */
    second_nic=false;net_ipv6_detached_on(3);second_index=1000;second_nic=true;
    net_ipv6_link_changed_on(second_index,true);sent_count=0;net_ipv6_poll();now_ms+=1000;net_ipv6_poll();
    assert(net_ipv6_route(&peer,second_index,NULL,&path3)==0 && path3.ifindex==second_index);
    udp(message,&path3.source,&peer,9000,text,5);before=sent_count;
    assert(net_ipv6_output(&path3,17,message,13,64,0,false,false)==13);
    assert(sent_count==before+1 && sent_ifindex[before]==second_index);
    second_nic=false;net_ipv6_detached_on(second_index);
}

/** @brief Link state must not impose a second, independent NIC count limit. */
static void test_many_interfaces(void)
{
    first_nic=false;second_nic=false;extra_nics=17;net_ipv6_init();
    struct net_ipv6_address addresses[16];uint32_t count=net_ipv6_addresses(addresses,16);
    bool removed=false;
    for(uint32_t i=0;i<count && i<16;i++)if(addresses[i].ifindex==100){
        assert(net_ipv6_address_update(&addresses[i],true)==0);removed=true;break;
    }
    assert(removed);
    struct net_ipv6_address address={0};address.address.s6_addr[0]=0xfe;
    address.address.s6_addr[1]=0x80;address.address.s6_addr[15]=1;
    address.ifindex=116;address.prefix_length=64;address.flags=NET_IPV6_ADDR_NODAD;
    address.preferred_until=address.valid_until=UINT64_MAX;
    assert(net_ipv6_address_update(&address,false)==0);
    struct in6_addr peer=address.address;peer.s6_addr[15]=2;
    struct net_ipv6_path path;
    assert(net_ipv6_route(&peer,116,&address.address,&path)==0 && path.ifindex==116);
    for(uint32_t i=100;i<117;i++)net_ipv6_detached_on(i);
    extra_nics=0;
}

/** @brief Allocation failures are reported and the maintenance pump retries registration. */
static void test_link_allocation_retry(void)
{
    net_ipv6_init();second_index=1000;second_nic=true;failed_allocations=1;
    assert(net_ipv6_link_changed_on(second_index,true)==-LINUX_ENOMEM);
    struct net_ipv6_address addresses[16];uint32_t count=net_ipv6_addresses(addresses,16);
    for(uint32_t i=0;i<count && i<16;i++)assert(addresses[i].ifindex!=second_index);
    assert(net_ipv6_maintenance_pending());
    sent_count=0;net_ipv6_poll();now_ms+=1000;net_ipv6_poll();
    struct in6_addr peer={.s6_addr={0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0x42}};
    struct net_ipv6_path path;
    assert(net_ipv6_route(&peer,second_index,NULL,&path)==0 && path.ifindex==second_index);
    second_nic=false;net_ipv6_detached_on(second_index);
}
#endif

/** @brief Exercise real host packet/state processing with deterministic NIC and time.
 * @return Zero on success. */
int main(void)
{
    struct net_ipv6_address entries[16];
    net_ipv6_init();
    uint32_t count=net_ipv6_addresses(entries,16);
    bool loop=false;
    for(uint32_t i=0;i<count && i<16;i++)if(entries[i].ifindex==1 && entries[i].address.s6_addr[15]==1)loop=true;
    if(!loop){fputs("FAIL IPv6 ::1 is always configured\n",stderr);return 1;}
#ifndef IPV6_NO_IMPLEMENTATION
    const uint8_t onebytes[16]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    const uint8_t peerbytes[16]={0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0x42};
    struct in6_addr one,peer,local;address(&one,onebytes);address(&peer,peerbytes);
    struct net_ipv6_path path;
    assert(net_ipv6_route(&one,0,NULL,&path)==0 && path.ifindex==1);
    uint8_t msg[4096];const uint8_t hello[]="hello";udp(msg,&one,&one,9000,hello,5);
    assert(net_get_u16(msg+6)==0x7d80);
    assert(net_ipv6_output(&path,17,msg,13,-1,0,false,false)==13);
    assert(udp_count==0);net_ipv6_poll();assert(udp_count==1 && received_len==5 && !memcmp(received,hello,5));
    net_ipv6_poll();int dad=last_icmp(135);assert(dad>=0);
    assert(sent[dad][21]==255 && !memcmp(sent[dad]+22,(uint8_t[16]){0},16));
    assert(sent[dad][38]==0xff && sent[dad][39]==2 && sent[dad][49]==1 && sent[dad][50]==0xff);
    now_ms=1000;net_ipv6_poll();
    count=net_ipv6_addresses(entries,16);bool found=false;
    for(uint32_t i=0;i<count && i<16;i++)if(entries[i].ifindex==2 && entries[i].address.s6_addr[0]==0xfe){
        assert(!(entries[i].flags & NET_IPV6_ADDR_TENTATIVE));local=entries[i].address;found=true;break;}
    assert(found && net_ipv6_route(&peer,2,NULL,&path)==0);
    udp(msg,&local,&peer,9000,hello,5);unsigned before=sent_count;
    assert(net_ipv6_output(&path,17,msg,13,64,0,false,false)==13);
    assert(sent_count>before && last_icmp(135)>=0);
    uint8_t na[32]={136,0,0,0,0x60},frame[4096];const uint8_t remote_mac[6]={2,0,0,0,0,1};
    memcpy(na+8,peer.s6_addr,16);na[24]=2;na[25]=1;memcpy(na+26,remote_mac,6);
    net_put_u16(na+2,net_ipv6_checksum(&peer,&local,58,na,32));
    uint32_t len=packet(frame,&peer,&local,58,255,na,32);net_ipv6_input(frame,len);
    assert(sent_count>before && sent[sent_count-1][20]==17 && !memcmp(sent[sent_count-1],remote_mac,6));
    assert(net_ipv6_checksum(&local,&peer,17,sent[sent_count-1]+54,13)==0);
    udp(msg,&peer,&local,9000,hello,5);len=packet(frame,&peer,&local,17,42,msg,13);
    unsigned delivered=udp_count;net_ipv6_input(frame,len);assert(udp_count==delivered+1);
    frame[60]=0;frame[61]=0;net_ipv6_input(frame,len);assert(udp_count==delivered+1);
    uint8_t large[2408];memset(large+8,0xa5,2400);udp(large,&local,&peer,9000,large+8,2400);
    before=sent_count;assert(net_ipv6_output(&path,17,large,sizeof(large),64,0,false,false)==(int)sizeof(large));
    assert(sent_count==before+2 && sent[before][20]==44 && sent[before+1][20]==44);
    assert(sent_len[before]<=1514 && sent_len[before+1]<=1514);
    assert(net_ipv6_output(&path,17,large,sizeof(large),64,0,true,false)==-LINUX_EMSGSIZE);

    /* Reassembly accepts out-of-order fragments and discards all overlapping pieces. */
    udp(large,&peer,&local,9000,large+8,2400);
    uint8_t frag[1460]={17,0,0,0,0,0,0,7};
    memcpy(frag+8,large+1448,960);net_put_u16(frag+2,1448);
    len=packet(frame,&peer,&local,44,64,frag,968);delivered=udp_count;net_ipv6_input(frame,len);
    net_put_u16(frag+2,1);memcpy(frag+8,large,1448);
    len=packet(frame,&peer,&local,44,64,frag,1456);net_ipv6_input(frame,len);
    assert(udp_count==delivered+1 && received_len==2400 && received[0]==0xa5);
    net_put_u32(frag+4,8);net_ipv6_input(frame,0); /* no packet is read when length is zero */
    len=packet(frame,&peer,&local,44,64,frag,1456);net_ipv6_input(frame,len);net_ipv6_input(frame,len);
    net_put_u16(frag+2,1448);memcpy(frag+8,large+1448,960);
    len=packet(frame,&peer,&local,44,64,frag,968);net_ipv6_input(frame,len);assert(udp_count==delivered+1);
    /* An atomic fragment with the same identification is independent of that poisoned queue. */
    uint8_t atomic[21]={17,0,0,0,0,0,0,8};udp(atomic+8,&peer,&local,9000,hello,5);
    len=packet(frame,&peer,&local,44,64,atomic,sizeof(atomic));net_ipv6_input(frame,len);assert(udp_count==delivered+2);

    /* A link-local RA must progress through the tick pump even with no IPv4 TCP work. */
    const uint8_t allbytes[16]={0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    struct in6_addr all;address(&all,allbytes);uint8_t ra[48]={134,0,0,0,64,0,0,30};
    ra[16]=3;ra[17]=4;ra[18]=64;ra[19]=0xc0;net_put_u32(ra+20,86400);net_put_u32(ra+24,3600);
    ra[32]=0x20;ra[33]=1;ra[34]=0x0d;ra[35]=0xb8;ra[39]=1;
    net_put_u16(ra+2,net_ipv6_checksum(&peer,&all,58,ra,sizeof(ra)));
    queued_length=packet(queued_frame,&peer,&all,58,255,ra,sizeof(ra));
    unsigned raw_before=raw_count;now_ms=1010;net_tcp_maintenance();assert(!queued_length && raw_count>raw_before);
    count=net_ipv6_addresses(entries,16);struct in6_addr global={0};bool have_global=false;
    for(uint32_t i=0;i<count && i<16;i++)if(entries[i].address.s6_addr[0]==0x20){
        global=entries[i].address;assert(entries[i].flags&NET_IPV6_ADDR_TENTATIVE);have_global=true;}
    assert(have_global);
    /* RFC 6980 rejects fragmented ND before raw sockets see it, including atomic fragments. */
    uint8_t other_ra[48],ra_frag[56]={58,0,0,0,0,0,0,77};memcpy(other_ra,ra,sizeof(other_ra));
    other_ra[32]=0x30;other_ra[2]=other_ra[3]=0;
    net_put_u16(other_ra+2,net_ipv6_checksum(&peer,&all,58,other_ra,sizeof(other_ra)));
    memcpy(ra_frag+8,other_ra,sizeof(other_ra));
    uint32_t addr_before=net_ipv6_addresses(NULL,0);raw_before=raw_count;
    len=packet(frame,&peer,&all,44,255,ra_frag,sizeof(ra_frag));net_ipv6_input(frame,len);
    assert(raw_count==raw_before && net_ipv6_addresses(NULL,0)==addr_before);
    net_put_u16(ra_frag+2,1);len=packet(frame,&peer,&all,44,255,ra_frag,32);net_ipv6_input(frame,len);
    net_put_u16(ra_frag+2,24);memcpy(ra_frag+8,other_ra+24,24);
    len=packet(frame,&peer,&all,44,255,ra_frag,32);net_ipv6_input(frame,len);
    assert(raw_count==raw_before && net_ipv6_addresses(NULL,0)==addr_before);
    now_ms=2010;net_ipv6_poll();
    count=net_ipv6_addresses(entries,16);for(uint32_t i=0;i<count && i<16;i++)if(!memcmp(&entries[i].address,&global,16))
        assert(!(entries[i].flags&NET_IPV6_ADDR_TENTATIVE));
    struct in6_addr external=global;external.s6_addr[7]=2;external.s6_addr[15]=9;
    assert(net_ipv6_route(&external,0,NULL,&path)==0 && !memcmp(&path.next_hop,&peer,16));
    /* An unauthenticated short valid lifetime cannot reduce an existing address below two hours. */
    net_put_u32(ra+20,60);net_put_u32(ra+24,60);ra[2]=ra[3]=0;
    net_put_u16(ra+2,net_ipv6_checksum(&peer,&all,58,ra,sizeof(ra)));
    len=packet(frame,&peer,&all,58,255,ra,sizeof(ra));net_ipv6_input(frame,len);
    count=net_ipv6_addresses(entries,16);for(uint32_t i=0;i<count && i<16;i++)if(!memcmp(&entries[i].address,&global,16))
        assert(entries[i].valid_until>=now_ms+7200000);

    /* A manually configured /64 supplies its own connected route. */
    struct net_ipv6_address manual={0};manual.address=global;manual.address.s6_addr[7]=0x22;
    manual.ifindex=2;manual.prefix_length=64;manual.flags=NET_IPV6_ADDR_NODAD;
    manual.preferred_until=manual.valid_until=UINT64_MAX;
    assert(net_ipv6_address_update(&manual,false)==0);
    struct in6_addr direct=manual.address;direct.s6_addr[15]^=3;
    assert(net_ipv6_route(&direct,2,NULL,&path)==0 && !memcmp(&path.next_hop,&direct,16));
    assert(net_ipv6_address_update(&manual,true)==0);

    /* Multicast is Ethernet-filtered then IPv6-membership-filtered and loop policy is per send. */
    const uint8_t groupbytes[16]={0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0xfb};
    struct in6_addr group;address(&group,groupbytes);assert(net_ipv6_multicast_change(&group,2,1)==0);
    assert(net_ipv6_route(&group,2,NULL,&path)==0);udp(msg,&path.source,&group,9000,hello,5);
    delivered=udp_count;assert(net_ipv6_output(&path,17,msg,13,1,0,false,false)==13);
    net_ipv6_poll();assert(udp_count==delivered);
    assert(net_ipv6_output(&path,17,msg,13,1,0,false,true)==13);net_ipv6_poll();assert(udp_count==delivered+1);
    /* Option action 10 sends an error even for multicast; 11 suppresses that error. */
    uint8_t option_packet[21]={17,0,0x80,0,1,2,0,0};udp(option_packet+8,&peer,&all,9000,hello,5);
    len=packet(frame,&peer,&all,0,64,option_packet,sizeof(option_packet));before=sent_count;
    net_ipv6_input(frame,len);assert(sent_count==before+1 && sent[sent_count-1][54]==4);
    option_packet[2]=0xc0;len=packet(frame,&peer,&all,0,64,option_packet,sizeof(option_packet));
    before=sent_count;net_ipv6_input(frame,len);assert(sent_count==before);
    uint8_t nested_error[16]={58,0,0x80,0,1,2,0,0,1,4,0,0,0,0,0,0};
    net_put_u16(nested_error+10,net_ipv6_checksum(&peer,&local,58,nested_error+8,8));
    len=packet(frame,&peer,&local,0,64,nested_error,sizeof(nested_error));
    before=sent_count;net_ipv6_input(frame,len);assert(sent_count==before);
    bool mld=false;for(unsigned i=0;i<sent_count;i++)if(sent_len[i]>=90 && sent[i][20]==0 &&
        sent[i][21]==1 && sent[i][54]==58 && sent[i][56]==5 && sent[i][62]==143)mld=true;
    assert(mld);
    uint8_t othermac[14]={2,0,0,0,0,77};net_put_u16(othermac+12,0x86dd);assert(!net_frame_for_us(othermac));
    othermac[0]=othermac[1]=0x33;assert(net_frame_for_us(othermac));
    net_put_u16(othermac+12,0x0800);assert(!net_frame_for_us(othermac));

    /* A first fragment that expires causes ICMPv6 reassembly time exceeded. */
    net_put_u16(frag+2,1);net_put_u32(frag+4,9);memcpy(frag+8,large,1448);
    len=packet(frame,&peer,&local,44,64,frag,1456);net_ipv6_input(frame,len);
    now_ms+=60001;before=sent_count;net_ipv6_poll();int timeout=last_icmp(3);
    assert(sent_count>before && timeout>=(int)before && sent[timeout][55]==1);

    /* ::1 and a full 65535-byte IPv6 payload work without a NIC. */
    net_ipv6_detached();nic_ready=false;assert(net_ipv6_route(&one,0,NULL,&path)==0);
    uint8_t *maximum=malloc(65535);assert(maximum);memset(maximum+8,0x7b,65527);
    udp(maximum,&one,&one,9000,maximum+8,65527);delivered=udp_count;
    assert(net_ipv6_output(&path,17,maximum,65535,64,0,false,false)==65535);
    maximum[8]=0;free(maximum);assert(udp_count==delivered);now_ms+=10;net_tcp_maintenance();
    assert(udp_count==delivered+1 && received_len==65527 && received[0]==0x7b);
    nic_ready=true;now_ms+=10;net_tcp_maintenance();count=net_ipv6_addresses(entries,16);found=false;
    for(uint32_t i=0;i<count && i<16;i++)if(entries[i].ifindex==2 && entries[i].address.s6_addr[0]==0xfe)found=true;
    assert(found);now_ms+=1000;net_ipv6_poll();
    const uint8_t peer2bytes[16]={0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0x43};
    struct in6_addr peer2;address(&peer2,peer2bytes);assert(net_ipv6_route(&peer2,2,NULL,&path)==0);
    udp(msg,&path.source,&peer2,9000,hello,5);
    assert(net_ipv6_output(&path,17,msg,13,64,0,false,false)==13);
    for(unsigned i=0;i<3;i++){now_ms+=1000;net_ipv6_poll();}
    before=sent_count;assert(net_ipv6_output(&path,17,msg,13,64,0,false,false)==13);
    assert(sent_count>before && last_icmp(135)>=(int)before);
    /* Randomized DAD, first RS and MLD delays must be bounded, and DAD sends its MLD report first. */
    random_word=UINT32_MAX;now_ms+=10000;net_ipv6_init();sent_count=0;
    uint64_t start=now_ms;net_ipv6_poll();assert(last_icmp(135)<0);
    while(last_icmp(135)<0 && now_ms-start<1000){now_ms+=10;net_ipv6_poll();}
    dad=last_icmp(135);assert(dad>=0 && now_ms-start>0 && now_ms-start<=1000);
    uint64_t dad_time=now_ms;struct in6_addr solicited={0};
    memcpy(solicited.s6_addr,sent[dad]+38,16);int joined=mld_record(0,&solicited,4);
    assert(joined>=0 && joined<dad && !memcmp(sent[joined]+22,(uint8_t[16]){0},16));
    now_ms=dad_time+1000;net_ipv6_poll();assert(net_ipv6_route(&peer,2,NULL,&path)==0);
    assert(last_icmp(133)<0);uint64_t ready=now_ms;
    while(last_icmp(133)<0 && now_ms-ready<1000){now_ms+=10;net_ipv6_poll();}
    assert(last_icmp(133)>=0 && now_ms-ready>0 && now_ms-ready<=1000);
    local=path.source;before=sent_count;assert(net_ipv6_multicast_change(&group,2,1)==0);net_ipv6_poll();
    assert(mld_record(before,&group,4)>=0);before=sent_count;start=now_ms;
    net_ipv6_poll();assert(mld_record(before,&group,4)<0);
    while(mld_record(before,&group,4)<0 && now_ms-start<1000){now_ms+=10;net_ipv6_poll();}
    assert(mld_record(before,&group,4)>=0 && now_ms-start>0 && now_ms-start<=1000);
    uint8_t query[36]={58,0,5,2,0,0,1,0,130};net_put_u16(query+12,1000);
    memcpy(query+16,group.s6_addr,16);net_put_u16(query+10,net_ipv6_checksum(&peer,&group,58,query+8,28));
    len=packet(frame,&peer,&group,0,1,query,sizeof(query));net_ipv6_input(frame,len);
    before=sent_count;start=now_ms;net_ipv6_poll();assert(mld_record(before,&group,2)<0);
    while(mld_record(before,&group,2)<0 && now_ms-start<1000){now_ms+=10;net_ipv6_poll();}
    assert(mld_record(before,&group,2)>=0 && now_ms-start>0 && now_ms-start<=1000);
    /* A shorter query must advance a pending response, including an MLDv2 floating code. */
    net_put_u16(query+12,0x8000);query[10]=query[11]=0;
    net_put_u16(query+10,net_ipv6_checksum(&peer,&group,58,query+8,28));
    len=packet(frame,&peer,&group,0,1,query,sizeof(query));net_ipv6_input(frame,len);
    before=sent_count;now_ms+=1001;net_ipv6_poll();assert(mld_record(before,&group,2)<0);
    net_put_u16(query+12,100);query[10]=query[11]=0;
    net_put_u16(query+10,net_ipv6_checksum(&peer,&group,58,query+8,28));
    len=packet(frame,&peer,&group,0,1,query,sizeof(query));net_ipv6_input(frame,len);start=now_ms;
    while(mld_record(before,&group,2)<0 && now_ms-start<100){now_ms+=10;net_ipv6_poll();}
    assert(mld_record(before,&group,2)>=0 && now_ms-start<=100);

    /* RA timing fields affect NUD; zero advertisements preserve them. */
    uint8_t timing_ra[16]={134,0,0,0};net_put_u32(timing_ra+8,4000);net_put_u32(timing_ra+12,2500);
    net_put_u16(timing_ra+2,net_ipv6_checksum(&peer,&all,58,timing_ra,16));
    len=packet(frame,&peer,&all,58,255,timing_ra,16);net_ipv6_input(frame,len);
    memset(timing_ra+8,0,8);timing_ra[2]=timing_ra[3]=0;
    net_put_u16(timing_ra+2,net_ipv6_checksum(&peer,&all,58,timing_ra,16));
    len=packet(frame,&peer,&all,58,255,timing_ra,16);net_ipv6_input(frame,len);
    assert(net_ipv6_route(&peer2,2,NULL,&path)==0);udp(msg,&local,&peer2,9000,hello,5);
    assert(net_ipv6_output(&path,17,msg,13,64,0,false,false)==13);before=sent_count;
    now_ms+=2499;net_ipv6_poll();assert(last_icmp(135)<(int)before);
    now_ms++;net_ipv6_poll();assert(last_icmp(135)>=(int)before);
    memcpy(na+8,peer2.s6_addr,16);na[2]=na[3]=0;net_put_u16(na+2,net_ipv6_checksum(&peer2,&local,58,na,32));
    len=packet(frame,&peer2,&local,58,255,na,32);net_ipv6_input(frame,len);assert(neighbor_state(&peer2)==2);
    start=now_ms;now_ms+=1999;net_ipv6_poll();assert(neighbor_state(&peer2)==2);
    while(neighbor_state(&peer2)==2 && now_ms-start<6000){now_ms+=1;net_ipv6_poll();}
    assert(neighbor_state(&peer2)==4 && now_ms-start>=2000 && now_ms-start<=6000);

    /* Link reset restores default retransmission and hardware-RNG failure still progresses. */
    net_ipv6_detached();random_word=0;nic_ready=true;net_ipv6_link_changed(true);net_ipv6_poll();
    now_ms+=1000;net_ipv6_poll();assert(net_ipv6_route(&peer2,2,NULL,&path)==0);
    net_put_u32(timing_ra+8,4000);net_put_u32(timing_ra+12,2500);timing_ra[2]=timing_ra[3]=0;
    net_put_u16(timing_ra+2,net_ipv6_checksum(&peer,&all,58,timing_ra,16));
    len=packet(frame,&peer,&all,58,64,timing_ra,16);net_ipv6_input(frame,len);
    udp(msg,&path.source,&peer2,9000,hello,5);assert(net_ipv6_output(&path,17,msg,13,64,0,false,false)==13);
    before=sent_count;now_ms+=999;net_ipv6_poll();assert(last_icmp(135)<(int)before);
    now_ms++;net_ipv6_poll();assert(last_icmp(135)>=(int)before);
    memcpy(na+8,peer2.s6_addr,16);na[2]=na[3]=0;
    net_put_u16(na+2,net_ipv6_checksum(&peer2,&path.source,58,na,32));
    len=packet(frame,&peer2,&path.source,58,255,na,32);net_ipv6_input(frame,len);
    assert(neighbor_state(&peer2)==2);now_ms+=6001;net_ipv6_poll();assert(neighbor_state(&peer2)==2);
    now_ms+=9000;net_ipv6_poll();assert(neighbor_state(&peer2)==4);
    net_ipv6_detached();random_unavailable=true;now_ms+=5000;net_ipv6_link_changed(true);sent_count=0;start=now_ms;
    while(last_icmp(135)<0 && now_ms-start<=1000){net_ipv6_poll();now_ms+=10;}
    assert(last_icmp(135)>=0 && now_ms-start<=1010);random_unavailable=false;
    /* Address configuration must accept a second real Ethernet interface. */
    test_multiple_interfaces();
    test_many_interfaces();
    test_link_allocation_retry();
    puts("PASS IPv6 protocol: loopback, DAD/NDP, checksums, fragments, RA/SLAAC, MLD and timer pump");
#endif
    return 0;
}
