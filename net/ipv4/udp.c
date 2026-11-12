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
int net_send_udp_to_mac_on(uint32_t ifindex, const uint8_t *dst_mac, uint32_t src_ip,
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
    net_write_eth_on(ifindex, frame, dst_mac, ETH_TYPE_IPV4);
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
    return net_interface_is_ready(ifindex) ? netdev_send(ifindex, frame, 42u + payload_len) : -LINUX_ENETDOWN;
}

/**
 * Net handle udp.
 * @param ip Value supplied by the caller.
 * @param total_len Value supplied by the caller.
 * @param ihl Value supplied by the caller.
 * @param src_ip Value supplied by the caller.
 * @param udp_wait Value supplied by the caller.
 */
void net_handle_udp_on(uint32_t ifindex, const uint8_t *ip, uint32_t total_len,
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
    net_udp_input(src_ip, net_get_u32(ip + 16), src_port, dst_port, udp + 8, udp_len - 8, (int)ifindex);
    if (!udp_wait || udp_wait->done || (udp_wait->ifindex && udp_wait->ifindex != ifindex)) return;
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

/** @brief Receive UDP on the primary compatibility interface.
 * @param ip Packet. @param total_len IP size. @param ihl Header bytes.
 * @param src_ip Host-order source. @param wait Optional waiter. */
void net_handle_udp(const uint8_t *ip, uint32_t total_len, uint32_t ihl,
                     uint32_t src_ip, struct net_udp_wait *wait)
{
    net_handle_udp_on(netdev_next(0), ip, total_len, ihl, src_ip, wait);
}

/** @brief Send an IPv4 datagram with an explicit bound interface or route selection.
 * @param ifindex Bound device or zero. @param source Optional source address.
 * @param destination Peer address. @param source_port Local port. @param destination_port Peer port.
 * @param data Borrowed payload. @param length Bytes. @return Zero or negative errno. */
int net_ipv4_send_udp_on(uint32_t ifindex, uint32_t source, uint32_t destination,
                          uint16_t source_port, uint16_t destination_port,
                          const void *data, uint32_t length)
{
    if (length > 1472 || (length && !data)) return -LINUX_EMSGSIZE;
    struct net_ipv4_path path;
    int result = net_ipv4_route_lookup(destination, source, ifindex, &path);
    if (result < 0) return result;
    uint8_t mac[6];
    struct net_interface *interface = net_interface_get(path.ifindex);
    if (destination == UINT32_MAX || (interface && destination == interface->broadcast))
        net_memcpy(mac, net_broadcast_mac, 6);
    else if (net_resolve_mac_on(path.ifindex, path.next_hop, 1000, mac) < 0)
        return -LINUX_EHOSTUNREACH;
    result = net_send_udp_to_mac_on(path.ifindex, mac, path.source, destination,
                                    source_port, destination_port, data, length);
    return result < 0 ? result : 0;
}

/** @brief Send UDP with automatic output device selection.
 * @param source Optional source. @param destination Peer. @param source_port Local port.
 * @param destination_port Peer port. @param data Payload. @param length Bytes.
 * @return Zero or negative errno. */
int net_ipv4_send_udp(uint32_t source, uint32_t destination, uint16_t source_port,
                       uint16_t destination_port, const void *data, uint32_t length)
{
    return net_ipv4_send_udp_on(0, source, destination, source_port, destination_port, data, length);
}

/** @brief Build a legacy UDP frame using destination-based route selection.
 * @param mac Destination MAC. @param source IPv4 source. @param destination Peer.
 * @param source_port Local port. @param destination_port Peer port.
 * @param data Payload. @param length Bytes. @return Driver result or negative errno. */
int net_send_udp_to_mac(const uint8_t *mac, uint32_t source, uint32_t destination,
    uint16_t source_port, uint16_t destination_port, const uint8_t *data, uint32_t length)
{
    struct net_ipv4_path path;
    int result = net_ipv4_route_lookup(destination, source, 0, &path);
    if (result < 0) return result;
    return net_send_udp_to_mac_on(path.ifindex, mac, source, destination,
                                  source_port, destination_port, data, length);
}
