/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/proc_stats.h>
#include <reliefnt/net_ipv6.h>
#include <linux/rtnetlink.h>

/** @brief Emit the 16 address octets in Linux proc's contiguous hexadecimal form.
 * @param s Output stream. @param address Network-order address. */
static void proc_ipv6(struct text_stream *s, const struct in6_addr *address)
{
    for (unsigned i = 0; i < 16; ++i) text_hex(s,address->s6_addr[i],2);
}

/** @brief Map routing scope values to the separate /proc/net/if_inet6 encoding.
 * @param scope Linux routing scope. @return Proc-specific hexadecimal scope. */
static uint32_t proc_ipv6_scope(uint32_t scope)
{
    if (scope == RT_SCOPE_HOST) return 0x10;
    if (scope == RT_SCOPE_LINK) return 0x20;
    if (scope == RT_SCOPE_SITE) return 0x40;
    return 0;
}

/** @brief Render a snapshot of the currently assigned IPv6 addresses.
 * @param s Output stream. @return Zero or a memory allocation error. */
static int proc_ipv6_addresses(struct text_stream *s)
{
    uint32_t count=net_ipv6_addresses(NULL,0);
    if (!count) return 0;
    if (count > UINT32_MAX / sizeof(struct net_ipv6_address)) return -LINUX_ENOMEM;
    struct net_ipv6_address *entries=kernel_malloc((size_t)count*sizeof(*entries));
    if (!entries) return -LINUX_ENOMEM;
    if (net_ipv6_addresses(entries,count)>count) { kernel_free(entries); return -LINUX_EAGAIN; }
    for (uint32_t i=0;i<count;++i) {
        struct net_ipv6_address *a=&entries[i];
        const char *name=a->ifindex==1 ? "lo" : a->ifindex==2 ? "eth0" : NULL;
        if (!name) continue;
        proc_ipv6(s,&a->address); text_string(s," "); text_hex(s,a->ifindex,2);
        text_string(s," "); text_hex(s,a->prefix_length,2);
        text_string(s," "); text_hex(s,proc_ipv6_scope(a->scope),2);
        text_string(s," "); text_hex(s,a->flags,2);
        text_string(s," "); text_string(s,name); text_string(s,"\n");
    }
    kernel_free(entries); return 0;
}

/** @brief Render the current IPv6 routes in Linux's hexadecimal proc format.
 * @param s Output stream. @return Zero or a memory allocation error. */
static int proc_ipv6_routes(struct text_stream *s)
{
    uint32_t count=net_ipv6_routes(NULL,0);
    if (!count) return 0;
    if (count > UINT32_MAX / sizeof(struct net_ipv6_route)) return -LINUX_ENOMEM;
    struct net_ipv6_route *entries=kernel_malloc((size_t)count*sizeof(*entries));
    if (!entries) return -LINUX_ENOMEM;
    if (net_ipv6_routes(entries,count)>count) { kernel_free(entries); return -LINUX_EAGAIN; }
    static const struct in6_addr zero;
    for (uint32_t i=0;i<count;++i) {
        struct net_ipv6_route *r=&entries[i];
        const char *name=r->ifindex==1 ? "lo" : r->ifindex==2 ? "eth0" : NULL;
        if (!name) continue;
        proc_ipv6(s,&r->destination); text_string(s," "); text_hex(s,r->prefix_length,2);
        text_string(s," "); proc_ipv6(s,&zero); text_string(s," 00 ");
        proc_ipv6(s,&r->gateway); text_string(s," "); text_hex(s,r->metric,8);
        text_string(s," 00000000 00000000 "); text_hex(s,r->flags,8);
        text_string(s," "); text_string(s,name); text_string(s,"\n");
    }
    kernel_free(entries); return 0;
}

/** @brief Emit an IPv4 address using Linux proc's native little-endian hex.
 * @param s Output. @param ip Host-order address. No ownership changes.
 */
static void proc_ip(struct text_stream *s, uint32_t ip)
{
    text_hex(s, __builtin_bswap32(ip), 8);
}
/** @brief Render core network state without driving the NIC or timers.
 * @param s Required slice. @param leaf Known name inside proc/net.
 * @return Zero or ENOENT. Execution-owned; no code/reference escapes.
 */
int net_proc_emit(struct text_stream *s, const char *leaf)
{
    if (!__builtin_strcmp(leaf, "if_inet6"))
        return proc_ipv6_addresses(s);
    if (!__builtin_strcmp(leaf, "ipv6_route"))
        return proc_ipv6_routes(s);
    if (!__builtin_strcmp(leaf, "tcp6") || !__builtin_strcmp(leaf, "udp6") ||
        !__builtin_strcmp(leaf, "raw6")) {
        net_ipv6_transport_proc_emit(s,leaf);
        return 0;
    }
    if (!__builtin_strcmp(leaf, "dev")) {
        struct net_proc_counters c;
        net_device_counters(&c);
        text_string(s, "Inter-|   Receive                                                |  "
                       "Transmit\n face |bytes    packets errs drop fifo frame compressed "
                       "multicast|bytes    packets errs drop fifo colls carrier compressed\n");
        /* Loopback datagrams are accounted by their owner rather than the NIC. */
        net_loopback_proc_emit(s);
        struct e1000_info info;
        e1000_get_info(&info);
        if (info.present) {
            text_string(s, "  eth0:");
            uint64_t fields[] = {c.rx_bytes, c.rx_packets, c.rx_errors, c.rx_drops, 0, 0, 0, 0,
                                 c.tx_bytes, c.tx_packets, c.tx_errors, 0,          0, 0, 0, 0};
            for (unsigned i = 0; i < 16; ++i) {
                text_string(s, " ");
                text_unsigned(s, fields[i]);
            }
            text_string(s, "\n");
        }
    } else if (!__builtin_strcmp(leaf, "route")) {
        text_string(s, "Iface\tDestination\tGateway "
                       "\tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
        /* No IPv4 forwarding/routing table exists beyond loopback, connected
         * subnet and the configured default gateway. Emit only those routes. */
        text_string(s, "lo\t0000007f\t00000000\t0001\t0\t0\t0\t000000ff\t0\t0\t0\n");
        if (net_interface_up && net_config.local_ip) {
            text_string(s, "eth0\t");
            proc_ip(s, net_config.local_ip & net_config.subnet_mask);
            text_string(s, "\t00000000\t0001\t0\t0\t0\t");
            proc_ip(s, net_config.subnet_mask);
            text_string(s, "\t0\t0\t0\n");
            if (net_config.gateway_ip) {
                text_string(s, "eth0\t00000000\t");
                proc_ip(s, net_config.gateway_ip);
                text_string(s, "\t0003\t0\t0\t0\t00000000\t0\t0\t0\n");
            }
        }
    } else if (!__builtin_strcmp(leaf, "arp"))
        net_arp_proc_emit(s);
    else if (!__builtin_strcmp(leaf, "udp"))
        net_udp_proc_emit(s);
    else if (!__builtin_strcmp(leaf, "unix"))
        unix_proc_emit(s);
    else if (!__builtin_strcmp(leaf, "tcp")) {
        text_string(s, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
                       "retrnsmt   uid  timeout inode\n");
        uint32_t index = 0;
        for (struct net_socket *n = net_sockets; n; n = n->registry_next) {
            if (!n->used || !n->local_port || n->ipv6)
                continue;
            unsigned state = n->state == RELIEFOS_NET_TCP_SYN_SENT ? 2
                             : n->state == RELIEFOS_NET_TCP_ESTABLISHED
                                 ? (n->fin_received ? (n->fin_sent ? 9 : 8)
                                    : n->fin_sent   ? (n->fin_acked ? 5 : 4)
                                                    : 1)
                             : n->state == RELIEFOS_NET_TCP_TIME_WAIT ? 6
                                                                      : 7;
            text_unsigned(s, index++);
            text_string(s, ": ");
            proc_ip(s, n->local_ip);
            text_string(s, ":");
            text_hex(s, n->local_port, 4);
            text_string(s, " ");
            proc_ip(s, n->remote_ip);
            text_string(s, ":");
            text_hex(s, n->remote_port, 4);
            text_string(s, " ");
            text_hex(s, state, 2);
            text_string(s, " ");
            text_hex(s, n->tx_len, 8);
            text_string(s, ":");
            text_hex(s, n->rx_len, 8);
            text_string(s, " 00:00000000 ");
            text_hex(s, n->tx_retries, 8);
            text_string(s, " ");
            text_unsigned(s, n->owner_uid);
            text_string(s, " 0 ");
            text_unsigned(s, (2ULL << 60) | (uint32_t)n->handle);
            text_string(s, "\n");
        }
    } else
        return -LINUX_ENOENT;
    return 0;
}
