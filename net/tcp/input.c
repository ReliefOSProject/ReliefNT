#include "../internal.h"

/**
 * @brief Find a registered TCP socket by its peer and local port tuple.
 * @param src_ip Peer IPv4 source address in host byte order.
 * @param src_port Peer source port.
 * @param dst_port Local destination port.
 * @return Matching live socket, or null when the tuple is not registered.
 */
static struct net_socket *net_socket_match(uint32_t src_ip, uint16_t src_port,
                                           uint16_t dst_port);

/**
 * @brief Route a validated TCP segment to its stable socket and update protocol state.
 * @param src_ip Peer IPv4 source address in host byte order.
 * @param src_port Peer source port.
 * @param dst_port Local destination port.
 * @param seq Incoming sequence number.
 * @param ack Peer cumulative acknowledgment number.
 * @param window Peer-advertised receive window in bytes.
 * @param flags TCP control flags.
 * @param payload Segment payload bytes, or null for an empty segment.
 * @param payload_len Payload length in bytes.
 * @note Accepted data and FINs wake blocked readers and notify poll readiness.
 */
static void net_socket_handle_tcp(uint32_t src_ip, uint16_t src_port,
                                  uint16_t dst_port, uint32_t seq,
                                  uint32_t ack, uint16_t window,
                                  uint8_t flags,
                                  const uint8_t *payload,
                                  uint32_t payload_len);


/**
 * @brief Compare TCP sequence numbers using wrap-safe signed subtraction.
 * @param a First sequence number.
 * @param b Second sequence number.
 * @return Nonzero when @p a is equal to or after @p b in TCP sequence space.
 */
int net_tcp_seq_after_or_equal(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}

/**
 * @brief Check whether an incoming segment intersects the current receive window.
 * @param socket Stable TCP socket whose receive sequence and advertised window apply.
 * @param seq Segment sequence number.
 * @param payload_len Segment payload byte count.
 * @param flags Segment control flags; SYN and FIN each consume one sequence byte.
 * @return True when the segment satisfies TCP receive-window acceptability.
 * @note Called under the network execution transaction before ACK or window state changes.
 */
static bool net_tcp_segment_acceptable(const struct net_socket *socket,
                                       uint32_t seq, uint32_t payload_len,
                                       uint8_t flags)
{
    uint32_t window;
    uint32_t segment_len = payload_len;
    uint32_t first_offset;
    uint32_t last_offset;
    if (!socket) return false;
    if (flags & TCP_FLAG_SYN) ++segment_len;
    if (flags & TCP_FLAG_FIN) ++segment_len;
    window = socket->rx_window_valid
        ? net_socket_receive_window(socket)
        : NET_SOCKET_RX_CAP - socket->rx_len;
    first_offset = seq - socket->remote_seq;
    if (!segment_len) return window ? first_offset < window
                                    : seq == socket->remote_seq;
    if (!window) return false;
    last_offset = first_offset + segment_len - 1u;
    return first_offset < window || last_offset < window;
}

/**
 * @brief Validate and dispatch one IPv4 TCP segment, including its peer window.
 * @param ip Complete IPv4 packet containing the TCP segment.
 * @param total_len Packet length in bytes.
 * @param ihl IPv4 header length in bytes.
 * @param src_ip Source IPv4 address in host byte order.
 * @param tcp_wait Optional synchronous legacy TCP exchange waiter.
 * @note Socket dispatch is serialized by the network execution transaction.
 */
void net_handle_tcp(const uint8_t *ip, uint32_t total_len,
                           uint32_t ihl, uint32_t src_ip,
                           struct net_tcp_wait *tcp_wait)
{
    const uint8_t *tcp;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint16_t window;
    uint32_t tcp_len;
    uint32_t hdr_len;
    uint32_t payload_len;
    uint8_t flags;
    if (total_len < ihl + 20u) {
        return;
    }
    tcp = ip + ihl;
    tcp_len = total_len - ihl;
    if (net_tcp_checksum(src_ip, net_get_u32(ip + 16), tcp, tcp_len)) return;
    src_port = net_get_u16(tcp);
    dst_port = net_get_u16(tcp + 2);
    hdr_len = (uint32_t)(tcp[12] >> 4) * 4u;
    if (hdr_len < 20u || hdr_len > tcp_len) {
        return;
    }
    flags = tcp[13];
    seq = net_get_u32(tcp + 4);
    ack = net_get_u32(tcp + 8);
    window = net_get_u16(tcp + 14);
    payload_len = tcp_len - hdr_len;
    net_socket_handle_tcp(src_ip, src_port, dst_port, seq, ack, window, flags,
                          tcp + hdr_len, payload_len);
    if (!tcp_wait) {
        return;
    }
    if (tcp_wait->src_port && src_port != tcp_wait->src_port) {
        return;
    }
    if (tcp_wait->dst_port && dst_port != tcp_wait->dst_port) {
        return;
    }
    if (tcp_wait->src_ip && src_ip != tcp_wait->src_ip) {
        return;
    }
    tcp_wait->flags |= flags;
    if (flags & TCP_FLAG_ACK) {
        if (!tcp_wait->acked_seq ||
            net_tcp_seq_after_or_equal(ack, tcp_wait->acked_seq)) {
            tcp_wait->acked_seq = ack;
        }
    }
    if (flags & TCP_FLAG_RST) {
        tcp_wait->reset = 1;
        ++tcp_wait->changed;
        return;
    }
    if (flags & TCP_FLAG_SYN) {
        tcp_wait->remote_seq = seq + 1u;
        ++tcp_wait->changed;
    }
    if (payload_len) {
        if (tcp_wait->remote_seq == 0 || seq == tcp_wait->remote_seq) {
            uint32_t copy_len = payload_len;
            if (copy_len > tcp_wait->capacity - tcp_wait->length) {
                copy_len = tcp_wait->capacity - tcp_wait->length;
                tcp_wait->overflow = 1;
            }
            if (copy_len && tcp_wait->payload) {
                net_memcpy(tcp_wait->payload + tcp_wait->length,
                           tcp + hdr_len, copy_len);
                tcp_wait->length += copy_len;
            }
            tcp_wait->remote_seq = seq + payload_len;
            ++tcp_wait->changed;
        } else if (seq + payload_len > tcp_wait->remote_seq) {
            tcp_wait->overflow = 1;
        }
    }
    if (flags & TCP_FLAG_FIN) {
        if (tcp_wait->remote_seq == 0 ||
            seq + payload_len == tcp_wait->remote_seq) {
            ++tcp_wait->remote_seq;
            tcp_wait->fin = 1;
            ++tcp_wait->changed;
        }
    }
}

/**
 * @brief Find a registered TCP socket by its peer and local port tuple.
 * @param src_ip Peer IPv4 source address in host byte order.
 * @param src_port Peer source port.
 * @param dst_port Local destination port.
 * @return Matching live socket, or null when the tuple is not registered.
 */
static struct net_socket *net_socket_match(uint32_t src_ip, uint16_t src_port,
                                           uint16_t dst_port)
{
    for (struct net_socket *socket = net_sockets; socket;
         socket = socket->registry_next) {
        if (!socket->used || socket->state == RELIEFOS_NET_TCP_CLOSED) {
            continue;
        }
        if (socket->remote_ip == src_ip &&
            socket->remote_port == src_port &&
            socket->local_port == dst_port) {
            return socket;
        }
    }
    return 0;
}

/**
 * @brief Route a validated TCP segment to its stable socket and update protocol state.
 * @param src_ip Peer IPv4 source address in host byte order.
 * @param src_port Peer source port.
 * @param dst_port Local destination port.
 * @param seq Incoming sequence number.
 * @param ack Peer cumulative acknowledgment number.
 * @param window Peer-advertised receive window in bytes.
 * @param flags TCP control flags.
 * @param payload Segment payload bytes, or null for an empty segment.
 * @param payload_len Payload length in bytes.
 * @note Accepted data and FINs wake blocked readers and notify poll readiness.
 */
static void net_socket_handle_tcp(uint32_t src_ip, uint16_t src_port,
                                  uint16_t dst_port, uint32_t seq,
                                  uint32_t ack, uint16_t window,
                                  uint8_t flags,
                                  const uint8_t *payload,
                                  uint32_t payload_len)
{
    struct net_socket *socket = net_socket_match(src_ip, src_port, dst_port);
    if (!socket) {
        return;
    }
    if (net_socket_trace_tls(socket)) {
        console_debug("[net] tls rx socket=%d state=%u flags=0x%x seq=%u ack=%u payload=%u expected=%u\n",
                       socket->handle, socket->state, flags, seq, ack,
                       payload_len, socket->remote_seq);
    }
    if (socket->state == RELIEFOS_NET_TCP_SYN_SENT) {
        if (flags & TCP_FLAG_RST) {
            socket->error = LINUX_ECONNREFUSED;
            net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_TCP_RESET);
            return;
        }
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) ==
                (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
            ack == socket->local_seq) {
            net_socket_acknowledge(socket, ack);
            if (socket->acked_seq != socket->local_seq) return;
            socket->remote_seq = seq + 1u;
            socket->peer_window = window;
            socket->peer_window_seq = seq;
            socket->peer_window_ack = ack;
            socket->peer_window_valid = true;
            socket->tx_cwnd = 3u * NET_TCP_MSS;
            socket->tx_ssthresh = NET_SOCKET_TX_CAP;
            socket->tx_rto_ms = NET_TCP_INITIAL_RTO_MS;
            socket->tx_retries = 0;
            socket->state = RELIEFOS_NET_TCP_ESTABLISHED;
            socket->status = RELIEFOS_NET_STATUS_OK;
            socket->syn_deadline = 0;
            net_socket_touch(socket);
            if (net_socket_trace_tls(socket)) {
                console_debug("[net] tls connected socket=%d local=%u remote=%u\n",
                               socket->handle, socket->local_port,
                               socket->remote_port);
            }
            (void)net_send_tcp_to_mac(socket->dst_mac, net_config.local_ip,
                                      socket->remote_ip, socket->local_port,
                                      socket->remote_port, socket->local_seq,
                                      socket->remote_seq, TCP_FLAG_ACK, 0, 0);
        }
        return;
    }
    if (socket->state != RELIEFOS_NET_TCP_ESTABLISHED &&
        socket->state != RELIEFOS_NET_TCP_TIME_WAIT) {
        return;
    }
    bool sequence_acceptable = net_tcp_segment_acceptable(socket, seq,
                                                           payload_len, flags);
    if (!sequence_acceptable) return;
    if (flags & TCP_FLAG_RST) {
        socket->error = LINUX_ECONNRESET;
        net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_TCP_RESET);
        return;
    }
    if (flags & TCP_FLAG_ACK) net_socket_acknowledge(socket, ack);
    if (sequence_acceptable && net_tcp_seq_after_or_equal(socket->local_seq, ack) &&
        net_tcp_seq_after_or_equal(ack, socket->acked_seq) &&
        (!socket->peer_window_valid ||
         (net_tcp_seq_after_or_equal(seq, socket->peer_window_seq) &&
          net_tcp_seq_after_or_equal(ack, socket->peer_window_ack)))) {
        socket->peer_window = window;
        socket->peer_window_seq = seq;
        socket->peer_window_ack = ack;
        socket->peer_window_valid = true;
        if (window) {
            socket->tx_persist_deadline = 0;
            socket->tx_persist_probe = false;
        }
    }
    if (payload_len) {
        if (net_tcp_seq_after_or_equal(socket->remote_seq, seq) && seq != socket->remote_seq) {
            uint32_t duplicate = socket->remote_seq - seq;
            if (duplicate > payload_len) duplicate = payload_len;
            payload += duplicate;
            payload_len -= duplicate;
            seq += duplicate;
        }
        if (seq == socket->remote_seq) {
            uint32_t free_bytes = socket->shutdown_read ? payload_len : NET_SOCKET_RX_CAP - socket->rx_len;
            uint32_t copy_len = payload_len;
            uint32_t overflow = 0;
            if (copy_len > free_bytes) {
                copy_len = free_bytes;
                overflow = 1;
            }
            if (copy_len && !socket->shutdown_read) {
                uint32_t tail = (socket->rx_head + socket->rx_len) % NET_SOCKET_RX_CAP;
                uint32_t first = NET_SOCKET_RX_CAP - tail;
                if (first > copy_len) first = copy_len;
                net_memcpy(socket->rx + tail, payload, first);
                net_memcpy(socket->rx, payload + first, copy_len - first);
                socket->rx_len += copy_len;
                socket->rx_bytes += copy_len;
                (void)kernel_wait_queue_wake_all(&socket->wait_read);
                kernel_poll_readiness_notify();
            }
            /* Acknowledge only bytes retained. The sender retransmits the
             * suffix once the application opens the receive window again. */
            socket->remote_seq += copy_len;
            socket->status = RELIEFOS_NET_STATUS_OK;
            net_socket_touch(socket);
            if (net_socket_trace_tls(socket)) {
                console_debug("[net] tls rx accepted socket=%d bytes=%u queued=%u next=%u overflow=%u\n",
                               socket->handle, copy_len, socket->rx_len,
                               socket->remote_seq, overflow);
            }
        } else if (net_socket_trace_tls(socket)) {
            console_debug("[net] tls rx ignored socket=%d seq=%u expected=%u bytes=%u\n",
                           socket->handle, seq, socket->remote_seq, payload_len);
        }
        (void)net_send_tcp_to_mac(socket->dst_mac, net_config.local_ip,
                                  socket->remote_ip, socket->local_port,
                                  socket->remote_port, socket->local_seq,
                                  socket->remote_seq, TCP_FLAG_ACK, 0, 0);
    }
    if (flags & TCP_FLAG_FIN) {
        if (seq + payload_len == socket->remote_seq) {
            ++socket->remote_seq;
            socket->fin_received = 1;
            socket->state = RELIEFOS_NET_TCP_TIME_WAIT;
            socket->status = RELIEFOS_NET_STATUS_OK;
            net_socket_touch(socket);
            (void)kernel_wait_queue_wake_all(&socket->wait_read);
            kernel_poll_readiness_notify();
            /* Acknowledge the peer FIN after advancing the receive sequence. */
            (void)net_send_tcp_to_mac(socket->dst_mac, net_config.local_ip,
                                      socket->remote_ip, socket->local_port,
                                      socket->remote_port, socket->local_seq,
                                      socket->remote_seq, TCP_FLAG_ACK, 0, 0);
        }
    }
}
