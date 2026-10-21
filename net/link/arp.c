#include "../internal.h"

/**
 * Net arp cache lookup.
 * @param ip Value supplied by the caller.
 * @param mac Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_arp_cache_lookup(uint32_t ip, uint8_t mac[6]);

/**
 * Net arp cache store.
 * @param ip Value supplied by the caller.
 * @param mac Value supplied by the caller.
 */
static void net_arp_cache_store(uint32_t ip, const uint8_t mac[6]);

/**
 * Net write arp ipv4.
 * @param frame Value supplied by the caller.
 * @param op Identifier or flags controlling the operation.
 * @param dst_eth_mac Value supplied by the caller.
 * @param target_mac Value supplied by the caller.
 * @param target_ip Value supplied by the caller.
 */
static void net_write_arp_ipv4(uint8_t *frame, uint16_t op,
                               const uint8_t *dst_eth_mac,
                               const uint8_t *target_mac,
                               uint32_t target_ip);

/**
 * Net send arp request.
 * @param target_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_send_arp_request(uint32_t target_ip);

/**
 * Net send arp reply.
 * @param target_mac Value supplied by the caller.
 * @param target_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_send_arp_reply(const uint8_t *target_mac, uint32_t target_ip);

static struct net_arp_cache_entry net_arp_cache[NET_ARP_CACHE_SIZE];
static uint32_t net_arp_cache_next;
/**
 * Net arp cache clear.
 */
void net_arp_cache_clear(void)
{
    net_memzero(net_arp_cache, sizeof(net_arp_cache));
    net_arp_cache_next = 0;
}

/**
 * Net arp cache lookup.
 * @param ip Value supplied by the caller.
 * @param mac Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_arp_cache_lookup(uint32_t ip, uint8_t mac[6])
{
    if (!ip || !mac) {
        return 0;
    }
    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i) {
        if (net_arp_cache[i].ip == ip &&
            !net_mac_is_zero(net_arp_cache[i].mac)) {
            net_memcpy(mac, net_arp_cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

/**
 * Net arp cache store.
 * @param ip Value supplied by the caller.
 * @param mac Value supplied by the caller.
 */
static void net_arp_cache_store(uint32_t ip, const uint8_t mac[6])
{
    uint32_t slot;
    if (!ip || !mac || net_mac_is_zero(mac) ||
        ip == 0xffffffffu || net_memeq(mac, net_broadcast_mac, 6)) {
        return;
    }
    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; ++i) {
        if (net_arp_cache[i].ip == ip || net_arp_cache[i].ip == 0) {
            net_arp_cache[i].ip = ip;
            net_memcpy(net_arp_cache[i].mac, mac, 6);
            return;
        }
    }
    slot = net_arp_cache_next++ % NET_ARP_CACHE_SIZE;
    net_arp_cache[slot].ip = ip;
    net_memcpy(net_arp_cache[slot].mac, mac, 6);
}

/**
 * Net write arp ipv4.
 * @param frame Value supplied by the caller.
 * @param op Identifier or flags controlling the operation.
 * @param dst_eth_mac Value supplied by the caller.
 * @param target_mac Value supplied by the caller.
 * @param target_ip Value supplied by the caller.
 */
static void net_write_arp_ipv4(uint8_t *frame, uint16_t op,
                               const uint8_t *dst_eth_mac,
                               const uint8_t *target_mac,
                               uint32_t target_ip)
{
    const uint8_t *src_mac = e1000_mac();
    net_write_eth(frame, dst_eth_mac, ETH_TYPE_ARP);
    net_put_u16(frame + 14, ARP_HTYPE_ETHERNET);
    net_put_u16(frame + 16, ETH_TYPE_IPV4);
    frame[18] = 6;
    frame[19] = 4;
    net_put_u16(frame + 20, op);
    net_memcpy(frame + 22, src_mac, 6);
    net_put_u32(frame + 28, net_config.local_ip);
    net_memcpy(frame + 32, target_mac, 6);
    net_put_u32(frame + 38, target_ip);
}

/**
 * Net send arp request.
 * @param target_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_send_arp_request(uint32_t target_ip)
{
    uint8_t frame[64];
    uint8_t zero_mac[6] = {0, 0, 0, 0, 0, 0};
    net_memzero(frame, sizeof(frame));
    net_write_arp_ipv4(frame, ARP_OPER_REQUEST, net_broadcast_mac,
                       zero_mac, target_ip);
    return net_interface_up ? e1000_send(frame, 42) : -LINUX_ENETDOWN;
}

/**
 * Net send arp reply.
 * @param target_mac Value supplied by the caller.
 * @param target_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_send_arp_reply(const uint8_t *target_mac, uint32_t target_ip)
{
    uint8_t frame[64];
    net_memzero(frame, sizeof(frame));
    net_write_arp_ipv4(frame, ARP_OPER_REPLY, target_mac, target_mac, target_ip);
    return net_interface_up ? e1000_send(frame, 42) : -LINUX_ENETDOWN;
}

/**
 * Net handle arp.
 * @param frame Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param arp_wait Value supplied by the caller.
 */
void net_handle_arp(const uint8_t *frame, uint32_t len,
                           struct net_arp_wait *arp_wait)
{
    uint16_t op;
    uint32_t sender_ip;
    uint32_t target_ip;
    const uint8_t *sender_mac;
    if (len < 42 ||
        net_get_u16(frame + 14) != ARP_HTYPE_ETHERNET ||
        net_get_u16(frame + 16) != ETH_TYPE_IPV4 ||
        frame[18] != 6 || frame[19] != 4) {
        return;
    }
    op = net_get_u16(frame + 20);
    sender_mac = frame + 22;
    sender_ip = net_get_u32(frame + 28);
    target_ip = net_get_u32(frame + 38);
    net_arp_cache_store(sender_ip, sender_mac);
    if (op == ARP_OPER_REQUEST && target_ip == net_config.local_ip) {
        (void)net_send_arp_reply(sender_mac, sender_ip);
        return;
    }
    if (op == ARP_OPER_REPLY && arp_wait && sender_ip == arp_wait->ip) {
        net_memcpy(arp_wait->mac, sender_mac, 6);
        arp_wait->done = 1;
    }
}

/**
 * Net resolve mac.
 * @param ip Value supplied by the caller.
 * @param timeout_ms Value supplied by the caller.
 * @param out_mac Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_resolve_mac(uint32_t ip, uint32_t timeout_ms, uint8_t *out_mac)
{
    struct net_arp_wait wait;
    uint64_t start = time_uptime_ms();
    uint64_t next_request = start;
    uint32_t spins = 0;
    wait = (struct net_arp_wait){0};
    wait.ip = ip;
    if (net_arp_cache_lookup(ip, out_mac)) {
        return 0;
    }
    while (!net_timeout_expired(start, timeout_ms, spins++)) {
        uint64_t now = time_uptime_ms();
        if (now >= next_request) {
            if (net_send_arp_request(ip) < 0) {
                return -1;
            }
            next_request = now + 250u;
        }
        net_poll_once(&wait, 0, 0, 0);
        if (wait.done) {
            net_memcpy(out_mac, wait.mac, 6);
            net_arp_cache_store(ip, wait.mac);
            return 0;
        }
        net_cpu_relax();
    }
    return -2;
}
