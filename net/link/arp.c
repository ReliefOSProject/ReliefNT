#include "../internal.h"
#include <reliefnt/proc_stats.h>

static struct net_arp_cache_entry net_arp_cache[NET_ARP_CACHE_SIZE];
static uint32_t net_arp_cache_next;

/** @brief Invalidate neighbors for one link without disturbing other links.
 * @param ifindex Interface, or zero for a complete reset. */
void net_arp_cache_clear_on(uint32_t ifindex)
{
    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i)
        if (!ifindex || net_arp_cache[i].ifindex == ifindex)
            net_arp_cache[i] = (struct net_arp_cache_entry){0};
}

/** @brief Reset the primary interface's compatibility ARP cache. */
void net_arp_cache_clear(void)
{
    net_arp_cache_clear_on(netdev_next(0));
}

/** @brief Copy a resolved neighbor on the specified Ethernet link.
 * @param ifindex Link. @param ip Host-order neighbor. @param mac Six output bytes.
 * @return One for a resolved entry, zero for a miss. */
static int net_arp_lookup_on(uint32_t ifindex, uint32_t ip, uint8_t mac[6])
{
    if (!ip || !mac) return 0;
    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i) {
        const struct net_arp_cache_entry *entry = &net_arp_cache[i];
        if (entry->ifindex == ifindex && entry->ip == ip && !net_mac_is_zero(entry->mac)) {
            net_memcpy(mac, entry->mac, 6);
            return 1;
        }
    }
    return 0;
}

/** @brief Cache a unicast mapping with its ingress link identity.
 * @param ifindex Link. @param ip Host-order neighbor. @param mac Six input bytes. */
static void net_arp_store_on(uint32_t ifindex, uint32_t ip, const uint8_t mac[6])
{
    if (!ip || !mac || net_mac_is_zero(mac) || ip == UINT32_MAX || (mac[0] & 1)) return;
    uint32_t slot = NET_ARP_CACHE_SIZE;
    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i) {
        if ((net_arp_cache[i].ifindex == ifindex && net_arp_cache[i].ip == ip) || !net_arp_cache[i].ip) {
            slot = i;
            break;
        }
    }
    if (slot == NET_ARP_CACHE_SIZE) slot = net_arp_cache_next++ % NET_ARP_CACHE_SIZE;
    net_arp_cache[slot].ifindex = ifindex;
    net_arp_cache[slot].ip = ip;
    net_memcpy(net_arp_cache[slot].mac, mac, 6);
}

/** @brief Construct one ARP request/reply from the selected link's configuration.
 * @param ifindex Link. @param frame Forty-two output bytes. @param op ARP opcode.
 * @param dst_mac Ethernet destination. @param target_mac ARP destination.
 * @param target_ip Host-order target IPv4 address. */
static void net_write_arp_on(uint32_t ifindex, uint8_t frame[42], uint16_t op,
                              const uint8_t dst_mac[6], const uint8_t target_mac[6], uint32_t target_ip)
{
    struct reliefos_net_config config = {0};
    net_interface_get_config(ifindex, &config);
    net_write_eth_on(ifindex, frame, dst_mac, ETH_TYPE_ARP);
    net_put_u16(frame + 14, ARP_HTYPE_ETHERNET);
    net_put_u16(frame + 16, ETH_TYPE_IPV4);
    frame[18] = 6;
    frame[19] = 4;
    net_put_u16(frame + 20, op);
    net_memcpy(frame + 22, netdev_mac(ifindex), 6);
    net_put_u32(frame + 28, config.local_ip);
    net_memcpy(frame + 32, target_mac, 6);
    net_put_u32(frame + 38, target_ip);
}

/** @brief Process ARP learning and replies on exactly the receiving link.
 * @param ifindex Ingress link. @param frame Borrowed Ethernet/ARP packet.
 * @param len Bytes available. @param arp_wait Optional link-scoped waiter. */
void net_handle_arp_on(uint32_t ifindex, const uint8_t *frame, uint32_t len,
                        struct net_arp_wait *arp_wait)
{
    if (len < 42 || net_get_u16(frame + 14) != ARP_HTYPE_ETHERNET ||
        net_get_u16(frame + 16) != ETH_TYPE_IPV4 || frame[18] != 6 || frame[19] != 4) return;
    uint16_t op = net_get_u16(frame + 20);
    if (op != ARP_OPER_REQUEST && op != ARP_OPER_REPLY) return;
    uint32_t sender_ip = net_get_u32(frame + 28), target_ip = net_get_u32(frame + 38);
    const uint8_t *sender_mac = frame + 22;
    net_arp_store_on(ifindex, sender_ip, sender_mac);
    if (arp_wait && (!arp_wait->ifindex || arp_wait->ifindex == ifindex) && arp_wait->ip == sender_ip) {
        net_memcpy(arp_wait->mac, sender_mac, 6);
        arp_wait->done = 1;
    }
    struct reliefos_net_config config = {0};
    if (op == ARP_OPER_REQUEST && !net_interface_get_config(ifindex, &config) &&
        config.local_ip && target_ip == config.local_ip && net_interface_is_ready(ifindex)) {
        uint8_t reply[42];
        net_write_arp_on(ifindex, reply, ARP_OPER_REPLY, sender_mac, sender_mac, sender_ip);
        (void)netdev_send(ifindex, reply, sizeof(reply));
    }
}

/** @brief Process ARP on the primary compatibility link.
 * @param frame Borrowed packet. @param len Byte count. @param wait Optional waiter. */
void net_handle_arp(const uint8_t *frame, uint32_t len, struct net_arp_wait *wait)
{
    net_handle_arp_on(netdev_next(0), frame, len, wait);
}

/** @brief Resolve a neighbor while polling all links with an interface-specific waiter.
 * @param ifindex Egress device. @param ip Host-order next hop.
 * @param timeout_ms Maximum uptime wait. @param out_mac Six output bytes.
 * @return Zero, negative driver error, or legacy minus two on timeout. */
int net_resolve_mac_on(uint32_t ifindex, uint32_t ip, uint32_t timeout_ms, uint8_t out_mac[6])
{
    if (!out_mac || !ip) return -LINUX_EINVAL;
    if (!net_interface_is_ready(ifindex)) return -LINUX_ENETDOWN;
    if (net_arp_lookup_on(ifindex, ip, out_mac)) return 0;
    struct net_arp_wait wait = {.ifindex = ifindex, .ip = ip};
    uint64_t start = time_uptime_ms(), next_request = start;
    uint32_t spins = 0;
    while (!net_timeout_expired(start, timeout_ms, spins++)) {
        if (!net_interface_is_ready(ifindex)) return -LINUX_ENETDOWN;
        uint64_t now = time_uptime_ms();
        if (now >= next_request) {
            uint8_t frame[42], zero[6] = {0};
            net_write_arp_on(ifindex, frame, ARP_OPER_REQUEST, net_broadcast_mac, zero, ip);
            int result = netdev_send(ifindex, frame, sizeof(frame));
            if (result < 0) return result;
            next_request = now + 250;
        }
        net_poll_once(&wait, NULL, NULL, NULL);
        if (wait.done) {
            net_memcpy(out_mac, wait.mac, 6);
            return 0;
        }
        net_cpu_relax();
    }
    return -2;
}

/** @brief Resolve a next hop using the route selected for its address.
 * @param ip Host-order next hop. @param timeout_ms Bound. @param out_mac Output.
 * @return Zero or negative failure/timeout. */
int net_resolve_mac(uint32_t ip, uint32_t timeout_ms, uint8_t out_mac[6])
{
    struct net_ipv4_path path;
    int result = net_ipv4_route_lookup(ip, 0, 0, &path);
    if (result < 0) return result;
    return net_resolve_mac_on(path.ifindex, ip, timeout_ms, out_mac);
}

/** @brief Emit live ARP entries with their actual interface names.
 * @param stream Execution-owned output slice. */
void net_arp_proc_emit(struct text_stream *stream)
{
    text_string(stream, "IP address       HW type     Flags       HW address            Mask     Device\n");
    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i) {
        const struct net_arp_cache_entry *entry = &net_arp_cache[i];
        char name[NET_IF_NAME_LEN];
        if (!entry->ip || !net_interface_name(entry->ifindex, name)) continue;
        for (int byte = 3; byte >= 0; --byte) {
            text_unsigned(stream, (entry->ip >> (byte * 8)) & 255);
            if (byte) text_string(stream, ".");
        }
        text_string(stream, " 0x1 0x2 ");
        for (unsigned byte = 0; byte < 6; ++byte) {
            if (byte) text_string(stream, ":");
            text_hex(stream, entry->mac[byte], 2);
        }
        text_string(stream, " * ");
        text_string(stream, name);
        text_string(stream, "\n");
    }
}
