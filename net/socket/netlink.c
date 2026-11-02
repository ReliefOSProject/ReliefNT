/* SPDX-License-Identifier: Apache-2.0 */
/* Route-netlink state is owned by the ordinary kernel execution context. */
#include <reliefnt/netlink.h>
#include <reliefnt/net_ipv6.h>
#include <reliefnt/net.h>
#include <reliefnt/e1000.h>
#include <reliefnt/futex.h>
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/socket_usercopy.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/time.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/wait.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/if.h>
#include <linux/ipv6_route.h>
#include <linux/poll.h>
#include <linux/rtnetlink.h>

/* One complete dump is one datagram; both its size and outstanding bytes are
 * bounded. A linked registry has no artificial process/socket count limit. */
#define NL_MESSAGE_MAX 8192u
#define NL_QUEUE_BUDGET (8u * NL_MESSAGE_MAX)
struct netlink_message { struct netlink_message *next; uint32_t length, groups; uint8_t data[]; };
struct netlink_socket {
    struct netlink_socket *next;
    struct netlink_message *head, *tail;
    uint32_t port, queued_bytes, groups;
    int type;
    bool bound, connected, overrun;
};
struct nl_builder { struct netlink_message *message; uint32_t used, port; };
static struct netlink_socket *netlink_sockets;
static uint32_t netlink_auto_port = 0x80000000u;
static uint64_t netlink_ino;
static struct net_ipv6_address *nl_previous_addresses;
static struct net_ipv6_route *nl_previous_routes;
static uint32_t nl_previous_address_count, nl_previous_route_count;
static struct e1000_info nl_previous_link;
static bool nl_previous_link_up, nl_snapshot_ready;
#define NL_SUPPORTED_GROUPS (RTMGRP_LINK|RTMGRP_IPV6_IFADDR|RTMGRP_IPV6_ROUTE)

/** @brief Resolve the route socket attached to an open description.
 * @param file Borrowed OFD. @return State or NULL for another kind. */
static struct netlink_socket *nl_socket(const struct task_file *file)
{ return file && file->kind == TASK_FILE_KIND_NETLINK ? (void *)(uintptr_t)file->aux : NULL; }

/** @brief Test a nonzero port against every bound endpoint.
 * @param port Proposed ID. @param except Socket excluded from the comparison.
 * @return True when another live route socket already owns it. */
static bool nl_port_used(uint32_t port, const struct netlink_socket *except)
{
    for (struct netlink_socket *s = netlink_sockets; s; s = s->next)
        if (s != except && s->bound && s->port == port) return true;
    return false;
}

/** @brief Assign a unique kernel-chosen port to an unbound OFD.
 * @param s Socket. @param task Current task. @return Zero or ENOSPC. */
static int nl_autobind(struct netlink_socket *s, const struct task *task)
{
    if (s->bound) return 0;
    uint32_t candidate = task ? task->pid : 0;
    if (!candidate || nl_port_used(candidate, s)) {
        uint32_t start = netlink_auto_port;
        do {
            candidate = netlink_auto_port++;
            if (!netlink_auto_port) netlink_auto_port = 0x80000000u;
            if (candidate && !nl_port_used(candidate, s)) break;
        } while (netlink_auto_port != start);
        if (!candidate || nl_port_used(candidate, s)) return -LINUX_ENOSPC;
    }
    s->port = candidate;
    s->bound = true;
    return 0;
}

/** @brief Begin an aligned reply in the bounded staging datagram.
 * @param b Builder. @param type Message type. @param flags Netlink flags.
 * @param seq Request sequence. @param body Body length. @return Writable body or NULL. */
static void *nl_begin(struct nl_builder *b, uint16_t type, uint16_t flags,
                      uint32_t seq, uint32_t body)
{
    uint32_t size = NLMSG_ALIGN(sizeof(struct nlmsghdr) + body);
    if (size > NL_MESSAGE_MAX - b->used) return NULL;
    struct nlmsghdr *header = (void *)(b->message->data + b->used);
    __builtin_memset(header, 0, size);
    *header = (struct nlmsghdr){.nlmsg_len = sizeof(*header) + body,
        .nlmsg_type = type, .nlmsg_flags = flags, .nlmsg_seq = seq, .nlmsg_pid = b->port};
    b->used += size;
    return header + 1;
}

/** @brief Append one aligned attribute to the last reply.
 * @param b Builder. @param start Start offset of its message. @param type Attribute ID.
 * @param data Borrowed bytes. @param length Value length. @return Zero or ENOBUFS. */
static int nl_attr(struct nl_builder *b, uint32_t start, uint16_t type,
                   const void *data, uint32_t length)
{
    uint32_t size = RTA_ALIGN(sizeof(struct rtattr) + length);
    if (size > NL_MESSAGE_MAX - b->used || length > UINT16_MAX - sizeof(struct rtattr))
        return -LINUX_ENOBUFS;
    struct rtattr *a = (void *)(b->message->data + b->used);
    __builtin_memset(a, 0, size);
    a->rta_len = (uint16_t)(sizeof(*a) + length);
    a->rta_type = type;
    if (length) __builtin_memcpy(a + 1, data, length);
    b->used += size;
    ((struct nlmsghdr *)(void *)(b->message->data + start))->nlmsg_len = b->used - start;
    return 0;
}

/** @brief Emit success or failure with the original request header.
 * @param b Builder. @param request Checked original request. @param error Native negative errno.
 * @return Zero or ENOBUFS. Error replies include the original request payload. */
static int nl_error(struct nl_builder *b, const struct nlmsghdr *request, int error)
{
    uint32_t extra = error ? request->nlmsg_len - sizeof(*request) : 0;
    struct nlmsgerr *reply = nl_begin(b, NLMSG_ERROR, 0, request->nlmsg_seq, sizeof(*reply) + extra);
    if (!reply) return -LINUX_ENOBUFS;
    reply->error = error;
    reply->msg = *request;
    if (extra) __builtin_memcpy(reply + 1, (const uint8_t *)request + sizeof(*request), extra);
    return 0;
}

/** @brief Terminate a multipart dump with its native status word.
 * @param b Builder. @param seq Request sequence. @return Zero or ENOBUFS. */
static int nl_done(struct nl_builder *b, uint32_t seq)
{
    int32_t *status = nl_begin(b, NLMSG_DONE, NLM_F_MULTI, seq, sizeof(*status));
    if (!status) return -LINUX_ENOBUFS;
    *status = 0;
    return 0;
}

/** @brief Convert a lifetime deadline into Linux seconds remaining.
 * @param until Absolute milliseconds or UINT64_MAX. @return Remaining seconds. */
static uint32_t nl_lifetime(uint64_t until)
{
    if (until == UINT64_MAX) return UINT32_MAX;
    uint64_t now = time_uptime_ms();
    if (until <= now) return 0;
    uint64_t remaining = until - now;
    uint64_t seconds = remaining / 1000u + !!(remaining % 1000u);
    return seconds >= UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)seconds;
}

/** @brief Convert Linux lifetime seconds to an absolute deadline.
 * @param seconds Lifetime or UINT32_MAX for infinity. @return Deadline in milliseconds. */
static uint64_t nl_deadline(uint32_t seconds)
{
    if (seconds == UINT32_MAX) return UINT64_MAX;
    uint64_t now = time_uptime_ms(), delta = (uint64_t)seconds * 1000u;
    return now >= UINT64_MAX - delta ? UINT64_MAX - 1u : now + delta;
}

/** @brief Count contiguous high bits of an IPv4 host-order mask.
 * @param mask IPv4 mask. @return Prefix length. */
static uint8_t nl_prefix4(uint32_t mask)
{
    uint8_t count = 0;
    while (mask & 0x80000000u) { ++count; mask <<= 1; }
    return count;
}

/** @brief Emit one interface from a copied link state.
 * @param b Builder. @param seq Sequence. @param index Interface index.
 * @param multi Multipart flag. @param type Native event. @param info Ethernet snapshot.
 * @param up Administrative and device readiness. @return Zero or ENOBUFS. */
static int nl_link_snapshot(struct nl_builder *b, uint32_t seq, int index, bool multi,
                            uint16_t type, const struct e1000_info *info, bool up)
{
    uint32_t start = b->used;
    struct ifinfomsg *m = nl_begin(b, type, multi ? NLM_F_MULTI : 0, seq, sizeof(*m));
    if (!m) return -LINUX_ENOBUFS;
    m->ifi_index = index;
    m->ifi_type = index == 1 ? 772 : 1; /* ARPHRD_LOOPBACK / ARPHRD_ETHER */
    m->ifi_flags = index == 1 ? IFF_UP | IFF_LOOPBACK | IFF_RUNNING | 0x10000u :
        IFF_BROADCAST | IFF_MULTICAST | (up ? IFF_UP | IFF_RUNNING : 0) |
        (info->active ? 0x10000u : 0);
    m->ifi_change = UINT32_MAX;
    uint32_t mtu = index == 1 ? 65536 : 1500;
    uint8_t zero[6] = {0}, broadcast[6] = {255,255,255,255,255,255};
    const char *name = index == 1 ? "lo" : "eth0";
    if (nl_attr(b,start,IFLA_IFNAME,name,index == 1 ? 3 : 5) ||
        nl_attr(b,start,IFLA_MTU,&mtu,sizeof(mtu)) ||
        nl_attr(b,start,IFLA_ADDRESS,index == 1 ? zero : info->mac,6) ||
        nl_attr(b,start,IFLA_BROADCAST,index == 1 ? zero : broadcast,6)) return -LINUX_ENOBUFS;
    return 0;
}

/** @brief Emit a live interface with current hardware state.
 * @param b Builder. @param seq Sequence. @param index Interface index.
 * @param multi Multipart flag. @return Zero or errno. */
static int nl_link(struct nl_builder *b, uint32_t seq, int index, bool multi)
{
    struct e1000_info info={0};
    if (index==2) { e1000_get_info(&info); if (!info.present) return -LINUX_ENODEV; }
    return nl_link_snapshot(b,seq,index,multi,RTM_NEWLINK,&info,net_interface_ready());
}

/** @brief Emit one IPv4 address using its actual configured mask.
 * @param b Builder. @param seq Sequence. @param index Interface. @param address Host-order IP.
 * @param prefix Prefix length. @param broadcast Host-order broadcast. @return Zero or errno. */
static int nl_address4(struct nl_builder *b, uint32_t seq, uint32_t index,
                       uint32_t address, uint8_t prefix, uint32_t broadcast)
{
    uint32_t start = b->used, ip = __builtin_bswap32(address), br = __builtin_bswap32(broadcast);
    struct ifaddrmsg *m = nl_begin(b, RTM_NEWADDR, NLM_F_MULTI, seq, sizeof(*m));
    if (!m) return -LINUX_ENOBUFS;
    *m = (struct ifaddrmsg){.ifa_family=AF_INET,.ifa_prefixlen=prefix,.ifa_flags=IFA_F_PERMANENT,
        .ifa_scope=index==1 ? RT_SCOPE_HOST : RT_SCOPE_UNIVERSE,.ifa_index=index};
    const char *name = index == 1 ? "lo" : "eth0";
    if (nl_attr(b,start,IFA_ADDRESS,&ip,4) || nl_attr(b,start,IFA_LOCAL,&ip,4) ||
        nl_attr(b,start,IFA_LABEL,name,index==1 ? 3 : 5) ||
        (index==2 && nl_attr(b,start,IFA_BROADCAST,&br,4))) return -LINUX_ENOBUFS;
    return 0;
}

/** @brief Emit one copied IPv6 address including flags and remaining lifetimes.
 * @param b Builder. @param seq Request sequence. @param a Snapshot. @param type Event type.
 * @param multi Multipart flag. @return Zero or errno. */
static int nl_address6(struct nl_builder *b, uint32_t seq, const struct net_ipv6_address *a,
                       uint16_t type, bool multi)
{
    uint32_t start = b->used;
    struct ifaddrmsg *m = nl_begin(b, type, multi ? NLM_F_MULTI : 0, seq, sizeof(*m));
    if (!m) return -LINUX_ENOBUFS;
    *m = (struct ifaddrmsg){.ifa_family=AF_INET6,.ifa_prefixlen=(uint8_t)a->prefix_length,
        .ifa_flags=(uint8_t)a->flags,.ifa_scope=(uint8_t)a->scope,.ifa_index=a->ifindex};
    struct ifa_cacheinfo cache = {.ifa_prefered=nl_lifetime(a->preferred_until),
                                   .ifa_valid=nl_lifetime(a->valid_until)};
    if (nl_attr(b,start,IFA_ADDRESS,&a->address,16) ||
        nl_attr(b,start,IFA_FLAGS,&a->flags,4) || nl_attr(b,start,IFA_CACHEINFO,&cache,sizeof(cache)))
        return -LINUX_ENOBUFS;
    return 0;
}

/** @brief Emit one IPv6 route with its destination, interface and gateway.
 * @param b Builder. @param seq Sequence. @param route Snapshot. @param type Event type.
 * @param multi Multipart flag. @return Zero or errno. */
static int nl_route6(struct nl_builder *b, uint32_t seq, const struct net_ipv6_route *route,
                     uint16_t type, bool multi)
{
    uint32_t start = b->used;
    struct rtmsg *m = nl_begin(b, type, multi ? NLM_F_MULTI : 0, seq, sizeof(*m));
    if (!m) return -LINUX_ENOBUFS;
    static const struct in6_addr zero;
    bool gateway = __builtin_memcmp(&route->gateway,&zero,sizeof(zero)) != 0;
    *m = (struct rtmsg){.rtm_family=AF_INET6,.rtm_dst_len=(uint8_t)route->prefix_length,
        .rtm_table=RT_TABLE_MAIN,.rtm_protocol=(route->flags & RTF_ADDRCONF) ? RTPROT_RA : RTPROT_BOOT,
        .rtm_scope=gateway ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK,.rtm_type=RTN_UNICAST};
    if ((route->prefix_length && nl_attr(b,start,RTA_DST,&route->destination,16)) ||
        nl_attr(b,start,RTA_OIF,&route->ifindex,4) ||
        (gateway && nl_attr(b,start,RTA_GATEWAY,&route->gateway,16)) ||
        nl_attr(b,start,RTA_PRIORITY,&route->metric,4)) return -LINUX_ENOBUFS;
    return 0;
}

/** @brief Emit an IPv4 route from existing core configuration.
 * @param b Builder. @param seq Sequence. @param destination Host-order network.
 * @param prefix Prefix length. @param gateway Host-order gateway. @param index Interface.
 * @return Zero or ENOBUFS. */
static int nl_route4(struct nl_builder *b, uint32_t seq, uint32_t destination,
                     uint8_t prefix, uint32_t gateway, uint32_t index)
{
    uint32_t start=b->used, dst=__builtin_bswap32(destination), gw=__builtin_bswap32(gateway);
    struct rtmsg *m=nl_begin(b,RTM_NEWROUTE,NLM_F_MULTI,seq,sizeof(*m));
    if (!m) return -LINUX_ENOBUFS;
    *m=(struct rtmsg){.rtm_family=AF_INET,.rtm_dst_len=prefix,.rtm_table=RT_TABLE_MAIN,
        .rtm_protocol=RTPROT_KERNEL,.rtm_scope=gateway ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK,
        .rtm_type=RTN_UNICAST};
    if ((prefix && nl_attr(b,start,RTA_DST,&dst,4)) || nl_attr(b,start,RTA_OIF,&index,4) ||
        (gateway && nl_attr(b,start,RTA_GATEWAY,&gw,4))) return -LINUX_ENOBUFS;
    return 0;
}

/** @brief Copy the current address snapshot without retaining protocol pointers.
 * @param out Receives owned array or NULL. @param count Receives element count.
 * @return Zero or allocation/budget error. */
static int nl_addresses(struct net_ipv6_address **out, uint32_t *count)
{
    *out = NULL; *count = net_ipv6_addresses(NULL,0);
    if (!*count) return 0;
    if (*count > NL_MESSAGE_MAX / sizeof(**out)) return -LINUX_ENOBUFS;
    *out = kernel_malloc((size_t)*count * sizeof(**out));
    if (!*out) return -LINUX_ENOMEM;
    if (net_ipv6_addresses(*out,*count) > *count) { kernel_free(*out); *out=NULL; return -LINUX_EAGAIN; }
    return 0;
}

/** @brief Copy the current route snapshot without retaining protocol pointers.
 * @param out Receives owned array or NULL. @param count Receives element count.
 * @return Zero or allocation/budget error. */
static int nl_routes(struct net_ipv6_route **out, uint32_t *count)
{
    *out = NULL; *count = net_ipv6_routes(NULL,0);
    if (!*count) return 0;
    if (*count > NL_MESSAGE_MAX / sizeof(**out)) return -LINUX_ENOBUFS;
    *out = kernel_malloc((size_t)*count * sizeof(**out));
    if (!*out) return -LINUX_ENOMEM;
    if (net_ipv6_routes(*out,*count) > *count) { kernel_free(*out); *out=NULL; return -LINUX_EAGAIN; }
    return 0;
}

/** @brief Drop the owned change-detection baseline after the last subscriber leaves. */
static void nl_forget_snapshot(void)
{
    kernel_free(nl_previous_addresses); kernel_free(nl_previous_routes);
    nl_previous_addresses=NULL; nl_previous_routes=NULL;
    nl_previous_address_count=nl_previous_route_count=0; nl_snapshot_ready=false;
}

/** @brief Capture a baseline before admitting the first subscription.
 * @return Zero or allocation error; no partial snapshot survives failure. */
static int nl_initial_snapshot(void)
{
    if (nl_snapshot_ready) return 0;
    struct net_ipv6_address *addresses; struct net_ipv6_route *routes;
    uint32_t acount, rcount;
    int rc=nl_addresses(&addresses,&acount); if (rc) return rc;
    rc=nl_routes(&routes,&rcount); if (rc) { kernel_free(addresses); return rc; }
    nl_previous_addresses=addresses; nl_previous_address_count=acount;
    nl_previous_routes=routes; nl_previous_route_count=rcount;
    e1000_get_info(&nl_previous_link); nl_previous_link_up=net_interface_ready();
    nl_snapshot_ready=true; return 0;
}

/** @brief Change the socket's supported multicast membership transactionally.
 * @param s Subscriber. @param groups New Linux group mask. @return Zero or errno. */
static int nl_set_groups(struct netlink_socket *s, uint32_t groups)
{
    if (groups & ~NL_SUPPORTED_GROUPS) return -LINUX_EOPNOTSUPP;
    if (groups) { int rc=nl_initial_snapshot(); if (rc) return rc; }
    s->groups=groups;
    if (!groups) {
        for (struct netlink_socket *it=netlink_sockets;it;it=it->next)
            if (it->groups) return 0;
        nl_forget_snapshot();
    }
    return 0;
}

/** @brief Report notification loss to every interested subscriber.
 * @param group Linux multicast group bit. */
static void nl_overrun(uint32_t group)
{
    for (struct netlink_socket *s=netlink_sockets;s;s=s->next)
        if (s->groups & group) s->overrun=true;
    kernel_poll_readiness_notify();
}

/** @brief Compare address identity independently of its mutable flags/lifetime.
 * @param a First snapshot. @param b Second snapshot. @return True for the same address. */
static bool nl_same_address(const struct net_ipv6_address *a,const struct net_ipv6_address *b)
{ return a->ifindex==b->ifindex && !__builtin_memcmp(&a->address,&b->address,16); }

/** @brief Compare all exposed state of the same address.
 * @param a First snapshot. @param b Second snapshot. @return True if unchanged. */
static bool nl_equal_address(const struct net_ipv6_address *a,const struct net_ipv6_address *b)
{
    return nl_same_address(a,b) && a->prefix_length==b->prefix_length && a->flags==b->flags &&
        a->scope==b->scope && a->preferred_until==b->preferred_until && a->valid_until==b->valid_until;
}

/** @brief Compare a route's table identity.
 * @param a First snapshot. @param b Second snapshot. @return True for the same route. */
static bool nl_same_route(const struct net_ipv6_route *a,const struct net_ipv6_route *b)
{
    return a->ifindex==b->ifindex && a->prefix_length==b->prefix_length &&
        !__builtin_memcmp(&a->destination,&b->destination,16) &&
        !__builtin_memcmp(&a->gateway,&b->gateway,16);
}

/** @brief Compare all exposed state of the same route.
 * @param a First snapshot. @param b Second snapshot. @return True if unchanged. */
static bool nl_equal_route(const struct net_ipv6_route *a,const struct net_ipv6_route *b)
{ return nl_same_route(a,b) && a->metric==b->metric && a->flags==b->flags && a->valid_until==b->valid_until; }

/** @brief Queue a bounded native event for every subscribed OFD.
 * @param group Linux group bit. @param type Message type. @param value Copied address,
 * route or Ethernet snapshot according to group. @param up Link readiness for link events. */
static void nl_publish(uint32_t group,uint16_t type,const void *value,bool up)
{
    bool interested=false;
    for (struct netlink_socket *s=netlink_sockets;s;s=s->next) if (s->groups & group) { interested=true; break; }
    if (!interested) return;
    struct netlink_message *message=kernel_malloc(sizeof(*message)+NL_MESSAGE_MAX);
    if (!message) { nl_overrun(group); return; }
    struct nl_builder b={.message=message};
    int rc=group==RTMGRP_IPV6_IFADDR ? nl_address6(&b,0,value,type,false) :
           group==RTMGRP_IPV6_ROUTE ? nl_route6(&b,0,value,type,false) :
           nl_link_snapshot(&b,0,2,false,type,value,up);
    if (rc) { kernel_free(message); nl_overrun(group); return; }
    for (struct netlink_socket *s=netlink_sockets;s;s=s->next) {
        if (!(s->groups & group)) continue;
        if (s->queued_bytes>NL_QUEUE_BUDGET-NL_MESSAGE_MAX) { s->overrun=true; continue; }
        struct netlink_message *copy=kernel_malloc(sizeof(*copy)+NL_MESSAGE_MAX);
        if (!copy) { s->overrun=true; continue; }
        copy->next=NULL; copy->length=b.used; copy->groups=group;
        __builtin_memcpy(copy->data,message->data,b.used);
        if (s->tail) s->tail->next=copy; else s->head=copy;
        s->tail=copy; s->queued_bytes+=NL_MESSAGE_MAX;
    }
    kernel_free(message); kernel_poll_readiness_notify();
}

/** @brief Diff owned address, route and link snapshots into multicast events.
 * Changes are detected under the existing execution owner; allocation failure
 * is surfaced as ENOBUFS to subscribers and the previous baseline is retained. */
void netlink_ipv6_changed(void)
{
    if (!nl_snapshot_ready) return;
    struct net_ipv6_address *addresses; struct net_ipv6_route *routes;
    uint32_t acount, rcount;
    int rc=nl_addresses(&addresses,&acount);
    if (rc) { nl_overrun(NL_SUPPORTED_GROUPS); return; }
    rc=nl_routes(&routes,&rcount);
    if (rc) { kernel_free(addresses); nl_overrun(NL_SUPPORTED_GROUPS); return; }
    for (uint32_t i=0;i<nl_previous_address_count;++i) {
        bool found=false;
        for (uint32_t j=0;j<acount;++j) if (nl_same_address(&nl_previous_addresses[i],&addresses[j])) { found=true; break; }
        if (!found) nl_publish(RTMGRP_IPV6_IFADDR,RTM_DELADDR,&nl_previous_addresses[i],false);
    }
    for (uint32_t i=0;i<acount;++i) {
        bool same=false;
        for (uint32_t j=0;j<nl_previous_address_count;++j)
            if (nl_equal_address(&addresses[i],&nl_previous_addresses[j])) { same=true; break; }
        if (!same) nl_publish(RTMGRP_IPV6_IFADDR,RTM_NEWADDR,&addresses[i],false);
    }
    for (uint32_t i=0;i<nl_previous_route_count;++i) {
        bool found=false;
        for (uint32_t j=0;j<rcount;++j) if (nl_same_route(&nl_previous_routes[i],&routes[j])) { found=true; break; }
        if (!found) nl_publish(RTMGRP_IPV6_ROUTE,RTM_DELROUTE,&nl_previous_routes[i],false);
    }
    for (uint32_t i=0;i<rcount;++i) {
        bool same=false;
        for (uint32_t j=0;j<nl_previous_route_count;++j)
            if (nl_equal_route(&routes[i],&nl_previous_routes[j])) { same=true; break; }
        if (!same) nl_publish(RTMGRP_IPV6_ROUTE,RTM_NEWROUTE,&routes[i],false);
    }
    struct e1000_info link={0}; e1000_get_info(&link); bool up=net_interface_ready();
    if (link.present!=nl_previous_link.present || link.active!=nl_previous_link.active ||
        up!=nl_previous_link_up || __builtin_memcmp(link.mac,nl_previous_link.mac,6)) {
        if (link.present) nl_publish(RTMGRP_LINK,RTM_NEWLINK,&link,up);
        else if (nl_previous_link.present) nl_publish(RTMGRP_LINK,RTM_DELLINK,&nl_previous_link,nl_previous_link_up);
    }
    kernel_free(nl_previous_addresses); kernel_free(nl_previous_routes);
    nl_previous_addresses=addresses; nl_previous_address_count=acount;
    nl_previous_routes=routes; nl_previous_route_count=rcount;
    nl_previous_link=link; nl_previous_link_up=up;
}

/** @brief Render a link, address or route dump from current network state.
 * @param b Builder. @param h Checked request. @return Zero or negative errno. */
static int nl_dump(struct nl_builder *b, const struct nlmsghdr *h)
{
    uint32_t payload = h->nlmsg_len - sizeof(*h);
    bool multi = (h->nlmsg_flags & NLM_F_DUMP) == NLM_F_DUMP;
    if (h->nlmsg_type == RTM_GETLINK) {
        if (!payload || (!multi && payload < sizeof(struct ifinfomsg))) return -LINUX_EINVAL;
        struct ifinfomsg req={0};
        __builtin_memcpy(&req,h+1,payload < sizeof(req) ? payload : sizeof(req));
        if (req.ifi_family != AF_UNSPEC && req.ifi_family != AF_PACKET &&
            req.ifi_family != AF_INET && req.ifi_family != AF_INET6) return -LINUX_EAFNOSUPPORT;
        if (!multi) {
            if (req.ifi_index != 1 && req.ifi_index != 2) return -LINUX_ENODEV;
            return nl_link(b,h->nlmsg_seq,req.ifi_index,false);
        }
        int rc=nl_link(b,h->nlmsg_seq,1,true); if (rc) return rc;
        if (e1000_is_ready()) { rc=nl_link(b,h->nlmsg_seq,2,true); if (rc) return rc; }
        return nl_done(b,h->nlmsg_seq);
    }
    if (!multi) return -LINUX_EOPNOTSUPP;
    if (h->nlmsg_type == RTM_GETADDR) {
        if (!payload) return -LINUX_EINVAL;
        struct ifaddrmsg req={0};
        __builtin_memcpy(&req,h+1,payload < sizeof(req) ? payload : sizeof(req));
        if (req.ifa_family != AF_UNSPEC && req.ifa_family != AF_INET && req.ifa_family != AF_INET6)
            return -LINUX_EAFNOSUPPORT;
        int rc=0;
        if (req.ifa_family != AF_INET6) {
            if ((!req.ifa_index || req.ifa_index==1) &&
                (rc=nl_address4(b,h->nlmsg_seq,1,0x7f000001u,8,0))) return rc;
            struct reliefos_net_config config; net_get_config(&config);
            if ((!req.ifa_index || req.ifa_index==2) && e1000_is_ready() && config.local_ip &&
                (rc=nl_address4(b,h->nlmsg_seq,2,config.local_ip,nl_prefix4(config.subnet_mask),
                                config.local_ip | ~config.subnet_mask))) return rc;
        }
        if (req.ifa_family != AF_INET) {
            struct net_ipv6_address *entries; uint32_t count;
            rc=nl_addresses(&entries,&count); if (rc) return rc;
            for (uint32_t i=0; i<count && !rc; ++i)
                if (!req.ifa_index || req.ifa_index==entries[i].ifindex)
                    rc=nl_address6(b,h->nlmsg_seq,&entries[i],RTM_NEWADDR,true);
            kernel_free(entries); if (rc) return rc;
        }
        return nl_done(b,h->nlmsg_seq);
    }
    if (!payload) return -LINUX_EINVAL;
    struct rtmsg req={0};
    __builtin_memcpy(&req,h+1,payload < sizeof(req) ? payload : sizeof(req));
    if (req.rtm_family != AF_UNSPEC && req.rtm_family != AF_INET && req.rtm_family != AF_INET6)
        return -LINUX_EAFNOSUPPORT;
    int rc=0;
    if (req.rtm_family != AF_INET6) {
        rc=nl_route4(b,h->nlmsg_seq,0x7f000000u,8,0,1); if (rc) return rc;
        struct reliefos_net_config config; net_get_config(&config);
        if (net_interface_ready() && config.local_ip) {
            rc=nl_route4(b,h->nlmsg_seq,config.local_ip & config.subnet_mask,
                         nl_prefix4(config.subnet_mask),0,2); if (rc) return rc;
            if (config.gateway_ip) { rc=nl_route4(b,h->nlmsg_seq,0,0,config.gateway_ip,2); if (rc) return rc; }
        }
    }
    if (req.rtm_family != AF_INET) {
        struct net_ipv6_route *entries; uint32_t count;
        rc=nl_routes(&entries,&count); if (rc) return rc;
        for (uint32_t i=0; i<count && !rc; ++i) rc=nl_route6(b,h->nlmsg_seq,&entries[i],RTM_NEWROUTE,true);
        kernel_free(entries); if (rc) return rc;
    }
    return nl_done(b,h->nlmsg_seq);
}

/** @brief Decode a fully bounded attribute sequence and reject duplicates.
 * @param data Attribute bytes. @param length Available bytes. @param attrs Output pointer table.
 * @param slots Table capacity. @return Zero or EINVAL for malformed/duplicate attributes. */
static int nl_parse_attrs(const uint8_t *data, uint32_t length,
                          const struct rtattr **attrs, uint32_t slots)
{
    while (length) {
        if (length < sizeof(struct rtattr)) return -LINUX_EINVAL;
        const struct rtattr *a=(const void *)data;
        if (a->rta_len < sizeof(*a) || a->rta_len > length) return -LINUX_EINVAL;
        uint32_t advance=RTA_ALIGN(a->rta_len);
        if (advance > length) return -LINUX_EINVAL;
        if (a->rta_type >= slots) return -LINUX_EOPNOTSUPP;
        if (attrs[a->rta_type]) return -LINUX_EINVAL;
        attrs[a->rta_type]=a;
        data+=advance; length-=advance;
    }
    return 0;
}

/** @brief Validate one optional fixed-size attribute and copy its value.
 * @param a Nullable attribute. @param out Writable destination. @param size Expected bytes.
 * @return Zero, or EINVAL for a size mismatch. */
static int nl_copy_attr(const struct rtattr *a, void *out, uint32_t size)
{
    if (!a) return 0;
    if (a->rta_len != sizeof(*a)+size) return -LINUX_EINVAL;
    __builtin_memcpy(out,a+1,size); return 0;
}

/** @brief Apply an authorized IPv6 address request using copied snapshots.
 * @param h Checked request. @param remove True for delete. @return Zero or errno. */
static int nl_change_address(const struct nlmsghdr *h, bool remove)
{
    if (h->nlmsg_len < sizeof(*h)+sizeof(struct ifaddrmsg)) return -LINUX_EINVAL;
    struct ifaddrmsg req; __builtin_memcpy(&req,h+1,sizeof(req));
    if (req.ifa_family != AF_INET6) return -LINUX_EAFNOSUPPORT;
    if (!req.ifa_index || req.ifa_prefixlen > 128) return -LINUX_EINVAL;
    const struct rtattr *a[9]={0};
    int rc=nl_parse_attrs((const uint8_t *)(h+1)+sizeof(req),
                          h->nlmsg_len-sizeof(*h)-sizeof(req),a,9); if (rc) return rc;
    for (unsigned i=0;i<9;++i)
        if (a[i] && i!=IFA_LOCAL && i!=IFA_ADDRESS && i!=IFA_FLAGS && i!=IFA_CACHEINFO && i!=IFA_LABEL)
            return -LINUX_EOPNOTSUPP;
    if (!a[IFA_LOCAL] && !a[IFA_ADDRESS]) return -LINUX_EINVAL;
    struct net_ipv6_address entry={.ifindex=req.ifa_index,.prefix_length=req.ifa_prefixlen,
        .scope=req.ifa_scope,.flags=req.ifa_flags,.preferred_until=UINT64_MAX,.valid_until=UINT64_MAX};
    rc=nl_copy_attr(a[IFA_LOCAL] ? a[IFA_LOCAL] : a[IFA_ADDRESS],&entry.address,16); if (rc) return rc;
    if (a[IFA_LOCAL] && a[IFA_ADDRESS]) {
        struct in6_addr peer; rc=nl_copy_attr(a[IFA_ADDRESS],&peer,16); if (rc) return rc;
        if (__builtin_memcmp(&entry.address,&peer,16)) return -LINUX_EOPNOTSUPP;
    }
    if (a[IFA_FLAGS]) { rc=nl_copy_attr(a[IFA_FLAGS],&entry.flags,4); if (rc) return rc; }
    if (entry.flags & ~(NET_IPV6_ADDR_NODAD|NET_IPV6_ADDR_PERMANENT)) return -LINUX_EOPNOTSUPP;
    if (a[IFA_LABEL]) {
        const char *label=(const char *)(a[IFA_LABEL]+1);
        uint32_t n=a[IFA_LABEL]->rta_len-sizeof(struct rtattr);
        const char *expected=entry.ifindex==1 ? "lo" : entry.ifindex==2 ? "eth0" : "";
        uint32_t expected_n=entry.ifindex==1 ? 3 : entry.ifindex==2 ? 5 : 1;
        if (n!=expected_n || __builtin_memcmp(label,expected,n)) return -LINUX_EOPNOTSUPP;
    }
    if (a[IFA_CACHEINFO]) {
        struct ifa_cacheinfo cache;
        rc=nl_copy_attr(a[IFA_CACHEINFO],&cache,sizeof(cache)); if (rc) return rc;
        if (!cache.ifa_valid || cache.ifa_prefered > cache.ifa_valid) return -LINUX_EINVAL;
        entry.preferred_until=nl_deadline(cache.ifa_prefered);
        entry.valid_until=nl_deadline(cache.ifa_valid);
    }
    if (entry.valid_until==UINT64_MAX) entry.flags|=NET_IPV6_ADDR_PERMANENT;
    struct net_ipv6_address *existing; uint32_t count;
    rc=nl_addresses(&existing,&count); if (rc) return rc;
    bool found=false;
    for (uint32_t i=0;i<count;++i)
        if (existing[i].ifindex==entry.ifindex && !__builtin_memcmp(&existing[i].address,&entry.address,16))
            { found=true; break; }
    kernel_free(existing);
    if (!remove && found && (h->nlmsg_flags & NLM_F_EXCL)) return -LINUX_EEXIST;
    if (!remove && !found && (h->nlmsg_flags & NLM_F_REPLACE) && !(h->nlmsg_flags & NLM_F_CREATE))
        return -LINUX_EADDRNOTAVAIL;
    if (remove && !found) return -LINUX_EADDRNOTAVAIL;
    return net_ipv6_address_update(&entry,remove);
}

/** @brief Apply an authorized IPv6 route request with checked native attributes.
 * @param h Checked request. @param remove True for delete. @return Zero or errno. */
static int nl_change_route(const struct nlmsghdr *h, bool remove)
{
    if (h->nlmsg_len < sizeof(*h)+sizeof(struct rtmsg)) return -LINUX_EINVAL;
    struct rtmsg req; __builtin_memcpy(&req,h+1,sizeof(req));
    if (req.rtm_family != AF_INET6) return -LINUX_EAFNOSUPPORT;
    if (req.rtm_dst_len > 128 || req.rtm_src_len || req.rtm_tos || req.rtm_flags ||
        (req.rtm_table && req.rtm_table!=RT_TABLE_MAIN) ||
        (req.rtm_type && req.rtm_type!=RTN_UNICAST)) return -LINUX_EOPNOTSUPP;
    const struct rtattr *a[16]={0};
    int rc=nl_parse_attrs((const uint8_t *)(h+1)+sizeof(req),
                          h->nlmsg_len-sizeof(*h)-sizeof(req),a,16); if (rc) return rc;
    for (unsigned i=0;i<16;++i)
        if (a[i] && i!=RTA_DST && i!=RTA_GATEWAY && i!=RTA_OIF && i!=RTA_PRIORITY && i!=RTA_TABLE)
            return -LINUX_EOPNOTSUPP;
    struct net_ipv6_route route={.prefix_length=req.rtm_dst_len,.metric=1024,.flags=RTF_UP,.valid_until=UINT64_MAX};
    if (req.rtm_dst_len && !a[RTA_DST]) return -LINUX_EINVAL;
    if ((rc=nl_copy_attr(a[RTA_DST],&route.destination,16)) ||
        (rc=nl_copy_attr(a[RTA_GATEWAY],&route.gateway,16)) ||
        (rc=nl_copy_attr(a[RTA_OIF],&route.ifindex,4)) ||
        (rc=nl_copy_attr(a[RTA_PRIORITY],&route.metric,4))) return rc;
    if (!route.ifindex) return -LINUX_EINVAL;
    if (a[RTA_TABLE]) { uint32_t table; rc=nl_copy_attr(a[RTA_TABLE],&table,4); if (rc) return rc;
        if (table!=RT_TABLE_MAIN) return -LINUX_EOPNOTSUPP; }
    if (a[RTA_GATEWAY]) route.flags|=RTF_GATEWAY;
    if (route.prefix_length==128) route.flags|=RTF_HOST;
    /* Canonicalize host bits exactly as the protocol route table does. */
    for (uint32_t bit=route.prefix_length;bit<128;++bit)
        route.destination.s6_addr[bit/8] &= (uint8_t)~(0x80u >> (bit%8));
    struct net_ipv6_route *existing; uint32_t count;
    rc=nl_routes(&existing,&count); if (rc) return rc;
    bool found=false;
    for (uint32_t i=0;i<count;++i)
        if (existing[i].ifindex==route.ifindex && existing[i].prefix_length==route.prefix_length &&
            !__builtin_memcmp(&existing[i].destination,&route.destination,16) &&
            !__builtin_memcmp(&existing[i].gateway,&route.gateway,16)) { found=true; break; }
    kernel_free(existing);
    if (!remove && found && (h->nlmsg_flags & NLM_F_EXCL)) return -LINUX_EEXIST;
    if (!remove && !found && (h->nlmsg_flags & NLM_F_REPLACE) && !(h->nlmsg_flags & NLM_F_CREATE))
        return -LINUX_ESRCH;
    if (remove && !found) return -LINUX_ESRCH;
    return net_ipv6_route_update(&route,remove);
}

/** @brief Process one checked request and append its response or error.
 * @param b Output builder. @param h Complete native request. @return Zero or staging error. */
static int nl_request(struct nl_builder *b, const struct nlmsghdr *h)
{
    uint32_t original=b->used;
    int rc;
    if (!(h->nlmsg_flags & NLM_F_REQUEST)) rc=-LINUX_EINVAL;
    else if (h->nlmsg_type==RTM_GETLINK || h->nlmsg_type==RTM_GETADDR || h->nlmsg_type==RTM_GETROUTE)
        rc=nl_dump(b,h);
    else if (h->nlmsg_type==RTM_NEWADDR || h->nlmsg_type==RTM_DELADDR ||
             h->nlmsg_type==RTM_NEWROUTE || h->nlmsg_type==RTM_DELROUTE) {
        struct task *task=sched_current_task();
        if (!task || !(task->cap_effective & (1ULL<<CAP_NET_ADMIN))) rc=-LINUX_EPERM;
        else if (h->nlmsg_flags & (NLM_F_ECHO|NLM_F_APPEND)) rc=-LINUX_EOPNOTSUPP;
        else if (h->nlmsg_type==RTM_NEWADDR || h->nlmsg_type==RTM_DELADDR)
            rc=nl_change_address(h,h->nlmsg_type==RTM_DELADDR);
        else rc=nl_change_route(h,h->nlmsg_type==RTM_DELROUTE);
    } else rc=-LINUX_EOPNOTSUPP;
    if (rc) { b->used=original; return nl_error(b,h,rc); }
    bool dump=(h->nlmsg_type==RTM_GETLINK || h->nlmsg_type==RTM_GETADDR || h->nlmsg_type==RTM_GETROUTE) &&
              (h->nlmsg_flags & NLM_F_DUMP)==NLM_F_DUMP;
    if ((h->nlmsg_flags & NLM_F_ACK) && !dump) return nl_error(b,h,0);
    return 0;
}

/** @brief Process one complete kernel-bound datagram and queue bounded replies.
 * @param file Pinned OFD. @param data Validated bytes. @param length Input length.
 * @param flags Linux send flags. @param address Optional user destination.
 * @param address_length Address length. @return Bytes accepted or errno. */
int task_netlink_send(struct task_file *file,const void *data,uint32_t length,uint32_t flags,
                      uint64_t address,uint32_t address_length)
{
    struct netlink_socket *s=nl_socket(file);
    if (!s) return -LINUX_EBADF;
    if (flags & ~(MSG_DONTWAIT|MSG_NOSIGNAL)) return -LINUX_EOPNOTSUPP;
    if (!length || length > NL_MESSAGE_MAX) return -LINUX_EMSGSIZE;
    if (address) {
        if (address_length < sizeof(struct sockaddr_nl)) return -LINUX_EINVAL;
        struct sockaddr_nl dst;
        int rc=socket_user_import(sched_current_task(),address,&dst,sizeof(dst)); if (rc) return rc;
        if (dst.nl_family!=AF_NETLINK) return -LINUX_EAFNOSUPPORT;
        if (dst.nl_pid || dst.nl_groups) return -LINUX_ECONNREFUSED;
    }
    const uint8_t *input=data;
    uint32_t pos=0, maximum_reply=0, requests=0;
    bool has_dump=false;
    while (pos<length) {
        if (length-pos < sizeof(struct nlmsghdr)) return -LINUX_EINVAL;
        struct nlmsghdr h; __builtin_memcpy(&h,input+pos,sizeof(h));
        if (h.nlmsg_len < sizeof(h) || h.nlmsg_len > length-pos) return -LINUX_EINVAL;
        uint32_t advance=NLMSG_ALIGN(h.nlmsg_len);
        if (advance > length-pos) return -LINUX_EINVAL;
        uint32_t error_size=NLMSG_ALIGN(sizeof(struct nlmsghdr)+sizeof(struct nlmsgerr)+
                                         h.nlmsg_len-sizeof(struct nlmsghdr));
        if (error_size > NL_MESSAGE_MAX-maximum_reply) return -LINUX_ENOBUFS;
        maximum_reply+=error_size; ++requests;
        if (h.nlmsg_type==RTM_GETLINK || h.nlmsg_type==RTM_GETADDR || h.nlmsg_type==RTM_GETROUTE)
            has_dump=true;
        pos+=advance;
    }
    if (has_dump && requests>1) return -LINUX_EOPNOTSUPP;
    if (s->queued_bytes > NL_QUEUE_BUDGET - NL_MESSAGE_MAX) return -LINUX_ENOBUFS;
    uint8_t *bytes=kernel_malloc(length);
    if (!bytes) return -LINUX_ENOMEM;
    __builtin_memcpy(bytes,input,length);
    struct netlink_message *message=kernel_malloc(sizeof(*message)+NL_MESSAGE_MAX);
    if (!message) { kernel_free(bytes); return -LINUX_ENOMEM; }
    int rc=nl_autobind(s,sched_current_task());
    if (rc) { kernel_free(message); kernel_free(bytes); return rc; }
    /* Reserve the response slot before mutations can publish notifications. */
    s->queued_bytes+=NL_MESSAGE_MAX;
    struct nl_builder builder={.message=message,.port=s->port};
    for (pos=0;pos<length;) {
        const struct nlmsghdr *h=(const void *)(bytes+pos);
        rc=nl_request(&builder,h);
        if (rc) { s->queued_bytes-=NL_MESSAGE_MAX; kernel_free(message); kernel_free(bytes); return rc; }
        pos+=NLMSG_ALIGN(h->nlmsg_len);
    }
    kernel_free(bytes);
    if (!builder.used) { s->queued_bytes-=NL_MESSAGE_MAX; kernel_free(message); return (int)length; }
    message->next=NULL; message->length=builder.used; message->groups=0;
    if (s->tail) s->tail->next=message; else s->head=message;
    s->tail=message;
    kernel_poll_readiness_notify();
    return (int)length;
}

/** @brief Receive one queued datagram after source output has been prepared.
 * @param file Retained OFD. @param data Writable destination. @param length Capacity.
 * @param flags Native flags. @param address Optional user source. @param address_length
 * User length pointer. @param source Optional staged source. @param source_length Optional
 * staged length. @return Byte count, errno, or blocked marker. */
static int nl_receive(struct task_file *file,void *data,uint32_t length,uint32_t flags,
                      uint64_t address,uint64_t address_length,void *source,uint32_t *source_length)
{
    struct netlink_socket *s=nl_socket(file);
    if (!s) return -LINUX_EBADF;
    if (flags & ~(MSG_DONTWAIT|MSG_PEEK|MSG_TRUNC|MSG_WAITALL)) return -LINUX_EOPNOTSUPP;
    if (s->overrun) { s->overrun=false; return -LINUX_ENOBUFS; }
    if (!s->head) {
        if ((file->flags & RELIEFOS_O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -LINUX_EAGAIN;
        sched_sleep_current_until(time_ticks()+1);
        return KERNEL_SYSCALL_BLOCKED;
    }
    struct sockaddr_nl from={.nl_family=AF_NETLINK,.nl_groups=s->head->groups};
    if (address) {
        uint32_t cap; int rc=socket_user_length(sched_current_task(),address_length,&cap); if (rc) return rc;
        if ((int32_t)cap<0) return -LINUX_EINVAL;
        uint32_t n=cap<sizeof(from) ? cap : sizeof(from);
        rc=socket_user_prepare(sched_current_task(),address,n,true); if (rc) return rc;
        if (n && !user_range_writable(address,n)) return -LINUX_EFAULT;
        if (n) __builtin_memcpy((void *)(uintptr_t)address,&from,n);
        *(uint32_t *)(uintptr_t)address_length=sizeof(from);
    }
    if (source && source_length) { __builtin_memcpy(source,&from,sizeof(from)); *source_length=sizeof(from); }
    struct netlink_message *m=s->head;
    uint32_t copied=length<m->length ? length : m->length;
    if (copied) __builtin_memcpy(data,m->data,copied);
    int result=(flags & MSG_TRUNC) ? (int)m->length : (int)copied;
    if (!(flags & MSG_PEEK)) {
        s->head=m->next; if (!s->head) s->tail=NULL;
        s->queued_bytes-=NL_MESSAGE_MAX; kernel_free(m);
    }
    return result;
}

/** @brief Receive one route-netlink datagram with optional user source address.
 * @param file Retained OFD. @param data Output. @param length Capacity. @param flags Flags.
 * @param address User source. @param address_length User socklen. @return Bytes or errno. */
int task_netlink_recv(struct task_file *file,void *data,uint32_t length,uint32_t flags,
                      uint64_t address,uint64_t address_length)
{ return nl_receive(file,data,length,flags,address,address_length,NULL,NULL); }

/** @brief Receive into kernel-owned buffers for atomic staged user output.
 * @param file Retained OFD. @param data Kernel output. @param length Capacity. @param flags Flags.
 * @param source Optional source buffer. @param source_length Output size. @return Bytes or errno. */
int task_netlink_recv_staged(struct task_file *file,void *data,uint32_t length,uint32_t flags,
                             void *source,uint32_t *source_length)
{ return nl_receive(file,data,length,flags,0,0,source,source_length); }

/** @brief Return current socket readiness under execution ownership.
 * @param file Retained OFD. @param events Requested bits. @return Ready bits or POLLNVAL. */
short task_netlink_poll(const struct task_file *file,short events)
{
    struct netlink_socket *s=nl_socket(file);
    if (!s) return POLLNVAL;
    return (s->overrun ? POLLERR : 0) | (s->head ? events&POLLIN : 0) |
        (s->queued_bytes<=NL_QUEUE_BUDGET-NL_MESSAGE_MAX ? events&POLLOUT : 0);
}

/** @brief Return the next queued netlink datagram size.
 * @param file Retained OFD. @return Length, zero or EBADF. */
int task_netlink_available(const struct task_file *file)
{
    struct netlink_socket *s=nl_socket(file);
    return !s ? -LINUX_EBADF : s->head ? (int)s->head->length : 0;
}

/** @brief Free final socket ownership and its queue.
 * @param file Final OFD. */
void task_netlink_release(struct task_file *file)
{
    struct netlink_socket *s=nl_socket(file);
    if (!s) return;
    struct netlink_socket **link=&netlink_sockets;
    while (*link && *link!=s) link=&(*link)->next;
    if (*link) *link=s->next;
    if (s->groups) {
        bool subscribers=false;
        for (struct netlink_socket *it=netlink_sockets;it;it=it->next)
            if (it->groups) { subscribers=true; break; }
        if (!subscribers) nl_forget_snapshot();
    }
    while (s->head) { struct netlink_message *m=s->head; s->head=m->next; kernel_free(m); }
    kernel_free(s); file->aux=0;
}

/** @brief Cancel a polling wait; timed waits retain no socket or user pointer.
 * @param file Socket OFD. @param task Task being cancelled. */
void task_netlink_cancel_wait(struct task_file *file,struct task *task)
{ (void)file; (void)task; }

/** @brief Export a native socket address after validating both user outputs.
 * @param task Current task. @param address User destination. @param length User socklen.
 * @param value Native address. @return Zero or negative errno. */
static int nl_export_address(struct task *task,uint64_t address,uint64_t length,
                             const struct sockaddr_nl *value)
{
    uint32_t cap; int rc=socket_user_length(task,length,&cap); if (rc) return rc;
    if ((int32_t)cap<0) return -LINUX_EINVAL;
    uint32_t n=cap<sizeof(*value) ? cap : sizeof(*value);
    rc=socket_user_prepare(task,address,n,true); if (rc) return rc;
    if (n && !user_range_writable(address,n)) return -LINUX_EFAULT;
    if (n) __builtin_memcpy((void *)(uintptr_t)address,value,n);
    *(uint32_t *)(uintptr_t)length=sizeof(*value); return 0;
}

/** @brief Dispatch socket creation, bind, connect, name and supported options.
 * @param number Native syscall. @param a0 First argument. @param a1 Second argument.
 * @param a2 Third argument. @param a3 Fourth argument. @param a4 Fifth argument.
 * @param a5 Sixth argument. @return Syscall result or negative errno. */
int64_t syscall_netlink(uint64_t number,uint64_t a0,uint64_t a1,uint64_t a2,
                        uint64_t a3,uint64_t a4,uint64_t a5)
{
    (void)a5;
    struct task *task=sched_current_task();
    if (!task) return -LINUX_ESRCH;
    if (number==__NR_socket) {
        int type=(int)a1&15;
        if (a0!=AF_NETLINK) return -LINUX_EAFNOSUPPORT;
        if (a1 & ~(uint64_t)(15|SOCK_NONBLOCK|SOCK_CLOEXEC)) return -LINUX_EINVAL;
        if (type!=SOCK_RAW && type!=SOCK_DGRAM) return -LINUX_ESOCKTNOSUPPORT;
        if (a2!=NETLINK_ROUTE) return -LINUX_EPROTONOSUPPORT;
        struct netlink_socket *s=kernel_malloc(sizeof(*s)); if (!s) return -LINUX_ENOMEM;
        struct task_file *file; int fd=task_allocate_fd(task,0,&file);
        if (fd<0) { kernel_free(s); return fd; }
        *s=(struct netlink_socket){.next=netlink_sockets,.type=type}; netlink_sockets=s;
        file->flags=TASK_FILE_FLAG_SOCKET|TASK_FILE_FLAG_SOCKET_INET|RELIEFOS_O_RDWR|
                    ((a1&SOCK_NONBLOCK) ? RELIEFOS_O_NONBLOCK : 0);
        file->fd_flags=(a1&SOCK_CLOEXEC) ? RELIEFOS_FD_CLOEXEC : 0;
        file->kind=TASK_FILE_KIND_NETLINK; file->aux=(uintptr_t)s;
        file->socket_ino=(5ULL<<60)|(++netlink_ino & ((1ULL<<60)-1));
        file->node.type=RELIEFOS_FS_TYPE_DEVICE; return fd;
    }
#ifdef CONFIG_SWAP
    struct task_file *file=task_file_for_io(task,(int32_t)a0);
#else
    struct task_file *file=task_file_for_fd(task,(int32_t)a0);
#endif
    struct netlink_socket *s=nl_socket(file); if (!s) return -LINUX_EBADF;
    if (number==__NR_bind || number==__NR_connect) {
        if ((int32_t)a2<0 || a2<sizeof(struct sockaddr_nl)) return -LINUX_EINVAL;
        struct sockaddr_nl input; int rc=socket_user_import(task,a1,&input,sizeof(input)); if (rc) return rc;
        if (input.nl_family!=AF_NETLINK) return -LINUX_EAFNOSUPPORT;
        if (number==__NR_connect) {
            if (input.nl_pid || input.nl_groups) return -LINUX_ECONNREFUSED;
            rc=nl_autobind(s,task); if (rc) return rc;
            s->connected=true; return 0;
        }
        if (s->bound) return -LINUX_EINVAL;
        if (input.nl_pid && nl_port_used(input.nl_pid,s)) return -LINUX_EADDRINUSE;
        rc=nl_set_groups(s,input.nl_groups); if (rc) return rc;
        if (!input.nl_pid) {
            rc=nl_autobind(s,task);
            if (rc) (void)nl_set_groups(s,0);
            return rc;
        }
        s->bound=true; s->port=input.nl_pid; return 0;
    }
    if (number==__NR_getsockname || number==__NR_getpeername) {
        struct sockaddr_nl result={.nl_family=AF_NETLINK,
            .nl_pid=number==__NR_getsockname && s->bound ? s->port : 0,
            .nl_groups=number==__NR_getsockname ? s->groups : 0};
        return nl_export_address(task,a1,a2,&result);
    }
    if (number==__NR_setsockopt || number==__NR_getsockopt) {
        if (a1==SOL_NETLINK && number==__NR_setsockopt &&
            (a2==NETLINK_ADD_MEMBERSHIP || a2==NETLINK_DROP_MEMBERSHIP)) {
            if (a4<sizeof(uint32_t)) return -LINUX_EINVAL;
            uint32_t group; int rc=socket_user_import(task,a3,&group,sizeof(group)); if (rc) return rc;
            if (!group || group>32) return -LINUX_EINVAL;
            uint32_t mask=1u<<(group-1);
            if (!(mask & NL_SUPPORTED_GROUPS)) return -LINUX_EOPNOTSUPP;
            return nl_set_groups(s,a2==NETLINK_ADD_MEMBERSHIP ? s->groups|mask : s->groups&~mask);
        }
        if (a1!=SOL_SOCKET) return -LINUX_ENOPROTOOPT;
        if (number==__NR_setsockopt) return -LINUX_ENOPROTOOPT;
        int value;
        if (a2==SO_TYPE) value=s->type;
        else if (a2==SO_DOMAIN) value=AF_NETLINK;
        else if (a2==SO_PROTOCOL) value=0;
        else if (a2==SO_ERROR) value=s->overrun ? LINUX_ENOBUFS : 0;
        else return -LINUX_ENOPROTOOPT;
        uint32_t cap; int rc=socket_user_length(task,a4,&cap); if (rc) return rc;
        if ((int32_t)cap<0) return -LINUX_EINVAL;
        uint32_t n=cap<sizeof(value) ? cap : sizeof(value);
        rc=socket_user_prepare(task,a3,n,true); if (rc) return rc;
        if (n && !user_range_writable(a3,n)) return -LINUX_EFAULT;
        if (n) __builtin_memcpy((void *)(uintptr_t)a3,&value,n);
        *(uint32_t *)(uintptr_t)a4=n;
        if (a2==SO_ERROR) s->overrun=false;
        return 0;
    }
    return -LINUX_EOPNOTSUPP;
}
