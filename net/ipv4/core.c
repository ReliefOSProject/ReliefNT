#include "../internal.h"

/**
 * Net ip for us.
 * @param dst_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_ip_for_us(uint32_t dst_ip);

static uint16_t net_packet_ip_id;
/**
 * Net ip for us.
 * @param dst_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_ip_for_us(uint32_t dst_ip)
{
    return dst_ip == net_config.local_ip ||
           dst_ip == 0xffffffffu ||
           dst_ip == 0;
}

/**
 * Net handle ipv4.
 * @param frame Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param ping_wait Value supplied by the caller.
 * @param udp_wait Value supplied by the caller.
 * @param tcp_wait Value supplied by the caller.
 */
void net_handle_ipv4(const uint8_t *frame, uint32_t len,
                            struct net_ping_wait *ping_wait,
                            struct net_udp_wait *udp_wait,
                            struct net_tcp_wait *tcp_wait)
{
    const uint8_t *ip;
    const uint8_t *icmp;
    uint32_t ihl;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t total_len;
    uint16_t icmp_len;
    if (len < 34) {
        return;
    }
    ip = frame + 14;
    if ((ip[0] >> 4) != 4) {
        return;
    }
    ihl = (uint32_t)(ip[0] & 0x0fu) * 4u;
    if (ihl < 20 || len < 14u + ihl) {
        return;
    }
    total_len = net_get_u16(ip + 2);
    if (total_len < ihl || 14u + total_len > len) {
        return;
    }
    /* Fragment reassembly is not implemented; fragments must never be
     * interpreted as independent transport packets. */
    if (net_checksum(ip, ihl) || (net_get_u16(ip + 6) & 0x3fffu)) return;
    dst_ip = net_get_u32(ip + 16);
    if (!net_ip_for_us(dst_ip)) {
        return;
    }
    src_ip = net_get_u32(ip + 12);
    if (ip[9] == IPV4_PROTO_UDP) {
        net_handle_udp(ip, total_len, ihl, src_ip, udp_wait);
        return;
    }
    if (ip[9] == IPV4_PROTO_TCP) {
        net_handle_tcp(ip, total_len, ihl, src_ip, tcp_wait);
        return;
    }
    if (ip[9] != IPV4_PROTO_ICMP || total_len < ihl + 8u) {
        return;
    }
    icmp = ip + ihl;
    icmp_len = (uint16_t)(total_len - ihl);
    if (icmp_len < 8) {
        return;
    }
    if (icmp[0] == ICMP_ECHO_REPLY && ping_wait &&
        src_ip == ping_wait->target_ip &&
        net_get_u16(icmp + 4) == ping_wait->ident &&
        net_get_u16(icmp + 6) == ping_wait->sequence) {
        ping_wait->done = 1;
    }
}

/**
 * @brief Send raw IPv4 using the configured route and Ethernet transport.
 * @param destination Host-order destination IPv4 address.
 * @param source Host-order bound source, zero selects the interface address.
 * @param protocol IPv4 protocol used when constructing a header.
 * @param header_included True when data already contains an IPv4 header.
 * @param data Borrowed validated message payload under the execution lock.
 * @param length Payload bytes.
 * @return Bytes accepted or negative errno; no fragmentation is implemented.
 */
int net_ipv4_send_raw(uint32_t destination, uint32_t source, uint8_t protocol,
                      bool header_included, const void *data, uint32_t length)
{
    uint32_t header = header_included ? 0 : 20;
    if (length > 1500 - header) return -LINUX_EMSGSIZE;
    if (header_included && (length < 20 || ((const uint8_t *)data)[0] >> 4 != 4)) return -LINUX_EINVAL;
    uint8_t frame[1514] = {0};
    uint8_t *ip = frame + 14;
    if (!source) source = net_config.local_ip;
    if (header_included) {
        net_memcpy(ip, data, length);
        uint32_t ihl = (ip[0] & 15) * 4;
        if (ihl < 20 || ihl > length) return -LINUX_EINVAL;
        net_put_u16(ip + 2, (uint16_t)length);
        if (!net_get_u32(ip + 12)) net_put_u32(ip + 12, source);
        if (!net_get_u16(ip + 4)) net_put_u16(ip + 4, ++net_packet_ip_id);
        net_put_u16(ip + 10, 0);
        net_put_u16(ip + 10, net_checksum(ip, ihl));
    } else {
        ip[0] = 0x45; ip[8] = 64; ip[9] = protocol;
        net_put_u16(ip + 2, (uint16_t)(length + 20));
        net_put_u16(ip + 4, ++net_packet_ip_id);
        net_put_u32(ip + 12, source); net_put_u32(ip + 16, destination);
        net_put_u16(ip + 10, net_checksum(ip, 20));
        net_memcpy(ip + 20, data, length);
    }
    if (destination == UINT32_MAX) net_memcpy(frame, net_broadcast_mac, 6);
    else if (net_resolve_mac(net_route_arp_ip(destination), 1000, frame) < 0) return -LINUX_EHOSTUNREACH;
    net_memcpy(frame + 6, e1000_mac(), 6);
    net_put_u16(frame + 12, 0x0800);
    int ret = net_interface_up ? e1000_send(frame, length + header + 14) : -LINUX_ENETDOWN;
    return ret < 0 ? ret : (int)length;
}
