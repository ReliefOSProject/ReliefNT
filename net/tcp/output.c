#include "../internal.h"

struct tcp_loop_frame { uint32_t length, ifindex; uint8_t bytes[NET_FRAME_MAX]; };
static struct tcp_loop_frame tcp_loop_frames[32];
static uint32_t tcp_loop_head, tcp_loop_count;

/** @brief Report pending task-context IPv4 loopback delivery.
 * @return True while at least one complete TCP frame is queued. */
bool net_tcp_loop_pending(void) { return tcp_loop_count != 0; }

/** @brief Deliver a bounded batch of deferred IPv4 TCP loopback frames.
 * Frames are copied before dispatch so a reply may safely reuse a ring slot. */
void net_tcp_loop_service(void)
{
    for (unsigned n = 0; n < 16 && tcp_loop_count; ++n) {
        struct tcp_loop_frame frame = tcp_loop_frames[tcp_loop_head];
        tcp_loop_head = (tcp_loop_head + 1u) % 32u; --tcp_loop_count;
        const uint8_t *ip = frame.bytes + 14;
        if (net_interface_exists(frame.ifindex) && net_interface_is_up(frame.ifindex))
            net_handle_tcp_on(frame.ifindex, ip, frame.length - 14u, 20u,
                              net_get_u32(ip + 12), NULL, NULL);
    }
}


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
static int net_send_tcp_to_mac_on(uint32_t ifindex, const uint8_t *dst_mac, uint32_t src_ip,
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
    net_write_eth_on(ifindex, frame, dst_mac, ETH_TYPE_IPV4);
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
            s->remote_ip == dst_ip && s->local_ip == src_ip) {
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
    int result;
    if ((dst_ip >> 24) == 127 || (dst_ip && net_interface_for_ipv4(dst_ip))) {
        if (tcp_loop_count == 32) return -LINUX_ENOBUFS;
        struct tcp_loop_frame *queued = &tcp_loop_frames[(tcp_loop_head + tcp_loop_count) % 32u];
        queued->length = 14u + total_len;
        queued->ifindex = ifindex;
        net_memcpy(queued->bytes, frame, queued->length);
        ++tcp_loop_count; result = (int)queued->length;
    } else result = net_interface_is_ready(ifindex) ? netdev_send(ifindex, frame, 14u + total_len) : -LINUX_ENETDOWN;
    if (result >= 0 && receiver && (flags & TCP_FLAG_ACK)) {
        receiver->rx_window_ack = ack;
        receiver->rx_window = window;
        receiver->rx_window_valid = true;
    }
    return result;
}

/** @brief Return a TCP segment payload limit bounded by peer MSS and IPv6 path MTU.
 * @param socket Registered socket. @return Positive payload size no larger than the IPv4 default. */
uint32_t net_socket_mss(const struct net_socket *socket)
{
    uint32_t mss = NET_TCP_MSS;
    if (socket->ipv6 && !socket->mapped) {
        uint32_t mtu = socket->mtu6 ? socket->mtu6 : 1280;
        mss = mtu > 60 ? mtu - 60 : 1;
        if (mss > NET_TCP_MSS) mss = NET_TCP_MSS;
    }
    if (socket->peer_mss && socket->peer_mss < mss) mss = socket->peer_mss;
    return mss;
}

/** @brief Emit one TCP segment for a stable socket, using IPv6 pseudo-header checksums.
 * @param socket Endpoint under execution ownership. @param seq Sequence number.
 * @param ack Acknowledgment. @param flags TCP control flags. @param payload Optional data.
 * @param length Payload bytes. @return Lower-layer result or negative errno. */
int net_send_tcp_socket(struct net_socket *socket, uint32_t seq, uint32_t ack,
                        uint8_t flags, const uint8_t *payload, uint32_t length)
{
    if (socket->resolving_neighbor) return -LINUX_EAGAIN;
    if (!socket->ipv6 || socket->mapped) {
        bool local = net_interface_for_ipv4(socket->remote_ip) != 0;
        if (!local) {
            struct net_ipv4_path path;
            int result = net_ipv4_route_lookup(socket->remote_ip, socket->local_ip,
                                               socket->bound_ifindex, &path);
            if (result < 0) return result;
            if (socket->ifindex != path.ifindex || socket->next_hop != path.next_hop ||
                net_mac_is_zero(socket->dst_mac)) {
                socket->resolving_neighbor = true;
                result = net_resolve_mac_on(path.ifindex, path.next_hop, 1000, socket->dst_mac);
                socket->resolving_neighbor = false;
                if (result < 0) return result;
            }
            socket->ifindex = path.ifindex;
            socket->next_hop = path.next_hop;
        }
        return net_send_tcp_to_mac_on(local ? net_interface_for_ipv4(socket->remote_ip) : socket->ifindex, socket->dst_mac,
            socket->local_ip, socket->remote_ip, socket->local_port, socket->remote_port,
            seq, ack, flags, payload, length);
    }
    if (socket->remote_scope6 && socket->bound_ifindex && socket->remote_scope6 != socket->bound_ifindex)
        return -LINUX_EINVAL;
    struct net_ipv6_path path;
    int ret = net_ipv6_route(&socket->remote6, socket->remote_scope6 ? socket->remote_scope6 : socket->bound_ifindex, &socket->local6, &path);
    if (ret < 0) return ret;
    socket->mtu6 = path.mtu;
    socket->ifindex = path.ifindex;
    uint32_t header = (flags & TCP_FLAG_SYN) ? 24u : 20u;
    if (length > net_socket_mss(socket) || (length && !payload)) return -LINUX_EMSGSIZE;
    uint8_t tcp[NET_TCP_MSS + 24];
    net_memzero(tcp, header + length);
    net_put_u16(tcp, socket->local_port); net_put_u16(tcp + 2, socket->remote_port);
    net_put_u32(tcp + 4, seq); net_put_u32(tcp + 8, ack);
    tcp[12] = (uint8_t)((header / 4u) << 4); tcp[13] = flags;
    uint32_t window = net_socket_select_window(socket);
    net_put_u16(tcp + 14, (uint16_t)window);
    if (flags & TCP_FLAG_SYN) {
        tcp[20] = 2; tcp[21] = 4;
        uint32_t local_mss = path.mtu > 60 ? path.mtu - 60 : 1;
        if (local_mss > NET_TCP_MSS) local_mss = NET_TCP_MSS;
        net_put_u16(tcp + 22, (uint16_t)local_mss);
    }
    if (length) net_memcpy(tcp + header, payload, length);
    net_put_u16(tcp + 16, net_ipv6_checksum(&path.source, &path.destination, 6, tcp, header + length));
    ret = net_ipv6_output(&path, 6, tcp, header + length, socket->unicast_hops, 0,
                          socket->dont_fragment || socket->mtu_discover == IPV6_PMTUDISC_DO, true);
    if (ret >= 0 && (flags & TCP_FLAG_ACK)) {
        socket->rx_window_ack = ack; socket->rx_window = window; socket->rx_window_valid = true;
    }
    return ret;
}

/** @brief Send a legacy TCP segment with normal route selection.
 * @param mac Peer MAC. @param source Local IP. @param destination Peer IP.
 * @param source_port Local port. @param destination_port Peer port.
 * @param seq Sequence. @param ack Acknowledgment. @param flags Control bits.
 * @param data Payload. @param length Bytes. @return Driver result or negative errno. */
int net_send_tcp_to_mac(const uint8_t *mac, uint32_t source, uint32_t destination,
    uint16_t source_port, uint16_t destination_port, uint32_t seq, uint32_t ack,
    uint8_t flags, const uint8_t *data, uint32_t length)
{
    struct net_ipv4_path path;
    int result = net_ipv4_route_lookup(destination, source, 0, &path);
    if (result < 0) return result;
    return net_send_tcp_to_mac_on(path.ifindex, mac, source, destination, source_port,
                                   destination_port, seq, ack, flags, data, length);
}
