#include "../internal.h"

/**
 * Net socket match.
 * @param src_ip Value supplied by the caller.
 * @param src_port Value supplied by the caller.
 * @param dst_port Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static struct net_socket *net_socket_match(uint32_t src_ip, uint16_t src_port,
                                           uint16_t dst_port);

/**
 * Net socket handle tcp.
 * @param src_ip Value supplied by the caller.
 * @param src_port Value supplied by the caller.
 * @param dst_port Value supplied by the caller.
 * @param seq Value supplied by the caller.
 * @param ack Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @param payload Value supplied by the caller.
 * @param payload_len Value supplied by the caller.
 */
static void net_socket_handle_tcp(uint32_t src_ip, uint16_t src_port,
                                  uint16_t dst_port, uint32_t seq,
                                  uint32_t ack, uint8_t flags,
                                  const uint8_t *payload,
                                  uint32_t payload_len);


/**
 * Net tcp seq after or equal.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_tcp_seq_after_or_equal(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}

/**
 * Net handle tcp.
 * @param ip Value supplied by the caller.
 * @param total_len Value supplied by the caller.
 * @param ihl Value supplied by the caller.
 * @param src_ip Value supplied by the caller.
 * @param tcp_wait Value supplied by the caller.
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
    payload_len = tcp_len - hdr_len;
    net_socket_handle_tcp(src_ip, src_port, dst_port, seq, ack, flags,
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
 * Net socket match.
 * @param src_ip Value supplied by the caller.
 * @param src_port Value supplied by the caller.
 * @param dst_port Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static struct net_socket *net_socket_match(uint32_t src_ip, uint16_t src_port,
                                           uint16_t dst_port)
{
    for (uint32_t i = 0; i < RELIEFOS_NET_SOCKET_MAX; ++i) {
        struct net_socket *socket = &net_sockets[i];
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
 * Net socket handle tcp.
 * @param src_ip Value supplied by the caller.
 * @param src_port Value supplied by the caller.
 * @param dst_port Value supplied by the caller.
 * @param seq Value supplied by the caller.
 * @param ack Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @param payload Value supplied by the caller.
 * @param payload_len Value supplied by the caller.
 */
static void net_socket_handle_tcp(uint32_t src_ip, uint16_t src_port,
                                  uint16_t dst_port, uint32_t seq,
                                  uint32_t ack, uint8_t flags,
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
    if (flags & TCP_FLAG_ACK) {
        if (!socket->acked_seq ||
            net_tcp_seq_after_or_equal(ack, socket->acked_seq)) {
            socket->acked_seq = ack;
        }
    }
    if (flags & TCP_FLAG_RST) {
        socket->error = socket->state == RELIEFOS_NET_TCP_SYN_SENT ? LINUX_ECONNREFUSED : LINUX_ECONNRESET;
        net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_TCP_RESET);
        return;
    }
    if (socket->state == RELIEFOS_NET_TCP_SYN_SENT) {
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) ==
                (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
            socket->acked_seq == socket->local_seq) {
            socket->remote_seq = seq + 1u;
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
            /**
 * Net dhcp add option.
 * @param dst_mac Value supplied by the caller.
 * @param local_ip Value supplied by the caller.
 * @param remote_ip Value supplied by the caller.
 * @param local_port Value supplied by the caller.
 * @param remote_port Value supplied by the caller.
 * @param local_seq Value supplied by the caller.
 * @param remote_seq Value supplied by the caller.
 * @param TCP_FLAG_ACK Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
            (void)net_send_tcp_to_mac(socket->dst_mac, net_config.local_ip,
                                      socket->remote_ip, socket->local_port,
                                      socket->remote_port, socket->local_seq,
                                      socket->remote_seq, TCP_FLAG_ACK, 0, 0);
        }
    }
}
