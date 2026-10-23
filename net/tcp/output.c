#include "../internal.h"



/**
 * Net tcp checksum.
 * @param src_ip Value supplied by the caller.
 * @param dst_ip Value supplied by the caller.
 * @param tcp Value supplied by the caller.
 * @param tcp_len Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint16_t net_tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                                 const uint8_t *tcp, uint16_t tcp_len)
{
    uint8_t pseudo[12];
    uint32_t sum = 0;
    net_put_u32(pseudo, src_ip);
    net_put_u32(pseudo + 4, dst_ip);
    pseudo[8] = 0;
    pseudo[9] = IPV4_PROTO_TCP;
    net_put_u16(pseudo + 10, tcp_len);
    sum = net_checksum_partial(sum, pseudo, sizeof(pseudo));
    sum = net_checksum_partial(sum, tcp, tcp_len);
    return net_checksum_finish(sum);
}

uint32_t net_socket_receive_window(const struct net_socket *socket)
{
    if (!socket->rx_window_valid) return 0;
    uint32_t consumed = socket->remote_seq - socket->rx_window_ack;
    return consumed < socket->rx_window ? socket->rx_window - consumed : 0;
}

uint32_t net_socket_select_window(const struct net_socket *socket)
{
    uint32_t free_bytes = NET_SOCKET_RX_CAP - socket->rx_len;
    uint32_t window = free_bytes - free_bytes % NET_TCP_MSS;
    uint32_t remaining = net_socket_receive_window(socket);
    /* As in Linux tcp_select_window(), do not retract an offered right edge.
     * Only extend it in full segments to avoid receiver silly-window syndrome. */
    return window > remaining ? window : remaining;
}

/**
 * @brief Build and transmit one TCP segment with a current local receive window.
 * @param dst_mac Ethernet destination address.
 * @param src_ip Host-order local IPv4 source address.
 * @param dst_ip Host-order peer IPv4 destination address.
 * @param src_port Local TCP port.
 * @param dst_port Peer TCP port.
 * @param seq TCP sequence number.
 * @param ack TCP acknowledgment number.
 * @param flags TCP control flags.
 * @param payload Segment payload, or null for no payload.
 * @param payload_len Payload byte count.
 * @return Driver result, or a negative error when the frame cannot be sent.
 * @note The receiver window is read from the matching stable socket under the
 * network execution transaction, then published only after successful transmit.
 */
int net_send_tcp_to_mac(const uint8_t *dst_mac, uint32_t src_ip,
                               uint32_t dst_ip, uint16_t src_port,
                               uint16_t dst_port, uint32_t seq,
                               uint32_t ack, uint8_t flags,
                               const uint8_t *payload,
                               uint32_t payload_len)
{
    uint8_t frame[NET_FRAME_MAX];
    uint8_t *ip = frame + 14;
    uint8_t *tcp = frame + 34;
    uint32_t tcp_header_len = (flags & TCP_FLAG_SYN) ? 24u : 20u;
    uint16_t tcp_len;
    uint16_t total_len;
    if (payload_len > NET_FRAME_MAX - 34u - tcp_header_len ||
        (payload_len && !payload)) {
        return -1;
    }
    tcp_len = (uint16_t)(tcp_header_len + payload_len);
    total_len = (uint16_t)(20u + tcp_len);
    net_memzero(frame, 34u + tcp_header_len + payload_len);
    net_write_eth(frame, dst_mac, ETH_TYPE_IPV4);
    ip[0] = 0x45;
    ip[1] = 0;
    net_put_u16(ip + 2, total_len);
    net_put_u16(ip + 4, net_ipv4_id++);
    net_put_u16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = IPV4_PROTO_TCP;
    net_put_u32(ip + 12, src_ip);
    net_put_u32(ip + 16, dst_ip);
    net_put_u16(ip + 10, net_checksum(ip, 20));

    net_put_u16(tcp, src_port);
    net_put_u16(tcp + 2, dst_port);
    net_put_u32(tcp + 4, seq);
    net_put_u32(tcp + 8, ack);
    tcp[12] = (uint8_t)((tcp_header_len / 4u) << 4);
    tcp[13] = flags;
    uint32_t window = 8192u;
    struct net_socket *receiver = 0;
    for (struct net_socket *s = net_sockets; s; s = s->registry_next) {
        if (s->used && s->local_port == src_port && s->remote_port == dst_port &&
            s->remote_ip == dst_ip) {
            receiver = s;
            window = net_socket_select_window(s);
            break;
        }
    }
    net_put_u16(tcp + 14, (uint16_t)window);
    net_put_u16(tcp + 16, 0);
    net_put_u16(tcp + 18, 0);
    if (flags & TCP_FLAG_SYN) {
        tcp[20] = 2;
        tcp[21] = 4;
        net_put_u16(tcp + 22, NET_TCP_MSS);
    }
    if (payload_len) {
        net_memcpy(tcp + tcp_header_len, payload, payload_len);
    }
    net_put_u16(tcp + 16, net_tcp_checksum(src_ip, dst_ip, tcp, tcp_len));
    int result = net_interface_up ? e1000_send(frame, 14u + total_len) : -LINUX_ENETDOWN;
    if (result >= 0 && receiver && (flags & TCP_FLAG_ACK)) {
        receiver->rx_window_ack = ack;
        receiver->rx_window = window;
        receiver->rx_window_valid = true;
    }
    return result;
}
