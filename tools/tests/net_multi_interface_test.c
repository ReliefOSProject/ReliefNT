/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the real interface ioctl and route code with two independent NICs. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../net/internal.h"
#include <linux/ipv6_route.h>
#include "../../net/socket/netlink.c"

static struct task current;
static bool carrier[4] = {false, false, true, true};
static uint8_t macs[4][6] = {{0}, {0}, {2,0,0,0,0,2}, {2,0,0,0,0,3}};
struct task *sched_current_task(void) { return &current; }
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *memory) { free(memory); }
bool user_range_ok(uint64_t address, uint64_t size) { return address >= 4096 && size <= 65536; }
bool user_range_writable(uint64_t address, uint64_t size) { return user_range_ok(address, size); }
int netdev_is_ready(uint32_t index) { return index < 4 && carrier[index]; }
const uint8_t *netdev_mac(uint32_t index) { return macs[index < 4 ? index : 0]; }
void netdev_get_info(uint32_t index, struct e1000_info *out)
{
    *out = (struct e1000_info){.present = index == 2 || index == 3,
                               .active = index < 4 && carrier[index]};
    memcpy(out->mac, netdev_mac(index), 6);
}
int net_ipv6_link_changed_on(uint32_t index, bool up) { (void)index; (void)up; return 0; }
uint64_t time_uptime_ms(void) { return 1000; }
void kernel_poll_readiness_notify(void) { }
uint32_t net_ipv6_addresses(struct net_ipv6_address *out, uint32_t capacity)
{ (void)out; (void)capacity; return 0; }
uint32_t net_ipv6_routes(struct net_ipv6_route *out, uint32_t capacity)
{ (void)out; (void)capacity; return 0; }
int net_ipv6_address_update(const struct net_ipv6_address *address, bool remove)
{ (void)address; (void)remove; return 0; }
int net_ipv6_route_update(const struct net_ipv6_route *route, bool remove)
{ (void)route; (void)remove; return 0; }
void net_arp_cache_clear_on(uint32_t index) { (void)index; }

static int call(unsigned command, struct ifreq *req)
{ return net_interface_ioctl(command, (uintptr_t)req); }
static int netlink_request_error(const struct nlmsghdr *request)
{
    struct netlink_message *message=kernel_malloc(sizeof(*message)+NL_MESSAGE_MAX);
    assert(message);
    struct nl_builder builder={.message=message};
    assert(!nl_request(&builder,request));
    assert(builder.used>=sizeof(struct nlmsghdr)+sizeof(struct nlmsgerr));
    struct nlmsghdr *reply=(void *)message->data;
    assert(reply->nlmsg_type==NLMSG_ERROR);
    int error=((struct nlmsgerr *)(reply+1))->error;
    kernel_free(message);
    return error;
}
static void set_address(const char *name, unsigned command, uint32_t ip)
{
    struct ifreq req = {0};
    strcpy(req.ifr_name, name);
    req.data.addr.sa_family = AF_INET;
    net_put_u32((uint8_t *)&req.data.addr + 4, ip);
    assert(call(command, &req) == 0);
}

int main(void)
{
    struct ifconf conf = {0};
    assert(net_interface_ioctl(SIOCGIFCONF, (uintptr_t)&conf) == 0);
    assert(conf.ifc_len == sizeof(struct ifreq));
    assert(!net_interface_register(2) && !net_interface_register(3));
    struct { struct nlmsghdr header; struct ifinfomsg info; } link={
        .header={.nlmsg_len=sizeof(link),.nlmsg_type=RTM_NEWLINK,
                 .nlmsg_flags=NLM_F_REQUEST|NLM_F_ACK},
        .info={.ifi_family=AF_UNSPEC,.ifi_index=3,.ifi_change=IFF_UP}};
    assert(netlink_request_error(&link.header)==-LINUX_EPERM);
    assert(net_interface_is_up(2) && net_interface_is_up(3));
    current.cap_effective=1ULL << CAP_NET_ADMIN;
    assert(!netlink_request_error(&link.header));
    assert(net_interface_is_up(2) && !net_interface_is_up(3));
    struct { struct nlmsghdr header; struct ifinfomsg info;
             struct rtattr name; char value[8]; struct rtattr mtu; uint32_t size; } named_link={
        .header={.nlmsg_len=sizeof(named_link),.nlmsg_type=19,
                 .nlmsg_flags=NLM_F_REQUEST|NLM_F_ACK},
        .info={.ifi_family=AF_UNSPEC,.ifi_flags=IFF_UP,.ifi_change=IFF_UP},
        .name={.rta_len=RTA_LENGTH(5),.rta_type=IFLA_IFNAME},.value="eth1",
        .mtu={.rta_len=RTA_LENGTH(4),.rta_type=IFLA_MTU},.size=1500};
    assert(!netlink_request_error(&named_link.header));
    assert(net_interface_is_up(3));
    named_link.size=1400;
    named_link.info.ifi_flags=0;
    assert(netlink_request_error(&named_link.header)==-LINUX_EOPNOTSUPP);
    assert(net_interface_is_up(3));
    named_link.size=1500;
    named_link.mtu.rta_type=IFLA_ADDRESS;
    assert(netlink_request_error(&named_link.header)==-LINUX_EOPNOTSUPP);
    assert(net_interface_is_up(3));
    current.cap_effective=0;
    struct ifreq req = {.ifr_name = "eth1"};
    assert(!call(SIOCGIFINDEX, &req) && req.data.value == 3);
    assert(!call(SIOCGIFHWADDR, &req) && req.data.addr.sa_data[5] == 3);
    struct {
        struct nlmsghdr header;
        struct ifaddrmsg address;
        struct rtattr local;
        uint8_t ip[16];
        struct rtattr label;
        char name[8];
    } address6 = {.header={.nlmsg_len=sizeof(address6),.nlmsg_flags=NLM_F_CREATE},
        .address={.ifa_family=AF_INET6,.ifa_prefixlen=64,.ifa_index=3},
        .local={.rta_len=RTA_LENGTH(16),.rta_type=IFA_LOCAL},.ip={0x20,1,0xdb,8},
        .label={.rta_len=RTA_LENGTH(5),.rta_type=IFA_LABEL},.name="eth1"};
    assert(!nl_change_address(&address6.header,false));
    strcpy(req.ifr_name, "eth01");
    assert(call(SIOCGIFINDEX, &req) == -LINUX_ENODEV);

    strcpy(req.ifr_name, "eth1");
    assert(call(SIOCGIFTXQLEN, &req) == 0 && req.data.value == 0);
    req.data.addr.sa_family = AF_INET;
    net_put_u32((uint8_t *)&req.data.addr + 4, 0xc0a8070fu);
    assert(call(SIOCSIFADDR, &req) == -LINUX_EPERM);
    current.cap_effective = 1ULL << CAP_NET_ADMIN;
    set_address("eth0", SIOCSIFADDR, 0x0a00020fu);
    set_address("eth0", SIOCSIFNETMASK, 0xffffff00u);
    set_address("eth1", SIOCSIFADDR, 0xc0a8070fu);
    set_address("eth1", SIOCSIFNETMASK, 0xffffff00u);
    struct ifreq list[3];
    conf = (struct ifconf){.ifc_len = sizeof(list), .ifc_buf = (uintptr_t)list};
    assert(!net_interface_ioctl(SIOCGIFCONF, (uintptr_t)&conf));
    assert(conf.ifc_len == sizeof(list));
    assert(!strcmp(list[0].ifr_name, "lo") && !strcmp(list[1].ifr_name, "eth0") &&
           !strcmp(list[2].ifr_name, "eth1"));
    assert(net_get_u32((uint8_t *)&list[1].data.addr + 4) == 0x0a00020fu);
    assert(net_get_u32((uint8_t *)&list[2].data.addr + 4) == 0xc0a8070fu);
    conf.ifc_len = 2 * sizeof(struct ifreq) + 1;
    assert(!net_interface_ioctl(SIOCGIFCONF, (uintptr_t)&conf));
    assert(conf.ifc_len == 2 * sizeof(struct ifreq));

    struct net_ipv4_path path;
    assert(!net_ipv4_route_lookup(0xc0a80732u, 0x0a00020fu, 0, &path));
    assert(path.ifindex == 3 && path.source == 0x0a00020fu && path.next_hop == 0xc0a80732u);
    char eth1[] = "eth1";
    struct rtentry route = {.rt_dst.sa_family = AF_INET, .rt_gateway.sa_family = AF_INET,
        .rt_flags = RTF_UP | RTF_GATEWAY, .rt_dev = (uintptr_t)eth1};
    net_put_u32((uint8_t *)&route.rt_gateway + 4, 0xc0a80701u);
    assert(!net_interface_ioctl(SIOCADDRT, (uintptr_t)&route));
    assert(!net_ipv4_route_lookup(0x08080808u, 0, 0, &path));
    assert(path.ifindex == 3 && path.source == 0xc0a8070fu && path.next_hop == 0xc0a80701u);
    assert(!net_interface_ioctl(SIOCDELRT, (uintptr_t)&route));
    assert(net_ipv4_route_lookup(0x08080808u, 0, 0, &path) == -LINUX_ENETUNREACH);

    struct {
        struct nlmsghdr header;
        struct ifaddrmsg address;
        struct rtattr local;
        uint32_t ip;
    } change = {.header={.nlmsg_len=sizeof(change),.nlmsg_flags=NLM_F_REPLACE},
                .address={.ifa_family=AF_INET,.ifa_prefixlen=24,.ifa_index=3},
                .local={.rta_len=RTA_LENGTH(4),.rta_type=IFA_LOCAL},
                .ip=__builtin_bswap32(0xc0a8080fu)};
    assert(!nl_change_address(&change.header, false));
    assert(net_interface_get(3)->config.local_ip == 0xc0a8080fu);
    assert(net_interface_get(3)->config.subnet_mask == 0xffffff00u);
    struct {
        struct nlmsghdr header;
        struct rtmsg route;
        struct rtattr destination;
        uint32_t dst;
        struct rtattr gateway;
        uint32_t gw;
        struct rtattr output;
        uint32_t ifindex;
    } route_change = {.header={.nlmsg_len=sizeof(route_change),.nlmsg_flags=NLM_F_CREATE|NLM_F_EXCL},
        .route={.rtm_family=AF_INET,.rtm_dst_len=16,.rtm_table=RT_TABLE_MAIN,.rtm_type=RTN_UNICAST},
        .destination={.rta_len=RTA_LENGTH(4),.rta_type=RTA_DST},.dst=__builtin_bswap32(0xc6330000u),
        .gateway={.rta_len=RTA_LENGTH(4),.rta_type=RTA_GATEWAY},.gw=__builtin_bswap32(0xc0a80801u),
        .output={.rta_len=RTA_LENGTH(4),.rta_type=RTA_OIF},.ifindex=3};
    assert(!nl_change_route(&route_change.header,false));
    assert(!net_ipv4_route_lookup(0xc6336409u,0,0,&path));
    assert(path.ifindex==3 && path.next_hop==0xc0a80801u);

    net_interface_get(2)->config.local_ip = 0;
    assert(!net_ipv4_route_lookup(UINT32_MAX, 0, 2, &path));
    assert(path.ifindex == 2 && path.source == 0);
    struct netlink_socket listener={.groups=RTMGRP_LINK};
    netlink_sockets=&listener;
    assert(!nl_initial_snapshot());
    net_interface_remove(3);
    netlink_ipv6_changed();
    assert(listener.head && ((struct nlmsghdr *)(void *)listener.head->data)->nlmsg_type==RTM_DELLINK);
    struct ifinfomsg *event=(void *)(listener.head->data+sizeof(struct nlmsghdr));
    assert(event->ifi_index==3);
    kernel_free(listener.head); listener.head=listener.tail=NULL;
    nl_forget_snapshot(); netlink_sockets=NULL;
    net_interface_remove(2);
    assert(net_interface_next(1) == 0 && net_interface_index("eth0") == 0);
    puts("PASS multi-interface IPv4 configuration, enumeration, routes, weak-host source and broadcast");
    return 0;
}
