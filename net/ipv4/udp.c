#include "../internal.h"



/**
 * Net send udp to mac.
 * @param dst_mac Value supplied by the caller.
 * @param src_ip Value supplied by the caller.
 * @param dst_ip Value supplied by the caller.
 * @param src_port Value supplied by the caller.
 * @param dst_port Value supplied by the caller.
 * @param payload Value supplied by the caller.
 * @param payload_len Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_send_udp_to_mac(const uint8_t *dst_mac, uint32_t src_ip,
                               uint32_t dst_ip, uint16_t src_port,
                               uint16_t dst_port, const uint8_t *payload,
                               uint32_t payload_len)
{
    uint8_t frame[NET_FRAME_MAX];
    uint8_t *ip = frame + 14;
    uint8_t *udp = frame + 34;
    uint16_t udp_len;
    uint16_t total_len;
    if ((payload_len && !payload) || payload_len > NET_FRAME_MAX - 42u) {
        return -1;
    }
    udp_len = (uint16_t)(8u + payload_len);
    total_len = (uint16_t)(20u + udp_len);
    net_memzero(frame, 42u + payload_len);
    net_write_eth(frame, dst_mac, ETH_TYPE_IPV4);
    ip[0] = 0x45;
    ip[1] = 0;
    net_put_u16(ip + 2, total_len);
    net_put_u16(ip + 4, net_ipv4_id++);
    net_put_u16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = IPV4_PROTO_UDP;
    net_put_u32(ip + 12, src_ip);
    net_put_u32(ip + 16, dst_ip);
    net_put_u16(ip + 10, net_checksum(ip, 20));
    net_put_u16(udp, src_port);
    net_put_u16(udp + 2, dst_port);
    net_put_u16(udp + 4, udp_len);
    net_put_u16(udp + 6, 0);
    net_memcpy(udp + 8, payload, payload_len);
    return net_interface_up ? e1000_send(frame, 42u + payload_len) : -LINUX_ENETDOWN;
}

/**
 * Net handle udp.
 * @param ip Value supplied by the caller.
 * @param total_len Value supplied by the caller.
 * @param ihl Value supplied by the caller.
 * @param src_ip Value supplied by the caller.
 * @param udp_wait Value supplied by the caller.
 */
void net_handle_udp(const uint8_t *ip, uint32_t total_len,
                           uint32_t ihl, uint32_t src_ip,
                           struct net_udp_wait *udp_wait)
{
    const uint8_t *udp;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t udp_len;
    uint32_t payload_len;
    if (total_len < ihl + 8u) {
        return;
    }
    udp = ip + ihl;
    src_port = net_get_u16(udp);
    dst_port = net_get_u16(udp + 2);
    udp_len = net_get_u16(udp + 4);
    if (udp_len < 8u || ihl + udp_len > total_len) {
        return;
    }
    if (net_get_u16(udp + 6)) {
        uint32_t sum = net_checksum_partial(0, ip + 12, 8);
        sum += IPV4_PROTO_UDP + udp_len;
        if (net_checksum_finish(net_checksum_partial(sum, udp, udp_len))) return;
    }
    net_udp_input(src_ip, net_get_u32(ip + 16), src_port, dst_port, udp + 8, udp_len - 8, 2);
    if (!udp_wait || udp_wait->done) return;
    if (udp_wait->src_port && src_port != udp_wait->src_port) {
        return;
    }
    if (udp_wait->dst_port && dst_port != udp_wait->dst_port) {
        return;
    }
    if (udp_wait->src_ip && src_ip != udp_wait->src_ip) {
        return;
    }
    payload_len = udp_len - 8u;
    if (payload_len > udp_wait->capacity) {
        payload_len = udp_wait->capacity;
    }
    net_memcpy(udp_wait->payload, udp + 8, payload_len);
    udp_wait->length = payload_len;
    udp_wait->done = 1;
}

int net_ipv4_send_udp(uint32_t source, uint32_t destination, uint16_t source_port,
                       uint16_t destination_port, const void *data, uint32_t length)
{
    if (!e1000_is_ready()) return -LINUX_ENETDOWN;
    if (net_ensure_ipv4_config(3000, 0) != RELIEFOS_NET_STATUS_OK) return -LINUX_ENETUNREACH;
    uint8_t mac[6];
    if (destination == 0xffffffffu) net_memcpy(mac, net_broadcast_mac, 6);
    else if (net_resolve_mac(net_route_arp_ip(destination), 1000, mac) < 0) return -LINUX_EHOSTUNREACH;
    if (!source) source = net_config.local_ip;
    return net_send_udp_to_mac(mac, source, destination, source_port, destination_port,
                               data, length) < 0 ? -LINUX_EIO : 0;
}
