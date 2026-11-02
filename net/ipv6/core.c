/* SPDX-License-Identifier: Apache-2.0 */
/** @file IPv6 host, neighbor discovery, bounded reassembly and MLD. */
#include "../internal.h"
#include <reliefnt/net_ipv6.h>
#include <reliefnt/random.h>
#include <linux/ipv6_route.h>

#define IPV6_ETHERTYPE 0x86ddu
#define IPV6_LINK_MTU 1500u
#define IPV6_MIN_MTU 1280u
#define IPV6_ADDR_MAX 16u
#define IPV6_ROUTE_MAX 24u
#define IPV6_NEIGH_MAX 16u
#define IPV6_GROUP_MAX 32u
#define IPV6_PENDING_MAX 8u
#define IPV6_REASM_MAX 4u
#define IPV6_PMTU_MAX 8u
#define IPV6_HEADER_LIMIT 2048u
#define IPV6_HEADER_COUNT 8u
#define IPV6_REASM_TIMEOUT 60000u
#define IPV6_FOREVER UINT64_MAX
#define IPV6_NUD_INCOMPLETE 1u
#define IPV6_NUD_REACHABLE 2u
#define IPV6_NUD_STALE 4u
#define IPV6_NUD_DELAY 8u
#define IPV6_NUD_PROBE 16u
#define IPV6_NUD_FAILED 32u
#define IPV6_SCOPE_UNIVERSE 0u
#define IPV6_SCOPE_LINK 253u
#define IPV6_SCOPE_HOST 254u

struct ipv6_address_slot {
    struct net_ipv6_address value;
    uint64_t dad_at;
    bool used, autonomous, dad_sent, dad_wait_mld;
};
struct ipv6_route_slot {
    struct net_ipv6_route value;
    uint64_t address_until, ra_until;
    bool used, automatic;
};
struct ipv6_neighbor_slot {
    struct net_ipv6_neighbor value;
    uint64_t deadline;
    uint8_t probes;
    bool used;
};
struct ipv6_group_slot {
    struct in6_addr address;
    uint32_t ifindex, refs;
    uint64_t report_at, query_at;
    uint8_t reports;
    bool used, query_pending;
};
struct ipv6_pending {
    struct net_ipv6_path path;
    uint8_t *data;
    uint32_t length, flowinfo;
    int hops;
    uint8_t protocol;
    bool used, loopback, dont_fragment;
};
struct ipv6_reassembly {
    struct in6_addr source, destination;
    uint8_t *data, *bitmap, *prefix;
    uint32_t identification, covered, total, highwater, prefix_len, prev_next;
    uint64_t deadline;
    uint8_t next;
    bool used, poisoned, have_first, have_last;
};
struct ipv6_pmtu {
    struct in6_addr destination;
    uint64_t deadline;
    uint32_t ifindex, mtu;
    bool used;
};

static struct ipv6_address_slot ipv6_addrs[IPV6_ADDR_MAX];
static struct ipv6_route_slot ipv6_routes[IPV6_ROUTE_MAX];
static struct ipv6_neighbor_slot ipv6_neighbors[IPV6_NEIGH_MAX];
static struct ipv6_group_slot ipv6_groups[IPV6_GROUP_MAX];
static struct ipv6_pending ipv6_pending[IPV6_PENDING_MAX];
static struct ipv6_reassembly ipv6_reassembly[IPV6_REASM_MAX];
static struct ipv6_pmtu ipv6_pmtu[IPV6_PMTU_MAX];
static bool ipv6_initialized, ipv6_link_up;
static uint32_t ipv6_link_mtu = IPV6_LINK_MTU, ipv6_frag_id, ipv6_rng_state;
static uint32_t ipv6_base_reachable_ms = 30000, ipv6_retrans_ms = 1000;
static uint64_t ipv6_reachable_ms = 30000, ipv6_reachable_refresh;
static uint64_t ipv6_rs_at, ipv6_error_at, ipv6_mld_v1_until;
static uint8_t ipv6_rs_sent, ipv6_error_count;
static const struct in6_addr ipv6_zero = {0};
static const struct in6_addr ipv6_loop = {.s6_addr={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}};
static const struct in6_addr ipv6_all_nodes = {.s6_addr={0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0,1}};
static const struct in6_addr ipv6_all_routers = {.s6_addr={0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0,2}};
static const struct in6_addr ipv6_mld_routers = {.s6_addr={0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0x16}};

/** @brief Obtain a discovery jitter word without requiring a hardware generator.
 * @return Hardware random value when available, otherwise a non-cryptographic interface sequence. */
static uint32_t ipv6_jitter_word(void)
{
    uint32_t value;
    if(kernel_random_fill(&value,sizeof(value))==0)return value;
    if(!ipv6_rng_state){const uint8_t *mac=e1000_mac();uint64_t now=time_uptime_ms();
        ipv6_rng_state=(uint32_t)now^(uint32_t)(now>>32)^0x9e3779b9u;
        for(uint32_t i=0;i<6;i++)ipv6_rng_state=(ipv6_rng_state^mac[i])*16777619u;
        if(!ipv6_rng_state)ipv6_rng_state=0x6d2b79f5u;}
    value=ipv6_rng_state;value^=value<<13;value^=value>>17;value^=value<<5;
    ipv6_rng_state=value;return value;
}
/** @brief Choose a bounded, uniformly scaled discovery delay.
 * @param maximum Largest accepted delay, at most UINT32_MAX. @return Milliseconds in the inclusive range. */
static uint32_t ipv6_jitter(uint32_t maximum)
{ return (uint32_t)(((uint64_t)ipv6_jitter_word()*((uint64_t)maximum+1u))>>32); }
/** @brief Recompute randomized neighbor reachability using the current advertised base time. */
static void ipv6_refresh_reachable(void)
{
    ipv6_reachable_ms=(uint64_t)ipv6_base_reachable_ms/2u+ipv6_jitter(ipv6_base_reachable_ms);
    ipv6_reachable_refresh=time_uptime_ms()+7200000u;
}
/** @brief Restore interface discovery timing defaults and choose a fresh reachable interval. */
static void ipv6_reset_timing(void)
{ ipv6_base_reachable_ms=30000;ipv6_retrans_ms=1000;ipv6_refresh_reachable(); }
/** @brief Compare IPv6 addresses. @param a First. @param b Second. @return True if identical. */
static bool ipv6_equal(const struct in6_addr *a,const struct in6_addr *b)
{ return net_memeq(a->s6_addr,b->s6_addr,16)!=0; }
/** @brief Test an unspecified address. @param a Address. @return True for all zeroes. */
static bool ipv6_unspecified(const struct in6_addr *a) { return ipv6_equal(a,&ipv6_zero); }
/** @brief Test an IPv6 multicast address. @param a Address. @return True for ff00::/8. */
static bool ipv6_multicast(const struct in6_addr *a) { return a->s6_addr[0]==0xff; }
/** @brief Test a link-local unicast address. @param a Address. @return True for fe80::/10. */
static bool ipv6_linklocal(const struct in6_addr *a)
{ return a->s6_addr[0]==0xfe && (a->s6_addr[1]&0xc0)==0x80; }
/** @brief Determine the appropriate Linux route address scope. @param a Address. @return Scope. */
static uint32_t ipv6_scope(const struct in6_addr *a)
{ return ipv6_equal(a,&ipv6_loop)?IPV6_SCOPE_HOST:(ipv6_linklocal(a)?IPV6_SCOPE_LINK:IPV6_SCOPE_UNIVERSE); }
/** @brief Compare a network prefix. @param a First. @param b Second. @param bits Prefix width.
 * @return True when all prefix bits agree. */
static bool ipv6_prefix_equal(const struct in6_addr *a,const struct in6_addr *b,uint32_t bits)
{
    uint32_t n=bits/8u, rest=bits%8u;
    if(n && !net_memeq(a->s6_addr,b->s6_addr,n))return false;
    return !rest || !((a->s6_addr[n]^b->s6_addr[n]) & (uint8_t)(0xffu<<(8u-rest)));
}
/** @brief Normalize host bits in a prefix. @param a Address updated in place. @param bits Prefix width. */
static void ipv6_mask(struct in6_addr *a,uint32_t bits)
{
    uint32_t n=bits/8u,rest=bits%8u;
    if(rest){a->s6_addr[n]&=(uint8_t)(0xffu<<(8u-rest));++n;}
    while(n<16)a->s6_addr[n++]=0;
}
/** @brief Compute matching leading address bits. @param a First. @param b Second. @return Matching bits. */
static uint32_t ipv6_match_bits(const struct in6_addr *a,const struct in6_addr *b)
{
    uint32_t n=0;
    for(uint32_t i=0;i<16;i++)for(uint8_t bit=0x80;bit;bit>>=1){
        if((a->s6_addr[i]&bit)!=(b->s6_addr[i]&bit))return n;
        ++n;
    }
    return n;
}
/** @brief Convert a wire lifetime to a monotonic deadline. @param seconds Lifetime.
 * @return Absolute deadline, preserving the infinite sentinel. */
static uint64_t ipv6_deadline(uint32_t seconds)
{
    if(seconds==UINT32_MAX)return IPV6_FOREVER;
    uint64_t now=time_uptime_ms(),delta=(uint64_t)seconds*1000u;
    return delta>=IPV6_FOREVER-now?IPV6_FOREVER:now+delta;
}
/** @brief Check an absolute monotonic deadline. @param deadline Absolute deadline.
 * @param now Uptime. @return True when the finite deadline elapsed. */
static bool ipv6_expired(uint64_t deadline,uint64_t now)
{ return deadline!=IPV6_FOREVER && now>=deadline; }
/** @brief Form a solicited-node multicast address. @param out Output. @param a Target unicast. */
static void ipv6_solicited(struct in6_addr *out,const struct in6_addr *a)
{
    *out=ipv6_zero;out->s6_addr[0]=0xff;out->s6_addr[1]=2;out->s6_addr[11]=1;out->s6_addr[12]=0xff;
    net_memcpy(out->s6_addr+13,a->s6_addr+13,3);
}
/** @brief Find a configured address. @param a Address. @param ifindex Interface or zero.
 * @return Matching private entry or NULL. */
static struct ipv6_address_slot *ipv6_find_address(const struct in6_addr *a,uint32_t ifindex)
{
    for(uint32_t i=0;i<IPV6_ADDR_MAX;i++)if(ipv6_addrs[i].used &&
        (!ifindex || ipv6_addrs[i].value.ifindex==ifindex) && ipv6_equal(&ipv6_addrs[i].value.address,a))return &ipv6_addrs[i];
    return 0;
}
/** @brief Find a multicast membership. @param a Group. @param ifindex Interface.
 * @return Matching private entry or NULL. */
static struct ipv6_group_slot *ipv6_find_group(const struct in6_addr *a,uint32_t ifindex)
{
    for(uint32_t i=0;i<IPV6_GROUP_MAX;i++)if(ipv6_groups[i].used && ipv6_groups[i].ifindex==ifindex &&
        ipv6_equal(&ipv6_groups[i].address,a))return &ipv6_groups[i];
    return 0;
}
/** @brief Check whether a destination is local on the receiving interface.
 * @param a Destination. @param ifindex Interface. @return True if accepted. */
static bool ipv6_destination_local(const struct in6_addr *a,uint32_t ifindex)
{
    if(ipv6_multicast(a)){struct ipv6_group_slot *g=ipv6_find_group(a,ifindex);
        return ipv6_equal(a,&ipv6_all_nodes) || (g && g->refs);}
    struct ipv6_address_slot *slot=ipv6_find_address(a,ifindex);
    return slot && !(slot->value.flags & (NET_IPV6_ADDR_TENTATIVE|NET_IPV6_ADDR_DADFAILED));
}
/** @brief Compute a transport pseudo-header checksum. @param source Source address.
 * @param destination Destination address. @param protocol Upper-layer protocol.
 * @param data Borrowed message bytes. @param length Message length. @return Folded checksum. */
uint16_t net_ipv6_checksum(const struct in6_addr *source,const struct in6_addr *destination,
                           uint8_t protocol,const void *data,uint32_t length)
{
    uint8_t trailer[8]={0};net_put_u32(trailer,length);trailer[7]=protocol;
    uint32_t sum=net_checksum_partial(0,source->s6_addr,16);
    sum=net_checksum_partial(sum,destination->s6_addr,16);
    sum=net_checksum_partial(sum,trailer,8);
    return net_checksum_finish(net_checksum_partial(sum,data,length));
}
/** @brief Copy live address snapshots. @param out Optional output. @param capacity Entries available.
 * @return Total entry count. */
uint32_t net_ipv6_addresses(struct net_ipv6_address *out,uint32_t capacity)
{
    uint32_t n=0;for(uint32_t i=0;i<IPV6_ADDR_MAX;i++)if(ipv6_addrs[i].used){
        if(out && n<capacity)out[n]=ipv6_addrs[i].value;
        ++n;}
    return n;
}
/** @brief Copy live route snapshots. @param out Optional output. @param capacity Entries available.
 * @return Total entry count. */
uint32_t net_ipv6_routes(struct net_ipv6_route *out,uint32_t capacity)
{
    uint32_t n=0;for(uint32_t i=0;i<IPV6_ROUTE_MAX;i++)if(ipv6_routes[i].used){
        if(out && n<capacity)out[n]=ipv6_routes[i].value;
        ++n;}
    return n;
}
/** @brief Copy neighbor snapshots. @param out Optional output. @param capacity Entries available.
 * @return Total entry count. */
uint32_t net_ipv6_neighbors(struct net_ipv6_neighbor *out,uint32_t capacity)
{
    uint32_t n=0;for(uint32_t i=0;i<IPV6_NEIGH_MAX;i++)if(ipv6_neighbors[i].used){
        if(out && n<capacity)out[n]=ipv6_neighbors[i].value;
        ++n;}
    return n;
}
/** @brief Retain or release one host multicast membership. @param address Multicast group.
 * @param ifindex Interface. @param delta One to join or minus one to leave. @return Zero or errno. */
int net_ipv6_multicast_change(const struct in6_addr *address,uint32_t ifindex,int delta)
{
    if(!address || !ipv6_multicast(address) || (ifindex!=1 && ifindex!=2) || (delta!=1 && delta!=-1))return -LINUX_EINVAL;
    struct ipv6_group_slot *g=ipv6_find_group(address,ifindex);
    if(delta<0){if(!g)return -LINUX_EADDRNOTAVAIL;if(--g->refs==0){
        /* Keep the group briefly to send an MLD state-change record. */
        g->reports=2;g->query_pending=false;g->report_at=time_uptime_ms();}return 0;}
    if(g){if(g->refs==UINT32_MAX)return -LINUX_ENOBUFS;if(!g->refs){g->reports=2;g->query_pending=false;g->report_at=time_uptime_ms();}++g->refs;return 0;}
    for(uint32_t i=0;i<IPV6_GROUP_MAX;i++)if(!ipv6_groups[i].used){
        g=&ipv6_groups[i];g->used=true;g->address=*address;g->ifindex=ifindex;g->refs=1;
        g->reports=2;g->query_pending=false;g->report_at=time_uptime_ms();return 0;}
    return -LINUX_ENOBUFS;
}
/** @brief Add, replace or remove a route. @param route Checked route in network byte order.
 * @param remove Remove rather than add. @return Zero or negative errno. */
int net_ipv6_route_update(const struct net_ipv6_route *route,bool remove)
{
    if(!route || route->prefix_length>128 || (route->ifindex!=1 && route->ifindex!=2) ||
       ipv6_multicast(&route->gateway))return -LINUX_EINVAL;
    struct net_ipv6_route normalized=*route;ipv6_mask(&normalized.destination,normalized.prefix_length);
    struct ipv6_route_slot *free_slot=0;
    for(uint32_t i=0;i<IPV6_ROUTE_MAX;i++){
        struct ipv6_route_slot *s=&ipv6_routes[i];if(!s->used){if(!free_slot)free_slot=s;continue;}
        if(s->value.ifindex==normalized.ifindex && s->value.prefix_length==normalized.prefix_length &&
           ipv6_equal(&s->value.destination,&normalized.destination) && ipv6_equal(&s->value.gateway,&normalized.gateway)){
            if(remove)s->used=false;else{s->value=normalized;s->automatic=false;s->address_until=s->ra_until=0;}
            netlink_ipv6_changed();return 0;}
    }
    if(remove)return -LINUX_ESRCH;
    if(!free_slot)return -LINUX_ENOBUFS;
    free_slot->value=normalized;free_slot->used=true;free_slot->automatic=false;netlink_ipv6_changed();return 0;
}
/** @brief Recompute an automatic connected route from manually assigned addresses.
 * @param address An address in the prefix. @param bits Prefix width.
 * @return Zero or negative capacity errno. */
static int ipv6_connected_route(const struct in6_addr *address,uint32_t bits)
{
    struct in6_addr prefix=*address;ipv6_mask(&prefix,bits);uint64_t deadline=0,now=time_uptime_ms();
    for(uint32_t i=0;i<IPV6_ADDR_MAX;i++){
        struct ipv6_address_slot *a=&ipv6_addrs[i];
        if(a->used && !a->autonomous && a->value.ifindex==2 && a->value.prefix_length==bits &&
           ipv6_prefix_equal(&a->value.address,&prefix,bits) && !ipv6_expired(a->value.valid_until,now) &&
           a->value.valid_until>deadline)deadline=a->value.valid_until;
    }
    struct ipv6_route_slot *slot=0,*empty=0;
    for(uint32_t i=0;i<IPV6_ROUTE_MAX;i++){
        struct ipv6_route_slot *r=&ipv6_routes[i];
        if(!r->used){if(!empty)empty=r;continue;}
        if(r->value.ifindex==2 && r->value.prefix_length==bits && ipv6_unspecified(&r->value.gateway) &&
           ipv6_equal(&r->value.destination,&prefix)){slot=r;break;}
    }
    if(slot && !slot->automatic)return 0;
    if(!slot){if(!deadline)return 0;if(!empty)return -LINUX_ENOBUFS;slot=empty;net_memzero(slot,sizeof(*slot));
        slot->used=true;slot->automatic=true;slot->value.destination=prefix;slot->value.ifindex=2;
        slot->value.prefix_length=bits;slot->value.metric=256;
        slot->value.flags=RTF_UP | (bits==128?RTF_HOST:0);}
    slot->address_until=deadline;
    if(slot->ra_until && ipv6_expired(slot->ra_until,now))slot->ra_until=0;
    slot->value.valid_until=slot->address_until>slot->ra_until?slot->address_until:slot->ra_until;
    if(!slot->value.valid_until)slot->used=false;
    netlink_ipv6_changed();return 0;
}
/** @brief Apply a manual or SLAAC address and associated membership.
 * @param address Configuration and absolute lifetimes. @param remove Remove when true.
 * @param autonomous True for router-derived addresses. @return Zero or negative errno. */
static int ipv6_address_set(const struct net_ipv6_address *address,bool remove,bool autonomous)
{
    if(!address || address->prefix_length>128 || (address->ifindex!=1 && address->ifindex!=2) ||
       ipv6_multicast(&address->address) || ipv6_unspecified(&address->address) ||
       address->preferred_until>address->valid_until)return -LINUX_EINVAL;
    struct ipv6_address_slot *slot=ipv6_find_address(&address->address,address->ifindex);
    if(remove){if(!slot)return -LINUX_EADDRNOTAVAIL;
        if(slot->value.ifindex==1 && ipv6_equal(&slot->value.address,&ipv6_loop))return -LINUX_EPERM;
        struct in6_addr group;ipv6_solicited(&group,&slot->value.address);
        if(slot->value.ifindex==2)(void)net_ipv6_multicast_change(&group,slot->value.ifindex,-1);
        bool connected=slot->value.ifindex==2 && !slot->autonomous;
        struct net_ipv6_address old=slot->value;slot->used=false;
        if(connected)(void)ipv6_connected_route(&old.address,old.prefix_length);
        netlink_ipv6_changed();return 0;}
    if(slot){struct net_ipv6_address old=slot->value;bool old_auto=slot->autonomous;
        uint32_t state=slot->value.flags & (NET_IPV6_ADDR_TENTATIVE|NET_IPV6_ADDR_DADFAILED);
        slot->value=*address;slot->value.flags|=state;slot->value.scope=ipv6_scope(&address->address);
        slot->autonomous=autonomous;
        if(old.ifindex==2 && !old_auto)(void)ipv6_connected_route(&old.address,old.prefix_length);
        if(address->ifindex==2 && !autonomous){int rc=ipv6_connected_route(&address->address,address->prefix_length);
            if(rc<0){slot->value=old;slot->autonomous=old_auto;
                if(old.ifindex==2 && !old_auto)(void)ipv6_connected_route(&old.address,old.prefix_length);
                return rc;}}
        netlink_ipv6_changed();return 0;}
    for(uint32_t i=0;i<IPV6_ADDR_MAX;i++)if(!ipv6_addrs[i].used){slot=&ipv6_addrs[i];break;}
    if(!slot)return -LINUX_ENOBUFS;
    struct in6_addr group;ipv6_solicited(&group,&address->address);
    if(address->ifindex==2){int ret=net_ipv6_multicast_change(&group,address->ifindex,1);if(ret<0)return ret;}
    slot->used=true;slot->value=*address;slot->value.scope=ipv6_scope(&address->address);
    slot->autonomous=autonomous;slot->dad_sent=false;slot->dad_wait_mld=false;slot->dad_at=time_uptime_ms();
    if(address->ifindex==2 && !(address->flags&NET_IPV6_ADDR_NODAD)){
        slot->value.flags|=NET_IPV6_ADDR_TENTATIVE;slot->dad_at+=ipv6_jitter(1000);
        struct ipv6_group_slot *g=ipv6_find_group(&group,address->ifindex);
        if(g && g->refs==1 && g->reports==2){g->report_at=slot->dad_at;slot->dad_wait_mld=true;}
    }
    if(address->ifindex==2 && !autonomous){int ret=ipv6_connected_route(&address->address,address->prefix_length);
        if(ret<0){slot->used=false;(void)net_ipv6_multicast_change(&group,address->ifindex,-1);return ret;}}
    netlink_ipv6_changed();return 0;
}
/** @brief Add or remove a local address and its solicited-node membership.
 * @param address Checked address, interface and absolute lifetimes. @param remove Remove when true.
 * @return Zero or negative errno; caller has already authorized configuration. */
int net_ipv6_address_update(const struct net_ipv6_address *address,bool remove)
{ return ipv6_address_set(address,remove,false); }

/** @brief Select a live source and route for an IPv6 destination. @param destination Destination.
 * @param scope Interface index or zero. @param bound Optional bound source. @param out_path Snapshot.
 * @return Zero or negative errno. */
int net_ipv6_route(const struct in6_addr *destination,uint32_t scope,
                   const struct in6_addr *bound,struct net_ipv6_path *out_path)
{
    if(!destination || !out_path || ipv6_unspecified(destination) || (scope && scope!=1 && scope!=2))return -LINUX_EINVAL;
    uint64_t now=time_uptime_ms();uint32_t ifindex=0,mtu=IPV6_LINK_MTU;
    struct in6_addr next=*destination;
    struct ipv6_address_slot *local=ipv6_find_address(destination,0);
    if(ipv6_equal(destination,&ipv6_loop)){ifindex=1;mtu=65575;}
    else if(local && !(local->value.flags&(NET_IPV6_ADDR_TENTATIVE|NET_IPV6_ADDR_DADFAILED))){
        ifindex=local->value.ifindex;mtu=65575;
    } else if(ipv6_multicast(destination)){
        uint8_t ms=destination->s6_addr[1]&15;
        if(ms==0)return -LINUX_EINVAL;
        ifindex=scope?scope:(ms==1?1:2);mtu=ifindex==1?65575:ipv6_link_mtu;
        if(ms==1 && ifindex!=1)return -LINUX_ENETUNREACH;
    } else {
        struct ipv6_route_slot *best=0;
        for(uint32_t i=0;i<IPV6_ROUTE_MAX;i++){
            struct ipv6_route_slot *r=&ipv6_routes[i];
            if(!r->used || ipv6_expired(r->value.valid_until,now) ||
               (scope && r->value.ifindex!=scope) || !ipv6_prefix_equal(destination,&r->value.destination,r->value.prefix_length))continue;
            if(!best || r->value.prefix_length>best->value.prefix_length ||
               (r->value.prefix_length==best->value.prefix_length && r->value.metric<best->value.metric))best=r;
        }
        if(!best)return -LINUX_ENETUNREACH;
        ifindex=best->value.ifindex;mtu=ifindex==1?65575:ipv6_link_mtu;
        if(!ipv6_unspecified(&best->value.gateway))next=best->value.gateway;
    }
    if(scope && scope!=ifindex)return -LINUX_ENETUNREACH;
    if(ifindex==2 && (!ipv6_link_up || !e1000_is_ready()))return -LINUX_ENETDOWN;
    if(ifindex==2 && ipv6_equal(destination,&ipv6_loop))return -LINUX_ENETUNREACH;
    struct ipv6_address_slot *source=0;uint32_t best_score=0;
    for(uint32_t i=0;i<IPV6_ADDR_MAX;i++){
        struct ipv6_address_slot *a=&ipv6_addrs[i];
        if(!a->used || a->value.ifindex!=ifindex ||
           a->value.flags&(NET_IPV6_ADDR_TENTATIVE|NET_IPV6_ADDR_DADFAILED) || ipv6_expired(a->value.valid_until,now))continue;
        if(bound && !ipv6_unspecified(bound) && !ipv6_equal(bound,&a->value.address))continue;
        bool link_destination=ipv6_linklocal(destination) ||
            (ipv6_multicast(destination) && (destination->s6_addr[1]&15)<=2);
        if(ifindex==2 && !link_destination && ipv6_linklocal(&a->value.address))continue;
        uint32_t score=ipv6_match_bits(destination,&a->value.address)+1;
        if(ifindex==2 && link_destination && ipv6_linklocal(&a->value.address))score+=512;
        if(!(a->value.flags&NET_IPV6_ADDR_DEPRECATED) && !ipv6_expired(a->value.preferred_until,now))score+=256;
        if(ipv6_equal(destination,&a->value.address))score+=512;
        if(!source || score>best_score){source=a;best_score=score;}
    }
    if(!source)return -LINUX_EADDRNOTAVAIL;
    for(uint32_t i=0;i<IPV6_PMTU_MAX;i++)if(ipv6_pmtu[i].used && ipv6_pmtu[i].ifindex==ifindex &&
        !ipv6_expired(ipv6_pmtu[i].deadline,now) && ipv6_equal(&ipv6_pmtu[i].destination,destination) && ipv6_pmtu[i].mtu<mtu)
        mtu=ipv6_pmtu[i].mtu;
    out_path->source=source->value.address;out_path->destination=*destination;out_path->next_hop=next;
    out_path->ifindex=ifindex;out_path->mtu=mtu;return 0;
}
/** @brief Transmit a complete IPv6 packet using a known Ethernet address. @param mac Destination MAC.
 * @param source IPv6 source. @param destination IPv6 destination. @param next Next header.
 * @param hops Hop limit. @param flowinfo Network-order flow bits. @param payload Bytes.
 * @param length Payload length no greater than Ethernet MTU minus IPv6 header. @return Driver status. */
static int ipv6_send_frame(const uint8_t mac[6],const struct in6_addr *source,
                           const struct in6_addr *destination,uint8_t next,uint8_t hops,
                           uint32_t flowinfo,const uint8_t *payload,uint32_t length)
{
    if(!ipv6_link_up || !e1000_is_ready())return -LINUX_ENETDOWN;
    if(length>IPV6_LINK_MTU-40)return -LINUX_EMSGSIZE;
    uint8_t frame[14+IPV6_LINK_MTU];uint8_t *ip=frame+14;const uint8_t *flow=(const uint8_t *)&flowinfo;
    net_write_eth(frame,mac,IPV6_ETHERTYPE);ip[0]=(uint8_t)(0x60|(flow[0]&15));
    ip[1]=flow[1];ip[2]=flow[2];ip[3]=flow[3];net_put_u16(ip+4,(uint16_t)length);
    ip[6]=next;ip[7]=hops;net_memcpy(ip+8,source->s6_addr,16);net_memcpy(ip+24,destination->s6_addr,16);
    if(length)net_memcpy(ip+40,payload,length);
    return e1000_send(frame,14+40+length);
}
/** @brief Derive an Ethernet destination from an IPv6 multicast group. @param mac Six-byte output.
 * @param address Multicast destination. */
static void ipv6_multicast_mac(uint8_t mac[6],const struct in6_addr *address)
{ mac[0]=0x33;mac[1]=0x33;net_memcpy(mac+2,address->s6_addr+12,4); }
/** @brief Find one neighbor. @param address Neighbor address. @return Entry or NULL. */
static struct ipv6_neighbor_slot *ipv6_find_neighbor(const struct in6_addr *address)
{
    for(uint32_t i=0;i<IPV6_NEIGH_MAX;i++)if(ipv6_neighbors[i].used &&
        ipv6_equal(&ipv6_neighbors[i].value.address,address))return &ipv6_neighbors[i];
    return 0;
}
/** @brief Create a bounded neighbor entry. @param address Neighbor address.
 * @return Entry or NULL when every slot is active. */
static struct ipv6_neighbor_slot *ipv6_create_neighbor(const struct in6_addr *address)
{
    struct ipv6_neighbor_slot *slot=ipv6_find_neighbor(address);
    if(slot && slot->value.state!=IPV6_NUD_FAILED)return slot;
    if(slot){net_memzero(slot->value.mac,6);slot->value.state=IPV6_NUD_INCOMPLETE;
        slot->deadline=time_uptime_ms();slot->probes=0;netlink_ipv6_changed();return slot;}
    for(uint32_t i=0;i<IPV6_NEIGH_MAX;i++)if(!ipv6_neighbors[i].used ||
        ipv6_neighbors[i].value.state==IPV6_NUD_FAILED){slot=&ipv6_neighbors[i];break;}
    if(!slot)return 0;
    net_memzero(slot,sizeof(*slot));slot->used=true;slot->value.address=*address;
    slot->value.ifindex=2;slot->value.state=IPV6_NUD_INCOMPLETE;slot->deadline=time_uptime_ms();
    netlink_ipv6_changed();return slot;
}
/** @brief Send a Neighbor Solicitation for DAD or address resolution. @param target Target.
 * @param source Usable source or unspecified for DAD. @param unicast Optional neighbor MAC.
 * @return Driver status. */
static int ipv6_send_ns(const struct in6_addr *target,const struct in6_addr *source,const uint8_t *unicast)
{
    struct in6_addr dest;uint8_t mac[6],ns[32]={0};uint32_t len=24;
    if(unicast){dest=*target;net_memcpy(mac,unicast,6);}else{ipv6_solicited(&dest,target);ipv6_multicast_mac(mac,&dest);}
    ns[0]=135;net_memcpy(ns+8,target->s6_addr,16);
    if(!ipv6_unspecified(source)){ns[24]=1;ns[25]=1;net_memcpy(ns+26,e1000_mac(),6);len=32;}
    net_put_u16(ns+2,net_ipv6_checksum(source,&dest,58,ns,len));
    return ipv6_send_frame(mac,source,&dest,58,255,0,ns,len);
}
/** @brief Send a Router Solicitation after link-local DAD. @param source Link-local source.
 * @return Driver status. */
static int ipv6_send_rs(const struct in6_addr *source)
{
    uint8_t mac[6],rs[16]={133,0,0,0,0,0,0,0,1,1};ipv6_multicast_mac(mac,&ipv6_all_routers);
    net_memcpy(rs+10,e1000_mac(),6);net_put_u16(rs+2,net_ipv6_checksum(source,&ipv6_all_routers,58,rs,16));
    return ipv6_send_frame(mac,source,&ipv6_all_routers,58,255,0,rs,16);
}
/** @brief Transmit one datagram, fragmenting at the source when necessary. @param path Route snapshot.
 * @param protocol Upper next-header. @param data Borrowed bytes. @param length Length.
 * @param hops Hop limit. @param flowinfo Network-order flow bits. @param dont_fragment Forbid fragments.
 * @param mac Resolved Ethernet destination. @return Accepted bytes or negative errno. */
static int ipv6_transmit(const struct net_ipv6_path *path,uint8_t protocol,const uint8_t *data,
                         uint32_t length,int hops,uint32_t flowinfo,bool dont_fragment,const uint8_t mac[6])
{
    uint32_t mtu=path->mtu;if(mtu>ipv6_link_mtu)mtu=ipv6_link_mtu;
    if(mtu<IPV6_MIN_MTU)mtu=IPV6_MIN_MTU;
    uint8_t hop=(uint8_t)(hops<0?64:hops);
    if(length+40<=mtu){int rc=ipv6_send_frame(mac,&path->source,&path->destination,protocol,hop,flowinfo,data,length);
        return rc<0?rc:(int)length;}
    if(dont_fragment)return -LINUX_EMSGSIZE;
    uint32_t chunk=(mtu-48)&~7u,ident=++ipv6_frag_id,offset=0;uint8_t fragment[IPV6_LINK_MTU-40];
    while(offset<length){uint32_t n=length-offset;if(n>chunk)n=chunk;
        fragment[0]=protocol;fragment[1]=0;net_put_u16(fragment+2,(uint16_t)(offset|(offset+n<length?1u:0u)));
        net_put_u32(fragment+4,ident);net_memcpy(fragment+8,data+offset,n);
        int rc=ipv6_send_frame(mac,&path->source,&path->destination,44,hop,flowinfo,fragment,n+8);
        if(rc<0)return rc;
        offset+=n;}
    return (int)length;
}
/** @brief Own a payload until a neighbor resolves or loopback is polled. @param path Route snapshot.
 * @param protocol Upper protocol. @param data Borrowed bytes. @param length Length.
 * @param hops Hop limit. @param flowinfo Network-order flow bits. @param dont_fragment Forbid fragments.
 * @param loopback Deferred local delivery. @return Accepted bytes or negative errno. */
static int ipv6_queue(const struct net_ipv6_path *path,uint8_t protocol,const void *data,uint32_t length,
                      int hops,uint32_t flowinfo,bool dont_fragment,bool loopback)
{
    struct ipv6_pending *slot=0;
    for(uint32_t i=0;i<IPV6_PENDING_MAX;i++)if(!ipv6_pending[i].used){slot=&ipv6_pending[i];break;}
    if(!slot)return -LINUX_ENOBUFS;
    uint8_t *copy=kernel_malloc(length?length:1);if(!copy)return -LINUX_ENOMEM;
    if(length)net_memcpy(copy,data,length);
    slot->path=*path;slot->protocol=protocol;slot->data=copy;
    slot->length=length;slot->hops=hops;slot->flowinfo=flowinfo;slot->dont_fragment=dont_fragment;
    slot->loopback=loopback;slot->used=true;return (int)length;
}
/** @brief Send or retain an IPv6 transport message. @param path Selected route.
 * @param protocol Next header. @param data Borrowed payload. @param length Payload bytes.
 * @param hops Hop limit or -1. @param flowinfo Network-order flow information.
 * @param dont_fragment Reject packets larger than MTU. @param multicast_loop Deliver to local members.
 * @return Accepted bytes or negative errno. */
int net_ipv6_output(const struct net_ipv6_path *path,uint8_t protocol,const void *data,uint32_t length,
                    int hops,uint32_t flowinfo,bool dont_fragment,bool multicast_loop)
{
    if(length>65535)return -LINUX_EMSGSIZE;
    if(!path || (!data && length) || hops < -1 || hops>255 ||
       (path->ifindex!=1 && path->ifindex!=2))return -LINUX_EINVAL;
    if(path->ifindex==2 && (!ipv6_link_up || !e1000_is_ready()))return -LINUX_ENETDOWN;
    if(dont_fragment && length+40>path->mtu)return -LINUX_EMSGSIZE;
    struct ipv6_address_slot *dst=ipv6_find_address(&path->destination,path->ifindex);
    if(path->ifindex==1 || (dst && !(dst->value.flags&(NET_IPV6_ADDR_TENTATIVE|NET_IPV6_ADDR_DADFAILED))))
        return ipv6_queue(path,protocol,data,length,hops,flowinfo,dont_fragment,true);
    if(ipv6_multicast(&path->destination)){
        struct ipv6_group_slot *group=ipv6_find_group(&path->destination,path->ifindex);
        struct ipv6_pending *queued=0;
        if(multicast_loop && (ipv6_equal(&path->destination,&ipv6_all_nodes) || (group && group->refs))){
            for(uint32_t i=0;i<IPV6_PENDING_MAX;i++)if(!ipv6_pending[i].used){queued=&ipv6_pending[i];break;}
            int rc=ipv6_queue(path,protocol,data,length,hops,flowinfo,dont_fragment,true);
            if(rc<0)return rc;
        }
        uint8_t mac[6];ipv6_multicast_mac(mac,&path->destination);
        int rc=ipv6_transmit(path,protocol,data,length,hops,flowinfo,dont_fragment,mac);
        if(rc<0 && queued){kernel_free(queued->data);queued->used=false;}
        return rc;
    }
    struct ipv6_neighbor_slot *neighbor=ipv6_create_neighbor(&path->next_hop);
    if(!neighbor)return -LINUX_ENOBUFS;
    if(neighbor->value.state!=IPV6_NUD_INCOMPLETE && neighbor->value.state!=IPV6_NUD_FAILED){
        if(neighbor->value.state==IPV6_NUD_STALE){neighbor->value.state=IPV6_NUD_DELAY;neighbor->deadline=time_uptime_ms()+5000;}
        return ipv6_transmit(path,protocol,data,length,hops,flowinfo,dont_fragment,neighbor->value.mac);
    }
    int rc=ipv6_queue(path,protocol,data,length,hops,flowinfo,dont_fragment,false);
    if(rc>=0 && neighbor->probes==0){(void)ipv6_send_ns(&path->next_hop,&path->source,0);
        neighbor->probes=1;neighbor->deadline=time_uptime_ms()+ipv6_retrans_ms;}
    return rc;
}

/** @brief Inform matching transport of an asynchronous neighbor or transmit failure.
 * @param pending Owned pending packet. @param error Positive Linux errno. */
static void ipv6_pending_error(const struct ipv6_pending *pending,int error)
{
    uint8_t quote[128]={0};uint32_t n=pending->length;if(n>sizeof(quote)-40)n=sizeof(quote)-40;
    quote[0]=0x60;net_put_u16(quote+4,(uint16_t)pending->length);quote[6]=pending->protocol;
    net_memcpy(quote+8,pending->path.source.s6_addr,16);net_memcpy(quote+24,pending->path.destination.s6_addr,16);
    net_memcpy(quote+40,pending->data,n);net_ipv6_socket_error(quote,n+40,error,0);
}
/** @brief Release queued packets for a newly resolved neighbor. @param neighbor Resolved entry. */
static void ipv6_flush_neighbor(struct ipv6_neighbor_slot *neighbor)
{
    for(uint32_t i=0;i<IPV6_PENDING_MAX;i++){
        struct ipv6_pending *p=&ipv6_pending[i];if(!p->used || p->loopback ||
            !ipv6_equal(&p->path.next_hop,&neighbor->value.address))continue;
        int rc=ipv6_transmit(&p->path,p->protocol,p->data,p->length,p->hops,p->flowinfo,p->dont_fragment,neighbor->value.mac);
        if(rc<0)ipv6_pending_error(p,-rc);
        kernel_free(p->data);p->used=false;
    }
}
/** @brief Update neighbor state from a validated link-layer option. @param address Neighbor address.
 * @param mac Link address. @param solicited Valid solicited advertisement. @param override Replace changed address. */
static void ipv6_learn_neighbor(const struct in6_addr *address,const uint8_t mac[6],bool solicited,bool override)
{
    if(ipv6_unspecified(address) || ipv6_multicast(address) || (mac[0]&1))return;
    struct ipv6_neighbor_slot *n=ipv6_create_neighbor(address);if(!n)return;
    bool changed=n->value.state!=IPV6_NUD_INCOMPLETE && !net_memeq(n->value.mac,mac,6);
    if(changed && !override){if(n->value.state==IPV6_NUD_REACHABLE)n->value.state=IPV6_NUD_STALE;return;}
    net_memcpy(n->value.mac,mac,6);n->value.state=solicited?IPV6_NUD_REACHABLE:IPV6_NUD_STALE;
    n->deadline=time_uptime_ms()+ipv6_reachable_ms;n->probes=0;netlink_ipv6_changed();ipv6_flush_neighbor(n);
}
/** @brief Find and validate an ND link-layer option. @param options Option bytes. @param length Byte length.
 * @param kind Wanted option type. @param mac Output pointer into input or NULL. @return False for malformed options. */
static bool ipv6_nd_options(const uint8_t *options,uint32_t length,uint8_t kind,const uint8_t **mac)
{
    *mac=0;
    while(length){if(length<2 || !options[1])return false;uint32_t n=(uint32_t)options[1]*8u;
        if(n>length)return false;
        if(options[0]==kind){if(n!=8 || *mac)return false;*mac=options+2;}
        options+=n;length-=n;}
    return true;
}
/** @brief Respond to a valid solicitation for one of our addresses. @param target Local address.
 * @param destination Recipient address. @param mac Ethernet recipient. @param solicited Solicited response. */
static void ipv6_send_na(const struct in6_addr *target,const struct in6_addr *destination,
                         const uint8_t mac[6],bool solicited)
{
    uint8_t na[32]={136,0,0,0,0,0,0,0};na[4]=(uint8_t)(0x20|(solicited?0x40:0));
    net_memcpy(na+8,target->s6_addr,16);na[24]=2;na[25]=1;net_memcpy(na+26,e1000_mac(),6);
    net_put_u16(na+2,net_ipv6_checksum(target,destination,58,na,32));
    (void)ipv6_send_frame(mac,target,destination,58,255,0,na,32);
}
/** @brief Mark an address as a failed duplicate. @param slot Tentative local address. */
static void ipv6_dad_failed(struct ipv6_address_slot *slot)
{
    slot->value.flags&=~NET_IPV6_ADDR_TENTATIVE;slot->value.flags|=NET_IPV6_ADDR_DADFAILED;
    netlink_ipv6_changed();
}
/** @brief Install or refresh an automatically learned prefix route. @param prefix Prefix address.
 * @param bits Prefix width. @param gateway Gateway or unspecified. @param deadline Lifetime.
 * @param metric Selection metric. @param addrconf True for an RA-derived route. */
static void ipv6_auto_route(const struct in6_addr *prefix,uint32_t bits,const struct in6_addr *gateway,
                            uint64_t deadline,uint32_t metric,bool addrconf)
{
    struct in6_addr network=*prefix;ipv6_mask(&network,bits);uint64_t now=time_uptime_ms();
    struct ipv6_route_slot *slot=0,*empty=0;
    for(uint32_t i=0;i<IPV6_ROUTE_MAX;i++){
        struct ipv6_route_slot *r=&ipv6_routes[i];if(!r->used){if(!empty)empty=r;continue;}
        if(r->value.ifindex==2 && r->value.prefix_length==bits && ipv6_equal(&r->value.destination,&network) &&
           ipv6_equal(&r->value.gateway,gateway)){slot=r;break;}
    }
    if(slot && !slot->automatic)return;
    if(!slot){if(ipv6_expired(deadline,now) || !empty)return;slot=empty;net_memzero(slot,sizeof(*slot));
        slot->used=true;slot->automatic=true;slot->value.destination=network;slot->value.gateway=*gateway;
        slot->value.prefix_length=bits;slot->value.ifindex=2;slot->value.metric=metric;
        slot->value.flags=RTF_UP | (bits==128?RTF_HOST:0) | (!ipv6_unspecified(gateway)?RTF_GATEWAY:0);}
    if(addrconf)slot->value.flags|=RTF_ADDRCONF;
    slot->ra_until=ipv6_expired(deadline,now)?0:deadline;
    if(slot->address_until && ipv6_expired(slot->address_until,now))slot->address_until=0;
    slot->value.valid_until=slot->ra_until>slot->address_until?slot->ra_until:slot->address_until;
    if(!slot->value.valid_until)slot->used=false;
    netlink_ipv6_changed();
}
/** @brief Apply a validated autonomous /64 prefix with RFC 4862 lifetime protection.
 * @param prefix Prefix information option. @param valid Valid lifetime seconds.
 * @param preferred Preferred lifetime seconds. */
static void ipv6_slaac(const uint8_t *prefix,uint32_t valid,uint32_t preferred)
{
    struct in6_addr address;net_memcpy(address.s6_addr,prefix,8);
    const uint8_t *mac=e1000_mac();address.s6_addr[8]=mac[0]^2;address.s6_addr[9]=mac[1];address.s6_addr[10]=mac[2];
    address.s6_addr[11]=0xff;address.s6_addr[12]=0xfe;address.s6_addr[13]=mac[3];address.s6_addr[14]=mac[4];address.s6_addr[15]=mac[5];
    struct ipv6_address_slot *slot=ipv6_find_address(&address,2);uint64_t now=time_uptime_ms();
    uint64_t valid_until=ipv6_deadline(valid),preferred_until=ipv6_deadline(preferred);
    if(slot){if(!slot->autonomous)return;
        uint64_t remaining=slot->value.valid_until==IPV6_FOREVER?IPV6_FOREVER:
            (slot->value.valid_until>now?slot->value.valid_until-now:0);
        uint64_t proposed=valid==UINT32_MAX?IPV6_FOREVER:(uint64_t)valid*1000u;
        if(proposed<=7200000u && proposed<=remaining){
            if(remaining<=7200000u)valid_until=slot->value.valid_until;
            else valid_until=now+7200000u;
        }
        slot->value.valid_until=valid_until;slot->value.preferred_until=preferred_until;
        if(preferred==0)slot->value.flags|=NET_IPV6_ADDR_DEPRECATED;
        else slot->value.flags&=~NET_IPV6_ADDR_DEPRECATED;
        netlink_ipv6_changed();return;
    }
    if(!valid)return;
    struct net_ipv6_address entry={0};entry.address=address;entry.ifindex=2;entry.prefix_length=64;
    entry.preferred_until=preferred_until;entry.valid_until=valid_until;
    if(preferred==0)entry.flags|=NET_IPV6_ADDR_DEPRECATED;
    (void)ipv6_address_set(&entry,false,true);
}
/** @brief Process a validated router advertisement and its bounded options.
 * @param source Link-local router. @param icmp Message. @param length Bytes. */
static void ipv6_router_advertisement(const struct in6_addr *source,const uint8_t *icmp,uint32_t length)
{
    if(!ipv6_linklocal(source) || length<16 || icmp[1])return;
    /* Validate the entire option sequence before applying any changes. */
    for(uint32_t off=16;off<length;){if(length-off<2 || !icmp[off+1])return;
        uint32_t n=(uint32_t)icmp[off+1]*8u;if(n>length-off)return;off+=n;}
    uint32_t reachable=net_get_u32(icmp+8),retrans=net_get_u32(icmp+12);
    if(reachable && reachable!=ipv6_base_reachable_ms){ipv6_base_reachable_ms=reachable;ipv6_refresh_reachable();}
    if(retrans)ipv6_retrans_ms=retrans;
    ipv6_rs_sent=3;
    uint16_t life=net_get_u16(icmp+6);ipv6_auto_route(&ipv6_zero,0,source,ipv6_deadline(life),1024,true);
    for(uint32_t off=16;off<length;){uint8_t kind=icmp[off];uint32_t n=(uint32_t)icmp[off+1]*8u;
        const uint8_t *p=icmp+off;
        if(kind==1 && n==8)ipv6_learn_neighbor(source,p+2,false,true);
        else if(kind==5 && n==8){uint32_t mtu=net_get_u32(p+4);if(mtu>=IPV6_MIN_MTU && mtu<=IPV6_LINK_MTU)ipv6_link_mtu=mtu;}
        else if(kind==3 && n==32){uint8_t bits=p[2],flags=p[3];uint32_t valid=net_get_u32(p+4),preferred=net_get_u32(p+8);
            struct in6_addr prefix;net_memcpy(prefix.s6_addr,p+16,16);
            if(bits<=128 && !ipv6_linklocal(&prefix) && !ipv6_multicast(&prefix)){
                if(flags&0x80)ipv6_auto_route(&prefix,bits,&ipv6_zero,ipv6_deadline(valid),256,true);
                if((flags&0x40) && bits==64 && preferred<=valid)ipv6_slaac(prefix.s6_addr,valid,preferred);
            }}
        off+=n;
    }
}
/** @brief Remember a validated path MTU update from an ICMPv6 quote. @param quote IPv6 header.
 * @param length Quote length. @param mtu Reported MTU. */
static void ipv6_update_pmtu(const uint8_t *quote,uint32_t length,uint32_t mtu)
{
    if(length<40 || quote[0]>>4!=6)return;
    struct in6_addr src,dst;net_memcpy(src.s6_addr,quote+8,16);net_memcpy(dst.s6_addr,quote+24,16);
    struct ipv6_address_slot *a=ipv6_find_address(&src,0);if(!a || ipv6_multicast(&dst))return;
    if(mtu<IPV6_MIN_MTU)mtu=IPV6_MIN_MTU;
    if(mtu>IPV6_LINK_MTU)return;
    struct ipv6_pmtu *slot=0;
    for(uint32_t i=0;i<IPV6_PMTU_MAX;i++){
        if(ipv6_pmtu[i].used && ipv6_pmtu[i].ifindex==a->value.ifindex && ipv6_equal(&ipv6_pmtu[i].destination,&dst)){
            slot=&ipv6_pmtu[i];break;}if(!slot && (!ipv6_pmtu[i].used || ipv6_expired(ipv6_pmtu[i].deadline,time_uptime_ms())))slot=&ipv6_pmtu[i];}
    if(!slot)return;
    if(!slot->used || ipv6_expired(slot->deadline,time_uptime_ms()) || mtu<slot->mtu)slot->mtu=mtu;
    slot->used=true;slot->destination=dst;slot->ifindex=a->value.ifindex;slot->deadline=time_uptime_ms()+600000;
}
/** @brief Decide whether an error response would answer another ICMP error or an unknowable fragment.
 * @param ip Original IPv6 packet. @param length Available quote. @return True when errors must be suppressed. */
static bool ipv6_error_suppressed(const uint8_t *ip,uint32_t length)
{
    uint32_t off=40,count=0;uint8_t next=ip[6];
    while(next==0 || next==43 || next==44 || next==60){
        if(++count>IPV6_HEADER_COUNT || length-off<8)return true;
        if(next==44){if(net_get_u16(ip+off+2)&0xfff8u)return true;
            next=ip[off];off+=8;continue;}
        uint32_t n=((uint32_t)ip[off+1]+1u)*8u;if(n>length-off)return true;
        next=ip[off];off+=n;
    }
    return next==58 && (off>=length || ip[off]<128);
}
/** @brief Generate a rate-limited ICMPv6 error for a locally received packet.
 * @param ip Original IPv6 packet. @param length Available bytes. @param type Error type.
 * @param code Error code. @param word Type-specific field. @param allow_multicast Permit RFC option exception. */
static void ipv6_error(const uint8_t *ip,uint32_t length,uint8_t type,uint8_t code,uint32_t word,bool allow_multicast)
{
    if(length<40)return;
    struct in6_addr src,dst;net_memcpy(src.s6_addr,ip+8,16);net_memcpy(dst.s6_addr,ip+24,16);
    if(ipv6_unspecified(&src) || ipv6_multicast(&src) || (!allow_multicast && ipv6_multicast(&dst)))return;
    if(ipv6_error_suppressed(ip,length))return;
    uint64_t now=time_uptime_ms();if(now-ipv6_error_at>=1000){ipv6_error_at=now;ipv6_error_count=0;}
    if(ipv6_error_count>=10)return;
    struct net_ipv6_path path;if(net_ipv6_route(&src,ipv6_linklocal(&src)?2:0,
        ipv6_multicast(&dst)?0:&dst,&path)<0)return;
    uint8_t error[1240]={0};uint32_t n=length;if(n>1232)n=1232;
    error[0]=type;error[1]=code;net_put_u32(error+4,word);net_memcpy(error+8,ip,n);
    net_put_u16(error+2,net_ipv6_checksum(&path.source,&path.destination,58,error,n+8));
    if(net_ipv6_output(&path,58,error,n+8,64,0,false,false)>=0)++ipv6_error_count;
}
/** @brief Process a validated ICMPv6 message. @param ip Original packet.
 * @param ip_length IPv6 bytes. @param icmp Message. @param length Message bytes.
 * @param ifindex Receiving interface. @param ethernet_mac Source Ethernet MAC or NULL for loopback.
 * @param fragmented True when any fragment header was present. */
static void ipv6_icmp(const uint8_t *ip,uint32_t ip_length,const uint8_t *icmp,uint32_t length,
                      uint32_t ifindex,const uint8_t *ethernet_mac,bool fragmented)
{
    struct in6_addr src,dst;net_memcpy(src.s6_addr,ip+8,16);net_memcpy(dst.s6_addr,ip+24,16);
    if(length<8)return;
    if(icmp[0]==128 && icmp[1]==0 && !ipv6_unspecified(&src) && !ipv6_multicast(&src)){
        struct net_ipv6_path path;const struct in6_addr *bound=ipv6_multicast(&dst)?0:&dst;
        if(net_ipv6_route(&src,ipv6_linklocal(&src)?ifindex:0,bound,&path)<0)return;
        uint8_t *reply=kernel_malloc(length);if(!reply)return;net_memcpy(reply,icmp,length);reply[0]=129;
        reply[2]=reply[3]=0;net_put_u16(reply+2,net_ipv6_checksum(&path.source,&path.destination,58,reply,length));
        (void)net_ipv6_output(&path,58,reply,length,64,0,false,false);kernel_free(reply);return;
    }
    if(icmp[0]>=1 && icmp[0]<=4 && icmp[1]<=4 && length>=48){
        int error=icmp[0]==1?(icmp[1]==4?LINUX_ECONNREFUSED:LINUX_EHOSTUNREACH):
            (icmp[0]==2?LINUX_EMSGSIZE:(icmp[0]==3?LINUX_ETIMEDOUT:LINUX_EPROTO));
        uint32_t mtu=icmp[0]==2?net_get_u32(icmp+4):0;
        if(icmp[0]==2)ipv6_update_pmtu(icmp+8,length-8,mtu);
        net_ipv6_socket_error(icmp+8,length-8,error,mtu);return;
    }
    if(ifindex!=2 || fragmented || ip[7]!=255 || icmp[1]!=0 || !ethernet_mac)return;
    if(icmp[0]==134){ipv6_router_advertisement(&src,icmp,length);return;}
    if((icmp[0]==135 || icmp[0]==136) && length>=24){
        struct in6_addr target;net_memcpy(target.s6_addr,icmp+8,16);if(ipv6_multicast(&target))return;
        const uint8_t *mac=0;if(!ipv6_nd_options(icmp+24,length-24,icmp[0]==135?1:2,&mac))return;
        if(icmp[0]==135){
            if(ipv6_unspecified(&src)){struct in6_addr group;ipv6_solicited(&group,&target);
                if(mac || !ipv6_equal(&dst,&group))return;}
            else if(mac)ipv6_learn_neighbor(&src,mac,false,true);
            struct ipv6_address_slot *slot=ipv6_find_address(&target,ifindex);if(!slot)return;
            if(slot->value.flags&NET_IPV6_ADDR_TENTATIVE){if(!net_memeq(ethernet_mac,e1000_mac(),6))ipv6_dad_failed(slot);return;}
            if(slot->value.flags&NET_IPV6_ADDR_DADFAILED)return;
            if(ipv6_unspecified(&src)){uint8_t allmac[6];ipv6_multicast_mac(allmac,&ipv6_all_nodes);
                ipv6_send_na(&target,&ipv6_all_nodes,allmac,false);}
            else ipv6_send_na(&target,&src,ethernet_mac,true);
        }else{
            if(ipv6_unspecified(&src) || (ipv6_multicast(&dst) && (icmp[4]&0x40)))return;
            struct ipv6_address_slot *slot=ipv6_find_address(&target,ifindex);
            if(slot && slot->value.flags&NET_IPV6_ADDR_TENTATIVE){if(!net_memeq(ethernet_mac,e1000_mac(),6))ipv6_dad_failed(slot);return;}
            if(mac && ipv6_find_neighbor(&target))ipv6_learn_neighbor(&target,mac,(icmp[4]&0x40)!=0,(icmp[4]&0x20)!=0);
        }
    }
    (void)ip_length;
}

/** @brief Check that a first fragment carries its complete upper-layer header.
 * @param next Fragment next header. @param data Fragmentable bytes. @param length Bytes.
 * @return True if the entire extension and transport header is present. */
static bool ipv6_first_complete(uint8_t next,const uint8_t *data,uint32_t length)
{
    uint32_t off=0,count=0;
    while(next==43 || next==60){
        if(++count>IPV6_HEADER_COUNT || length-off<8)return false;
        uint32_t n=((uint32_t)data[off+1]+1)*8u;if(n>length-off)return false;
        next=data[off];off+=n;
    }
    if(next==0 || next==44)return false;
    if(next==17 || next==58)return length-off>=8;
    if(next==6)return length-off>=20 && (data[off+12]>>4)>=5 && (uint32_t)(data[off+12]>>4)*4u<=length-off;
    return true;
}
/** @brief Release the storage for a reassembly entry. @param r Entry to clear. */
static void ipv6_reassembly_clear(struct ipv6_reassembly *r)
{
    kernel_free(r->data);kernel_free(r->bitmap);kernel_free(r->prefix);net_memzero(r,sizeof(*r));
}
/** @brief Expire a reassembly and quote its first fragment when available.
 * @param r Expired entry; released before return. */
static void ipv6_reassembly_expire(struct ipv6_reassembly *r)
{
    if(r->have_first && !r->poisoned && r->prefix){
        uint8_t quote[1232]={0};uint32_t n=r->prefix_len;
        if(n>sizeof(quote))n=sizeof(quote);
        net_memcpy(quote,r->prefix,n);
        if(n==r->prefix_len && n+8<=sizeof(quote)){
            quote[n]=r->next;net_put_u16(quote+n+2,1);net_put_u32(quote+n+4,r->identification);n+=8;
            for(uint32_t i=0;i<r->highwater && n<sizeof(quote);i++){
                if(!(r->bitmap[i>>3]&(1u<<(i&7u))))break;
                quote[n++]=r->data[i];
            }
        }
        ipv6_error(quote,n,3,1,0,false);
    }
    ipv6_reassembly_clear(r);
}
/** @brief Poison an overlapping or inconsistent reassembly until its timeout. @param r Entry. */
static void ipv6_reassembly_poison(struct ipv6_reassembly *r)
{
    kernel_free(r->data);kernel_free(r->bitmap);kernel_free(r->prefix);
    r->data=0;r->bitmap=0;r->prefix=0;r->poisoned=true;
}
/** @brief Validate and dispatch an IPv6 packet. @param ip Borrowed packet. @param length Bytes.
 * @param ifindex Receiving interface. @param mac Ethernet source or NULL. @param reassembled Reassembly guard. */
static void ipv6_receive_ip(const uint8_t *ip,uint32_t length,uint32_t ifindex,const uint8_t *mac,bool reassembled);

/** @brief Insert a fragment into bounded storage, rejecting any overlap. @param ip Original packet.
 * @param ip_length Packet bytes. @param off Fragment header offset. @param previous Previous next-header offset.
 * @param ifindex Receiving interface. */
static void ipv6_fragment(const uint8_t *ip,uint32_t ip_length,uint32_t off,uint32_t previous,uint32_t ifindex)
{
    const uint8_t *fragment=ip+off;uint16_t field=net_get_u16(fragment+2);
    uint32_t offset=field&0xfff8u,n=ip_length-off-8u,ident=net_get_u32(fragment+4);
    bool more=(field&1)!=0;if((more && (!n || (n&7u))) || offset+n>65535u || off-40u+offset+n>65535u)return;
    if(offset==0 && !ipv6_first_complete(fragment[0],fragment+8,n)){
        ipv6_error(ip,ip_length,4,3,0,false);return;}
    struct in6_addr src,dst;net_memcpy(src.s6_addr,ip+8,16);net_memcpy(dst.s6_addr,ip+24,16);
    struct ipv6_reassembly *r=0,*free_slot=0;
    for(uint32_t i=0;i<IPV6_REASM_MAX;i++){
        struct ipv6_reassembly *e=&ipv6_reassembly[i];
        if(e->used && ipv6_expired(e->deadline,time_uptime_ms()))ipv6_reassembly_expire(e);
        if(e->used && e->identification==ident && ipv6_equal(&e->source,&src) && ipv6_equal(&e->destination,&dst)){
            r=e;break;}if(!e->used && !free_slot)free_slot=e;
    }
    if(!r){if(!free_slot)return;r=free_slot;r->data=kernel_malloc(65535);r->bitmap=kernel_malloc(8192);
        if(!r->data || !r->bitmap){ipv6_reassembly_clear(r);return;}
        net_memzero(r->bitmap,8192);r->used=true;r->source=src;r->destination=dst;
        r->identification=ident;r->next=fragment[0];r->deadline=time_uptime_ms()+IPV6_REASM_TIMEOUT;
    }
    if(r->poisoned)return;
    if(r->next!=fragment[0] || (r->have_last && offset+n>r->total) ||
       (!more && r->have_last && r->total!=offset+n) || (!more && r->highwater>offset+n)){
        ipv6_reassembly_poison(r);return;}
    for(uint32_t i=0;i<n;i++)if(r->bitmap[(offset+i)>>3]&(1u<<((offset+i)&7u))){ipv6_reassembly_poison(r);return;}
    if(offset==0){r->prefix=kernel_malloc(off);if(!r->prefix){ipv6_reassembly_clear(r);return;}
        net_memcpy(r->prefix,ip,off);r->prefix_len=off;r->prev_next=previous;r->have_first=true;}
    for(uint32_t i=0;i<n;i++)r->bitmap[(offset+i)>>3]|=(uint8_t)(1u<<((offset+i)&7u));
    if(n)net_memcpy(r->data+offset,fragment+8,n);
    r->covered+=n;if(offset+n>r->highwater)r->highwater=offset+n;
    if(!more){r->have_last=true;r->total=offset+n;}
    if(!r->have_first || !r->have_last || r->covered!=r->total)return;
    if(r->prefix_len-40u+r->total>65535u){ipv6_reassembly_clear(r);return;}
    uint32_t total=r->prefix_len+r->total;uint8_t *packet=kernel_malloc(total);
    if(!packet){ipv6_reassembly_clear(r);return;}
    net_memcpy(packet,r->prefix,r->prefix_len);packet[r->prev_next]=r->next;
    net_put_u16(packet+4,(uint16_t)(total-40));net_memcpy(packet+r->prefix_len,r->data,r->total);
    ipv6_reassembly_clear(r);ipv6_receive_ip(packet,total,ifindex,0,true);kernel_free(packet);
}
/** @brief Parse hop-by-hop or destination options with RFC unknown-option actions.
 * @param ip Packet. @param total Packet bytes. @param off Extension offset.
 * @param length Extension length. @param router_alert Set when the MLD router alert is present.
 * @return True when processing may continue. */
static bool ipv6_options(const uint8_t *ip,uint32_t total,uint32_t off,uint32_t length,bool *router_alert)
{
    uint32_t pos=off+2,end=off+length;
    while(pos<end){uint8_t type=ip[pos];if(type==0){++pos;continue;}
        if(end-pos<2 || (uint32_t)ip[pos+1]+2u>end-pos){ipv6_error(ip,total,4,0,pos,false);return false;}
        uint32_t n=(uint32_t)ip[pos+1]+2u;
        if(type==5 && n==4 && ip[pos+2]==0 && ip[pos+3]==0)*router_alert=true;
        else if(type!=1){uint8_t action=type>>6;if(action){
            if(action==2 || action==3){ipv6_error(ip,total,4,2,pos,action==2);}
            return false;}}
        pos+=n;
    }
    return true;
}
/** @brief Decode the MLD query maximum-response code.
 * @param code Wire field. @param v2 True for MLDv2 floating encoding. @return Maximum response milliseconds. */
static uint32_t ipv6_mld_response_ms(uint16_t code,bool v2)
{
    if(!v2 || code<32768)return code;
    return (uint32_t)((code&0x0fffu)|0x1000u)<<(((code>>12)&7u)+3u);
}
/** @brief Schedule listener reports in response to a valid MLD query. @param ip IPv6 packet.
 * @param icmp Query. @param length Bytes. @param router_alert Hop-by-hop router alert was present. */
static void ipv6_mld_query(const uint8_t *ip,const uint8_t *icmp,uint32_t length,bool router_alert)
{
    if(ip[7]!=1 || !router_alert || length<24 || icmp[1])return;
    struct in6_addr source,group;net_memcpy(source.s6_addr,ip+8,16);net_memcpy(group.s6_addr,icmp+8,16);
    if(!ipv6_unspecified(&source) && !ipv6_linklocal(&source))return;
    if(!ipv6_unspecified(&group) && !ipv6_multicast(&group))return;
    bool v2=length!=24;
    if(!v2)ipv6_mld_v1_until=time_uptime_ms()+260000;
    else if(length<28 || (uint32_t)net_get_u16(icmp+26)*16u>length-28)return;
    uint32_t maximum=ipv6_mld_response_ms(net_get_u16(icmp+4),v2);
    for(uint32_t i=0;i<IPV6_GROUP_MAX;i++)if(ipv6_groups[i].used && ipv6_groups[i].refs &&
        ipv6_groups[i].ifindex==2 && (ipv6_unspecified(&group) || ipv6_equal(&group,&ipv6_groups[i].address))){
        uint64_t deadline=time_uptime_ms()+ipv6_jitter(maximum);
        if(!ipv6_groups[i].query_pending || deadline<ipv6_groups[i].query_at)ipv6_groups[i].query_at=deadline;
        ipv6_groups[i].query_pending=true;}
}
/** @brief Validate extension headers and transport checksums before delivery.
 * @param ip Borrowed packet. @param length Available bytes. @param ifindex Interface.
 * @param mac Ethernet source or NULL. @param reassembled True for a reconstructed packet. */
static void ipv6_receive_ip(const uint8_t *ip,uint32_t length,uint32_t ifindex,const uint8_t *mac,bool reassembled)
{
    if(length<40 || ip[0]>>4!=6)return;
    uint32_t total=40u+net_get_u16(ip+4);if(total>length)return;
    struct in6_addr src,dst;net_memcpy(src.s6_addr,ip+8,16);net_memcpy(dst.s6_addr,ip+24,16);
    if(ipv6_multicast(&src) || (ifindex==2 && (ipv6_equal(&src,&ipv6_loop) || ipv6_equal(&dst,&ipv6_loop))) ||
       (ifindex==2 && ipv6_multicast(&dst) && (dst.s6_addr[1]&15)<2) || !ipv6_destination_local(&dst,ifindex))return;
    uint32_t off=40,previous=6,count=0;uint8_t next=ip[6];bool fragmented=reassembled,router_alert=false;
    while(next==0 || next==43 || next==44 || next==60){
        if(++count>IPV6_HEADER_COUNT || off>IPV6_HEADER_LIMIT || total-off<8)return;
        if(next==44){
            if(fragmented)return;
            uint16_t field=net_get_u16(ip+off+2);fragmented=true;
            if((field&0xfff9u)!=0){ipv6_fragment(ip,total,off,previous,ifindex);return;}
            next=ip[off];previous=off;off+=8;continue;
        }
        if(next==0 && off!=40){ipv6_error(ip,total,4,1,previous,false);return;}
        uint32_t n=((uint32_t)ip[off+1]+1u)*8u;if(n>total-off || off+n>IPV6_HEADER_LIMIT)return;
        if(next==43 && ip[off+3]){ipv6_error(ip,total,4,0,off+2,false);return;}
        if((next==0 || next==60) && !ipv6_options(ip,total,off,n,&router_alert))return;
        next=ip[off];previous=off;off+=n;
    }
    uint32_t payload=total-off;const uint8_t *data=ip+off;
    if(ipv6_unspecified(&src) && (next!=58 || payload<1 || data[0]!=135))return;
    if(next==59)return;
    if(next==17){
        if(payload<8 || net_get_u16(data+4)!=payload || !net_get_u16(data+6) ||
           net_ipv6_checksum(&src,&dst,17,data,payload))return;
        net_raw6_input(&src,&dst,next,data,payload,ifindex,ip[7]);
        if(!net_udp6_input(&src,&dst,net_get_u16(data),net_get_u16(data+2),data+8,payload-8,ifindex,ip[7]))
            ipv6_error(ip,total,1,4,0,false);
    }else if(next==6){
        if(payload<20 || (data[12]>>4)<5 || (uint32_t)(data[12]>>4)*4u>payload ||
           net_ipv6_checksum(&src,&dst,6,data,payload))return;
        net_raw6_input(&src,&dst,next,data,payload,ifindex,ip[7]);net_tcp6_input(&src,&dst,data,payload,ifindex);
    }else if(next==58){
        if(payload<8 || net_ipv6_checksum(&src,&dst,58,data,payload))return;
        /* RFC 6980: fragmented Neighbor Discovery must not reach raw consumers either. */
        if(fragmented && data[0]>=133 && data[0]<=137)return;
        net_raw6_input(&src,&dst,next,data,payload,ifindex,ip[7]);
        if(data[0]==130 && ifindex==2 && !fragmented)ipv6_mld_query(ip,data,payload,router_alert);
        ipv6_icmp(ip,total,data,payload,ifindex,mac,fragmented);
    }else{
        net_raw6_input(&src,&dst,next,data,payload,ifindex,ip[7]);ipv6_error(ip,total,4,1,previous,false);
    }
}
/** @brief Process one complete IPv6 Ethernet frame. @param frame Borrowed frame.
 * @param length Ethernet bytes including header. */
void net_ipv6_input(const uint8_t *frame,uint32_t length)
{
    if(!frame || length<54 || net_get_u16(frame+12)!=IPV6_ETHERTYPE || !ipv6_link_up)return;
    ipv6_receive_ip(frame+14,length-14,2,frame+6,false);
}

/** @brief Return a usable Ethernet link-local source. @return Borrowed address or NULL. */
static const struct in6_addr *ipv6_link_source(void)
{
    for(uint32_t i=0;i<IPV6_ADDR_MAX;i++)if(ipv6_addrs[i].used && ipv6_addrs[i].value.ifindex==2 &&
       ipv6_linklocal(&ipv6_addrs[i].value.address) &&
       !(ipv6_addrs[i].value.flags&(NET_IPV6_ADDR_TENTATIVE|NET_IPV6_ADDR_DADFAILED)))return &ipv6_addrs[i].value.address;
    return 0;
}
/** @brief Send one MLDv1/v2 group-state report with the mandatory Router Alert.
 * @param group Membership entry. @param query True for a query response. @return True when sent or unnecessary. */
static bool ipv6_send_mld(struct ipv6_group_slot *group,bool query)
{
    if(group->ifindex!=2 || ipv6_equal(&group->address,&ipv6_all_nodes))return true;
    const struct in6_addr *source=ipv6_link_source();if(!source)source=&ipv6_zero;
    bool v1=time_uptime_ms()<ipv6_mld_v1_until;
    struct in6_addr dest=v1?(group->refs?group->address:ipv6_all_routers):ipv6_mld_routers;
    uint8_t payload[36]={58,0,5,2,0,0,1,0};uint8_t *icmp=payload+8;uint32_t n;
    if(v1){n=24;icmp[0]=(uint8_t)(group->refs?131:132);net_memcpy(icmp+8,group->address.s6_addr,16);}
    else{n=28;icmp[0]=143;net_put_u16(icmp+6,1);icmp[8]=(uint8_t)(group->refs?(query?2:4):3);
        net_memcpy(icmp+12,group->address.s6_addr,16);}
    net_put_u16(icmp+2,net_ipv6_checksum(source,&dest,58,icmp,n));uint8_t mac[6];ipv6_multicast_mac(mac,&dest);
    return ipv6_send_frame(mac,source,&dest,0,1,0,payload,n+8)>=0;
}
/** @brief Tear down owned reassembly and queued payload state for one link. */
static void ipv6_clear_link_state(void)
{
    for(uint32_t i=0;i<IPV6_PENDING_MAX;i++)if(ipv6_pending[i].used && ipv6_pending[i].path.ifindex==2){
        ipv6_pending_error(&ipv6_pending[i],LINUX_ENETDOWN);kernel_free(ipv6_pending[i].data);ipv6_pending[i].used=false;}
    for(uint32_t i=0;i<IPV6_REASM_MAX;i++)if(ipv6_reassembly[i].used)ipv6_reassembly_clear(&ipv6_reassembly[i]);
    for(uint32_t i=0;i<IPV6_ADDR_MAX;i++)if(ipv6_addrs[i].used && ipv6_addrs[i].value.ifindex==2)ipv6_addrs[i].used=false;
    for(uint32_t i=0;i<IPV6_ROUTE_MAX;i++)if(ipv6_routes[i].used && ipv6_routes[i].value.ifindex==2)ipv6_routes[i].used=false;
    for(uint32_t i=0;i<IPV6_GROUP_MAX;i++)if(ipv6_groups[i].used && ipv6_groups[i].ifindex==2)ipv6_groups[i].used=false;
    net_memzero(ipv6_neighbors,sizeof(ipv6_neighbors));net_memzero(ipv6_pmtu,sizeof(ipv6_pmtu));
    ipv6_link_mtu=IPV6_LINK_MTU;ipv6_rs_sent=0;ipv6_mld_v1_until=0;
    ipv6_base_reachable_ms=30000;ipv6_retrans_ms=1000;netlink_ipv6_changed();
}
/** @brief Change administrative Ethernet state and rebuild link-local discovery when enabled.
 * @param up Administrative state; no NIC IRQ ownership is changed. */
void net_ipv6_link_changed(bool up)
{
    if(!ipv6_initialized)return;
    bool enabled=up && e1000_is_ready();
    if(enabled==ipv6_link_up)return;
    ipv6_link_up=enabled;if(!enabled){ipv6_clear_link_state();return;}
    ipv6_reset_timing();
    const uint8_t *mac=e1000_mac();struct net_ipv6_address a={0};a.address.s6_addr[0]=0xfe;a.address.s6_addr[1]=0x80;
    a.address.s6_addr[8]=mac[0]^2;a.address.s6_addr[9]=mac[1];a.address.s6_addr[10]=mac[2];
    a.address.s6_addr[11]=0xff;a.address.s6_addr[12]=0xfe;a.address.s6_addr[13]=mac[3];
    a.address.s6_addr[14]=mac[4];a.address.s6_addr[15]=mac[5];a.ifindex=2;a.prefix_length=64;
    a.flags=NET_IPV6_ADDR_PERMANENT;a.preferred_until=a.valid_until=IPV6_FOREVER;
    (void)net_ipv6_address_update(&a,false);
    struct in6_addr prefix={.s6_addr={0xfe,0x80}};ipv6_auto_route(&prefix,64,&ipv6_zero,IPV6_FOREVER,256,false);
    ipv6_rs_at=IPV6_FOREVER;ipv6_rs_sent=0;
}
/** @brief Retire NIC-specific addresses, routes, neighbors, fragments and queued packets. */
void net_ipv6_detached(void)
{
    if(ipv6_link_up){ipv6_link_up=false;ipv6_clear_link_state();}
}
/** @brief Initialize loopback and Ethernet host state without waiting for discovery. */
void net_ipv6_init(void)
{
    if(ipv6_initialized){
        for(uint32_t i=0;i<IPV6_PENDING_MAX;i++)if(ipv6_pending[i].used)kernel_free(ipv6_pending[i].data);
        for(uint32_t i=0;i<IPV6_REASM_MAX;i++)if(ipv6_reassembly[i].used)ipv6_reassembly_clear(&ipv6_reassembly[i]);
    }
    net_memzero(ipv6_addrs,sizeof(ipv6_addrs));net_memzero(ipv6_routes,sizeof(ipv6_routes));
    net_memzero(ipv6_neighbors,sizeof(ipv6_neighbors));net_memzero(ipv6_groups,sizeof(ipv6_groups));
    net_memzero(ipv6_pending,sizeof(ipv6_pending));net_memzero(ipv6_reassembly,sizeof(ipv6_reassembly));
    net_memzero(ipv6_pmtu,sizeof(ipv6_pmtu));ipv6_link_up=false;ipv6_initialized=true;
    ipv6_link_mtu=IPV6_LINK_MTU;ipv6_frag_id=0;ipv6_rng_state=0;ipv6_error_at=0;ipv6_error_count=0;
    ipv6_mld_v1_until=0;ipv6_base_reachable_ms=30000;ipv6_retrans_ms=1000;
    struct net_ipv6_address loop={0};loop.address=ipv6_loop;loop.ifindex=1;loop.prefix_length=128;
    loop.flags=NET_IPV6_ADDR_PERMANENT|NET_IPV6_ADDR_NODAD;loop.preferred_until=loop.valid_until=IPV6_FOREVER;
    (void)net_ipv6_address_update(&loop,false);
    struct net_ipv6_route route={0};route.destination=ipv6_loop;route.ifindex=1;route.prefix_length=128;
    route.valid_until=IPV6_FOREVER;route.flags=RTF_UP|RTF_HOST;(void)net_ipv6_route_update(&route,false);
    net_ipv6_link_changed(net_interface_up);
}
/** @brief Check if task-context receive or timer service should continue.
 * @return True while an Ethernet interface or deferred loopback packet is active. */
bool net_ipv6_maintenance_pending(void)
{
    if(ipv6_initialized && net_interface_up && e1000_is_ready())return true;
    for(uint32_t i=0;i<IPV6_PENDING_MAX;i++)if(ipv6_pending[i].used && ipv6_pending[i].loopback)return true;
    return false;
}
/** @brief Deliver a retained packet locally without recursing into a socket send call.
 * @param p Owned pending packet, detached from the queue before transport delivery. */
static void ipv6_loop_deliver(struct ipv6_pending *p)
{
    uint8_t *packet=kernel_malloc(40u+p->length);if(!packet){ipv6_pending_error(p,LINUX_ENOMEM);return;}
    net_memzero(packet,40);packet[0]=0x60;net_put_u16(packet+4,(uint16_t)p->length);packet[6]=p->protocol;
    packet[7]=(uint8_t)(p->hops<0?64:p->hops);net_memcpy(packet+8,p->path.source.s6_addr,16);
    net_memcpy(packet+24,p->path.destination.s6_addr,16);if(p->length)net_memcpy(packet+40,p->data,p->length);
    ipv6_receive_ip(packet,40u+p->length,p->path.ifindex,0,false);kernel_free(packet);
}
/** @brief Advance bounded address, neighbor, listener, reassembly and deferred-loopback work. */
void net_ipv6_poll(void)
{
    if(!ipv6_initialized)return;
    if(!ipv6_link_up && net_interface_up && e1000_is_ready())net_ipv6_link_changed(true);
    uint64_t now=time_uptime_ms();
    for(uint32_t i=0;i<IPV6_PENDING_MAX;i++)if(ipv6_pending[i].used && ipv6_pending[i].loopback){
        struct ipv6_pending p=ipv6_pending[i];ipv6_pending[i].used=false;ipv6_loop_deliver(&p);kernel_free(p.data);}
    if(ipv6_link_up){
        if(now>=ipv6_reachable_refresh)ipv6_refresh_reachable();
        /* Report a new solicited-node membership before sending its DAD probe. */
        for(uint32_t i=0;i<IPV6_GROUP_MAX;i++){
            struct ipv6_group_slot *g=&ipv6_groups[i];if(!g->used)continue;
            if(g->query_pending && now>=g->query_at && ipv6_send_mld(g,true))g->query_pending=false;
            if(g->reports && now>=g->report_at && ipv6_send_mld(g,false)){
                --g->reports;g->report_at=now+ipv6_jitter(1000);
                if(!g->reports && !g->refs)g->used=false;
            }
        }
    }
    for(uint32_t i=0;i<IPV6_ADDR_MAX;i++){
        struct ipv6_address_slot *a=&ipv6_addrs[i];if(!a->used)continue;
        if(ipv6_expired(a->value.valid_until,now)){struct net_ipv6_address old=a->value;
            (void)net_ipv6_address_update(&old,true);continue;}
        if(!(a->value.flags&NET_IPV6_ADDR_DEPRECATED) && ipv6_expired(a->value.preferred_until,now)){
            a->value.flags|=NET_IPV6_ADDR_DEPRECATED;netlink_ipv6_changed();}
        if(ipv6_link_up && (a->value.flags&NET_IPV6_ADDR_TENTATIVE) && now>=a->dad_at){
            if(!a->dad_sent && a->dad_wait_mld){struct in6_addr group;ipv6_solicited(&group,&a->value.address);
                struct ipv6_group_slot *g=ipv6_find_group(&group,a->value.ifindex);
                if(g && g->reports==2)continue;
                a->dad_wait_mld=false;}
            if(!a->dad_sent){if(ipv6_send_ns(&a->value.address,&ipv6_zero,0)>=0){a->dad_sent=true;a->dad_at=now+ipv6_retrans_ms;}}
            else{a->value.flags&=~NET_IPV6_ADDR_TENTATIVE;netlink_ipv6_changed();}
        }
    }
    for(uint32_t i=0;i<IPV6_ROUTE_MAX;i++)if(ipv6_routes[i].used && ipv6_expired(ipv6_routes[i].value.valid_until,now)){
        ipv6_routes[i].used=false;netlink_ipv6_changed();}
    for(uint32_t i=0;i<IPV6_REASM_MAX;i++)if(ipv6_reassembly[i].used && ipv6_expired(ipv6_reassembly[i].deadline,now))
        ipv6_reassembly_expire(&ipv6_reassembly[i]);
    if(!ipv6_link_up)return;
    const struct in6_addr *link_source=ipv6_link_source();
    if(link_source && !ipv6_rs_sent && ipv6_rs_at==IPV6_FOREVER)ipv6_rs_at=now+ipv6_jitter(1000);
    if(link_source && ipv6_rs_sent<3 && now>=ipv6_rs_at){
        if(ipv6_send_rs(link_source)>=0){++ipv6_rs_sent;ipv6_rs_at=now+4000;}}
    for(uint32_t i=0;i<IPV6_NEIGH_MAX;i++){
        struct ipv6_neighbor_slot *n=&ipv6_neighbors[i];if(!n->used || now<n->deadline)continue;
        if(n->value.state==IPV6_NUD_REACHABLE){n->value.state=IPV6_NUD_STALE;netlink_ipv6_changed();}
        else if(n->value.state==IPV6_NUD_DELAY){n->value.state=IPV6_NUD_PROBE;n->probes=0;n->deadline=now;}
        if(n->value.state==IPV6_NUD_INCOMPLETE || n->value.state==IPV6_NUD_PROBE){
            if(n->probes>=3){n->value.state=IPV6_NUD_FAILED;netlink_ipv6_changed();
                for(uint32_t j=0;j<IPV6_PENDING_MAX;j++)if(ipv6_pending[j].used && !ipv6_pending[j].loopback &&
                    ipv6_equal(&ipv6_pending[j].path.next_hop,&n->value.address)){
                    ipv6_pending_error(&ipv6_pending[j],LINUX_EHOSTUNREACH);kernel_free(ipv6_pending[j].data);ipv6_pending[j].used=false;}
            }else if(link_source){(void)ipv6_send_ns(&n->value.address,link_source,
                n->value.state==IPV6_NUD_PROBE?n->value.mac:0);++n->probes;n->deadline=now+ipv6_retrans_ms;}
        }
    }
}
