#include "../internal.h"

#define NET_TCP_SOCKET_SERVICE_BUDGET 4u

/**
 * Net now32.
 * @return The value or status produced by the operation.
 */
static uint32_t net_now32(void);

/**
 * @brief Service one queued TCP segment, retransmission, persist probe, or FIN.
 * @param socket Stable registered socket to service.
 * @note Called under the execution transaction from task context; accepted
 *       transmit-ring bytes survive this call.
 */
static void net_socket_service(struct net_socket *socket);

/**
 * Net socket clear.
 * @param socket Value supplied by the caller.
 */
static void net_socket_clear(struct net_socket *socket);

/**
 * Net socket gc.
 */
static void net_socket_gc(void);

/**
 * Net socket find.
 * @param handle Value supplied by the caller.
 * @param owner_pid Value supplied by the caller.
 * @param allow_closed Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static struct net_socket *net_socket_find(int32_t handle, uint32_t owner_pid,
                                          int allow_closed);

/**
 * Net socket alloc.
 * @param owner_pid Value supplied by the caller.
 * @param owner_uid Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static struct net_socket *net_socket_alloc(uint32_t owner_pid, uint32_t owner_uid);

/**
 * Net socket clamp timeout.
 * @param timeout_ms Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_socket_clamp_timeout(uint32_t timeout_ms);

/**
 * @brief Start a TCP handshake for one resolved IPv4 address.
 * @param socket Stable registered socket object.
 * @param remote_ip Peer IPv4 address in host byte order.
 * @param remote_port Peer TCP port.
 * @param timeout_ms Explicit connection timeout in milliseconds.
 * @param asynchronous Return after sending SYN when true, otherwise poll to completion.
 * @return ReliefOS network status; asynchronous starts return TCP_TIMEOUT while SYN_SENT.
 * @note Resets per-connection sequence, window, queue, retry, and error state before SYN.
 */
static uint32_t net_socket_connect_ip(struct net_socket *socket,
                                      uint32_t remote_ip, uint16_t remote_port,
                                      uint32_t timeout_ms, bool asynchronous);

/**
 * Net connection visible.
 * @param socket Value supplied by the caller.
 * @param viewer Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_connection_visible(const struct net_socket *socket,
                                  const struct task *viewer);

static int32_t net_next_socket_handle = 1;
static struct net_socket *net_tcp_work_head;
static struct net_socket *net_tcp_work_tail;
static uint32_t net_tcp_work_count;

/**
 * @brief Check whether the local TCP write half remains open.
 * @param socket Stable socket whose half-close flags are inspected.
 * @return True while established or after peer FIN, until local shutdown/FIN.
 * @note Legacy UAPI exposes peer CLOSE_WAIT through TIME_WAIT; FIN flags retain
 *       the independent read and write state without changing public numbers.
 */
static bool net_socket_write_open(const struct net_socket *socket)
{
    if (!socket || socket->shutdown_write || socket->fin_sent) return false;
    return socket->state == RELIEFOS_NET_TCP_ESTABLISHED ||
        (socket->state == RELIEFOS_NET_TCP_TIME_WAIT && socket->fin_received);
}

/**
 * @brief Check whether a TCP socket currently needs autonomous maintenance.
 * @param socket Stable registered socket to inspect.
 * @return True for active handshakes, queued/unacknowledged data, persist work,
 *         deferred FINs, or a FIN awaiting ACK/retransmission.
 */
static bool net_socket_work_pending(const struct net_socket *socket)
{
    if (!socket || !socket->used) return false;
    if (socket->state == RELIEFOS_NET_TCP_SYN_SENT)
        return socket->syn_deadline != 0;
    return (socket->state == RELIEFOS_NET_TCP_ESTABLISHED ||
            socket->state == RELIEFOS_NET_TCP_TIME_WAIT) &&
        (socket->tx_len || socket->tx_sent ||
         (socket->fin_pending && !socket->fin_sent) ||
         (socket->fin_sent && !socket->fin_acked));
}

/**
 * @brief Remove a socket from the active TCP maintenance queue.
 * @param socket Stable socket; a socket not currently queued is unchanged.
 */
static void net_socket_work_remove(struct net_socket *socket)
{
    if (!socket || !socket->maintenance_queued) return;
    if (socket->maintenance_prev)
        socket->maintenance_prev->maintenance_next = socket->maintenance_next;
    else
        net_tcp_work_head = socket->maintenance_next;
    if (socket->maintenance_next)
        socket->maintenance_next->maintenance_prev = socket->maintenance_prev;
    else
        net_tcp_work_tail = socket->maintenance_prev;
    socket->maintenance_next = 0;
    socket->maintenance_prev = 0;
    socket->maintenance_queued = false;
    if (net_tcp_work_count) --net_tcp_work_count;
}

/**
 * @brief Add a socket to the active TCP maintenance queue when work is pending.
 * @param socket Stable registered socket whose protocol work should progress.
 */
static void net_socket_work_schedule(struct net_socket *socket)
{
    if (!net_socket_work_pending(socket) || socket->maintenance_queued) return;
    socket->maintenance_prev = net_tcp_work_tail;
    socket->maintenance_next = 0;
    if (net_tcp_work_tail)
        net_tcp_work_tail->maintenance_next = socket;
    else
        net_tcp_work_head = socket;
    net_tcp_work_tail = socket;
    socket->maintenance_queued = true;
    ++net_tcp_work_count;
}

/**
 * @brief Indicate whether the active TCP queue has protocol work.
 * @return True when a registered TCP socket is queued for maintenance.
 */
bool net_tcp_maintenance_pending(void)
{
    return net_tcp_work_count != 0;
}

/**
 * Net now32.
 * @return The value or status produced by the operation.
 */
static uint32_t net_now32(void)
{
    return (uint32_t)time_uptime_ms();
}

/**
 * @brief Unlink a socket after detaching its waiters and freeing owned buffers.
 * @param socket Stable socket object that is no longer externally reachable.
 */
static void net_socket_clear(struct net_socket *socket)
{
    if (socket) {
        net_socket_work_remove(socket);
        struct net_socket **link = &net_sockets;
        while (*link && *link != socket) link = &(*link)->registry_next;
        if (*link == socket) {
            *link = socket->registry_next;
        }
        (void)kernel_wait_queue_wake_all(&socket->wait_write);
        (void)kernel_wait_queue_wake_all(&socket->wait_read);
        kernel_free(socket->rx);
        kernel_free(socket->tx);
        kernel_free(socket);
    }
}

/**
 * @brief Refresh the last-change timestamp used by diagnostics and garbage collection.
 * @param socket Socket whose change time should be refreshed.
 */
void net_socket_touch(struct net_socket *socket)
{
    if (socket) {
        socket->changed_ms = net_now32();
    }
}

/**
 * @brief Reclaim unreferenced closed or fully acknowledged sockets after their hold interval.
 * @note A socket with an unacknowledged local FIN remains registered for retries.
 */
static void net_socket_gc(void)
{
    uint32_t now = net_now32();
    struct net_socket *socket = net_sockets;
    while (socket) {
        struct net_socket *next = socket->registry_next;
        if (!socket->used || socket->fd_owned) {
            socket = next;
            continue;
        }
        if ((socket->state == RELIEFOS_NET_TCP_CLOSED ||
             (socket->state == RELIEFOS_NET_TCP_TIME_WAIT &&
              socket->tx_len == 0 && socket->tx_sent == 0 &&
              (!socket->fin_sent || socket->fin_acked))) &&
            now - socket->changed_ms >= NET_SOCKET_CLOSE_HOLD_MS) {
            net_socket_clear(socket);
        }
        socket = next;
    }
}

/**
 * @brief Find a stable socket by handle and optional owner identity.
 * @param handle Internal socket handle.
 * @param owner_pid Required owner, or zero to match any owner.
 * @param allow_closed Whether closed sockets remain visible to the lookup.
 * @return Matching live registry object, or null.
 */
static struct net_socket *net_socket_find(int32_t handle, uint32_t owner_pid,
                                          int allow_closed)
{
    if (handle <= 0) {
        return 0;
    }
    for (struct net_socket *socket = net_sockets; socket; socket = socket->registry_next) {
        if (!socket->used || socket->handle != handle) {
            continue;
        }
        if (owner_pid && socket->owner_pid != owner_pid) {
            continue;
        }
        if (!allow_closed && socket->state == RELIEFOS_NET_TCP_CLOSED) {
            return 0;
        }
        return socket;
    }
    return 0;
}

/**
 * @brief Mark a socket closed and wake every waiter that can reference it.
 * @param socket Stable socket object to close.
 * @param status Final network status recorded for diagnostics.
 */
void net_socket_mark_closed(struct net_socket *socket, uint32_t status)
{
    if (!socket) {
        return;
    }
    socket->state = RELIEFOS_NET_TCP_CLOSED;
    socket->status = status;
    net_socket_work_remove(socket);
    net_socket_touch(socket);
    (void)kernel_wait_queue_wake_all(&socket->wait_write);
    (void)kernel_wait_queue_wake_all(&socket->wait_read);
    kernel_poll_readiness_notify();
}

/**
 * Net socket trace tls.
 * @param socket Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_socket_trace_tls(const struct net_socket *socket)
{
    return NET_TCP_TRACE && socket && socket->remote_port == 443u;
}

/**
 * @brief Allocate and register a stable TCP socket object with dynamic buffers.
 * @param owner_pid Process identifier that owns the socket.
 * @param owner_uid User identifier used for connection visibility.
 * @return New registered socket, or null when memory allocation fails.
 */
static struct net_socket *net_socket_alloc(uint32_t owner_pid, uint32_t owner_uid)
{
    struct net_socket *slot;
    int32_t handle;
    net_socket_gc();
    slot = kernel_malloc(sizeof(*slot));
    if (!slot) return 0;
    net_memzero(slot, sizeof(*slot));
    slot->rx = kernel_malloc(NET_SOCKET_RX_CAP);
    slot->tx = kernel_malloc(NET_SOCKET_TX_CAP);
    if (!slot->rx || !slot->tx) {
        kernel_free(slot->rx);
        kernel_free(slot->tx);
        kernel_free(slot);
        return 0;
    }
    kernel_wait_queue_init(&slot->wait_read);
    kernel_wait_queue_init(&slot->wait_write);
    slot->used = 1;
    for (;;) {
        handle = net_next_socket_handle;
        net_next_socket_handle = handle == 0x7fffffff ? 1 : handle + 1;
        if (handle > 0 && !net_socket_find(handle, 0, 1)) break;
    }
    slot->handle = handle;
    slot->owner_pid = owner_pid;
    slot->owner_uid = owner_uid;
    slot->state = RELIEFOS_NET_TCP_CLOSED;
    slot->status = RELIEFOS_NET_STATUS_SOCKET_CLOSED;
    slot->receive_timeout = UINT64_MAX;
    slot->send_timeout = UINT64_MAX;
    slot->tx_cwnd = 3u * NET_TCP_MSS;
    slot->tx_ssthresh = NET_SOCKET_TX_CAP;
    slot->tx_rto_ms = NET_TCP_INITIAL_RTO_MS;
    slot->created_ms = net_now32();
    slot->changed_ms = slot->created_ms;
    if (!net_sockets) net_sockets = slot;
    else {
        struct net_socket *tail = net_sockets;
        while (tail->registry_next) tail = tail->registry_next;
        tail->registry_next = slot;
    }
    return slot;
}

/**
 * @brief Apply the legacy network-control timeout default and upper bound.
 * @param timeout_ms Caller-supplied timeout in milliseconds.
 * @return Bounded nonzero timeout in milliseconds.
 */
static uint32_t net_socket_clamp_timeout(uint32_t timeout_ms)
{
    if (!timeout_ms) {
        timeout_ms = NET_SOCKET_DEFAULT_TIMEOUT_MS;
    }
    if (timeout_ms > RELIEFOS_NET_MAX_TIMEOUT_MS) {
        timeout_ms = RELIEFOS_NET_MAX_TIMEOUT_MS;
    }
    return timeout_ms;
}

/**
 * @brief Copy the current unacknowledged TCP bytes into a contiguous segment.
 * @param socket Socket whose transmit ring is read.
 * @param output Storage for at most one MSS.
 * @param offset Offset from the transmit ring's first unacknowledged byte.
 * @param length Number of bytes copied from the ring head.
 */
static void net_socket_tx_copy(const struct net_socket *socket, uint8_t *output,
                               uint32_t offset, uint32_t length)
{
    uint32_t start = (socket->tx_head + offset) % NET_SOCKET_TX_CAP;
    uint32_t first = NET_SOCKET_TX_CAP - start;
    if (first > length) first = length;
    net_memcpy(output, socket->tx + start, first);
    net_memcpy(output + first, socket->tx, length - first);
}

/**
 * @brief Advance a socket's transmit ring by a valid cumulative ACK.
 * @param socket Stable registered socket.
 * @param ack Next sequence number expected by the peer.
 */
void net_socket_acknowledge(struct net_socket *socket, uint32_t ack)
{
    uint32_t advance;
    if (!socket) return;
    if (socket->state == RELIEFOS_NET_TCP_SYN_SENT) {
        if (ack == socket->local_seq) socket->acked_seq = ack;
        return;
    }
    if (socket->state != RELIEFOS_NET_TCP_ESTABLISHED &&
        socket->state != RELIEFOS_NET_TCP_TIME_WAIT) return;
    if (!net_tcp_seq_after_or_equal(ack, socket->acked_seq)) return;
    advance = ack - socket->acked_seq;
    if (socket->fin_sent && !socket->fin_acked && !socket->tx_sent &&
        !socket->tx_len && ack == socket->local_seq &&
        socket->acked_seq == socket->local_seq - 1u) {
        socket->acked_seq = ack;
        socket->fin_acked = true;
        socket->tx_retransmit_deadline = 0;
        socket->tx_retries = 0;
        socket->tx_rto_ms = NET_TCP_INITIAL_RTO_MS;
        (void)kernel_wait_queue_wake_all(&socket->wait_write);
        kernel_poll_readiness_notify();
        net_socket_touch(socket);
        return;
    }
    if (!advance || advance > socket->tx_sent) return;

    socket->acked_seq = ack;
    socket->tx_head = (socket->tx_head + advance) % NET_SOCKET_TX_CAP;
    socket->tx_len -= advance;
    socket->tx_sent -= advance;
    if (socket->tx_cwnd < socket->tx_ssthresh) {
        uint32_t increase = advance < NET_TCP_MSS ? advance : NET_TCP_MSS;
        socket->tx_cwnd = socket->tx_cwnd > NET_SOCKET_TX_CAP - increase
            ? NET_SOCKET_TX_CAP : socket->tx_cwnd + increase;
    } else if (socket->tx_cwnd) {
        uint64_t increase = (uint64_t)NET_TCP_MSS * NET_TCP_MSS /
                            socket->tx_cwnd;
        if (!increase) increase = 1;
        socket->tx_cwnd = socket->tx_cwnd > NET_SOCKET_TX_CAP - increase
            ? NET_SOCKET_TX_CAP : socket->tx_cwnd + (uint32_t)increase;
    }
    socket->tx_retries = 0;
    socket->tx_rto_ms = NET_TCP_INITIAL_RTO_MS;
    socket->tx_retransmit_deadline = socket->tx_sent
        ? time_uptime_ms() + socket->tx_rto_ms : 0;
    if (!socket->tx_sent) socket->tx_persist_probe = false;
    (void)kernel_wait_queue_wake_all(&socket->wait_write);
    kernel_poll_readiness_notify();
    net_socket_touch(socket);
}

/**
 * @brief Service one TCP socket's pending data or close request.
 * @param socket Stable registered socket.
 */
static void net_socket_service(struct net_socket *socket)
{
    uint8_t segment[NET_TCP_MSS];
    uint32_t length;
    uint32_t sequence;
    bool retransmit;
    uint64_t now;
    uint32_t flight_limit;
    uint32_t unsent;
    uint32_t send_window;
    if (!socket || !socket->used) return;
    now = time_uptime_ms();
    if (socket->state == RELIEFOS_NET_TCP_SYN_SENT) {
        if (socket->syn_deadline && now >= socket->syn_deadline) {
            socket->error = LINUX_ETIMEDOUT;
            net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_TCP_TIMEOUT);
        } else if (socket->syn_deadline && now >= socket->syn_retransmit) {
            if (net_send_tcp_to_mac(socket->dst_mac, socket->local_ip,
                    socket->remote_ip, socket->local_port, socket->remote_port,
                    socket->local_seq - 1u, 0, TCP_FLAG_SYN, 0, 0) < 0) {
                socket->error = LINUX_ENETDOWN;
                net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_TX_FAILED);
            } else {
                socket->syn_retransmit = now + NET_TCP_SYN_RETRANSMIT_MS;
                net_socket_touch(socket);
            }
        }
        return;
    }
    if (socket->state != RELIEFOS_NET_TCP_ESTABLISHED &&
        socket->state != RELIEFOS_NET_TCP_TIME_WAIT) return;
    retransmit = false;
    if (socket->fin_sent && !socket->fin_acked) {
        if (now < socket->tx_retransmit_deadline) return;
        if (socket->tx_retries >= NET_TCP_MAX_RETRANSMITS) {
            socket->error = LINUX_ETIMEDOUT;
            net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_TCP_TIMEOUT);
            return;
        }
        if (net_send_tcp_to_mac(socket->dst_mac, socket->local_ip,
                socket->remote_ip, socket->local_port, socket->remote_port,
                socket->local_seq - 1u, socket->remote_seq,
                TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0) < 0) {
            socket->error = LINUX_ENETDOWN;
            net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_TX_FAILED);
            return;
        }
        ++socket->tx_retries;
        socket->tx_rto_ms = socket->tx_rto_ms >= NET_TCP_MAX_RTO_MS / 2u
            ? NET_TCP_MAX_RTO_MS : socket->tx_rto_ms * 2u;
        socket->tx_retransmit_deadline = now + socket->tx_rto_ms;
        net_socket_touch(socket);
        return;
    }
    if (socket->tx_sent && now >= socket->tx_retransmit_deadline) {
            if (!socket->tx_persist_probe && socket->tx_retries >= NET_TCP_MAX_RETRANSMITS) {
                socket->error = LINUX_ETIMEDOUT;
                net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_TCP_TIMEOUT);
                return;
            }
            if (!socket->tx_persist_probe) {
                uint32_t threshold = socket->tx_sent / 2u;
                if (threshold < 2u * NET_TCP_MSS) threshold = 2u * NET_TCP_MSS;
                socket->tx_ssthresh = threshold;
                socket->tx_cwnd = NET_TCP_MSS;
                ++socket->tx_retries;
            }
            socket->tx_persist_probe = socket->peer_window_valid &&
                                       socket->peer_window == 0;
            length = socket->tx_sent < NET_TCP_MSS
                ? socket->tx_sent : NET_TCP_MSS;
            sequence = socket->acked_seq;
            net_socket_tx_copy(socket, segment, 0, length);
            socket->tx_rto_ms = socket->tx_rto_ms >= NET_TCP_MAX_RTO_MS / 2u
                ? NET_TCP_MAX_RTO_MS : socket->tx_rto_ms * 2u;
            socket->tx_retransmit_deadline = now + socket->tx_rto_ms;
            retransmit = true;
            goto transmit;
    }
    send_window = socket->peer_window_valid ? socket->peer_window : 0;
    flight_limit = socket->tx_cwnd < send_window ? socket->tx_cwnd : send_window;
    unsent = socket->tx_len - socket->tx_sent;
    if (unsent && flight_limit > socket->tx_sent) {
        uint32_t room = flight_limit - socket->tx_sent;
        length = unsent > NET_TCP_MSS ? NET_TCP_MSS : unsent;
        if (length > room) length = room;
        sequence = socket->local_seq;
        net_socket_tx_copy(socket, segment, socket->tx_sent, length);
        socket->tx_persist_deadline = 0;
        socket->tx_persist_probe = false;
        goto transmit;
    }
    if (unsent && socket->peer_window_valid && socket->peer_window == 0 &&
        !socket->tx_sent) {
        if (!socket->tx_persist_deadline)
            socket->tx_persist_deadline = now + NET_TCP_INITIAL_RTO_MS;
        if (now < socket->tx_persist_deadline) return;
        length = 1;
        sequence = socket->local_seq;
        net_socket_tx_copy(socket, segment, 0, length);
        socket->tx_persist_probe = true;
        socket->tx_persist_deadline = now + socket->tx_rto_ms;
        goto transmit;
    }
    if (!socket->tx_len && !socket->tx_sent &&
        socket->fin_pending && !socket->fin_sent) {
        if (net_send_tcp_to_mac(socket->dst_mac, socket->local_ip,
                socket->remote_ip, socket->local_port, socket->remote_port,
                socket->local_seq, socket->remote_seq,
                TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0) < 0) {
            socket->error = LINUX_ENETDOWN;
            net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_TX_FAILED);
            return;
        }
        ++socket->local_seq;
        socket->fin_sent = true;
        socket->fin_acked = false;
        socket->tx_retries = 0;
        socket->tx_rto_ms = NET_TCP_INITIAL_RTO_MS;
        socket->tx_retransmit_deadline = now + socket->tx_rto_ms;
        socket->state = RELIEFOS_NET_TCP_TIME_WAIT;
        socket->status = RELIEFOS_NET_STATUS_OK;
        net_socket_touch(socket);
        kernel_poll_readiness_notify();
        return;
    } else return;
transmit:
    if (net_send_tcp_to_mac(socket->dst_mac, socket->local_ip,
            socket->remote_ip, socket->local_port, socket->remote_port,
            sequence, socket->remote_seq, TCP_FLAG_PSH | TCP_FLAG_ACK,
            segment, length) < 0) {
        socket->error = LINUX_ENETDOWN;
        net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_TX_FAILED);
        return;
    }
    if (!retransmit) {
        bool start_timer = socket->tx_sent == 0;
        socket->tx_sent += length;
        socket->local_seq += length;
        socket->tx_bytes += length;
        if (start_timer) {
            if (!socket->tx_rto_ms) socket->tx_rto_ms = NET_TCP_INITIAL_RTO_MS;
            socket->tx_retries = 0;
            socket->tx_retransmit_deadline = now + socket->tx_rto_ms;
        }
    }
    if (retransmit) socket->tx_persist_deadline = now + socket->tx_rto_ms;
    net_socket_touch(socket);
}

/**
 * @brief Service a bounded rotating batch of active TCP sockets.
 * @note The work queue contains only sockets with protocol work, so idle open
 *       sockets never add cost to packet polling or periodic maintenance.
 */
void net_tcp_service_sockets(void)
{
    uint32_t service_limit = net_tcp_work_count;
    if (service_limit > NET_TCP_SOCKET_SERVICE_BUDGET)
        service_limit = NET_TCP_SOCKET_SERVICE_BUDGET;
    for (uint32_t serviced = 0; serviced < service_limit &&
         net_tcp_work_head; ++serviced) {
        struct net_socket *socket = net_tcp_work_head;
        net_socket_work_remove(socket);
        net_socket_service(socket);
        net_socket_work_schedule(socket);
    }
}

/**
 * @brief Allocate and register one dynamically backed TCP socket.
 * @param request Requested domain/type/protocol and output handle/status.
 * @param owner_pid Owning process identifier.
 * @param owner_uid Owning user identifier.
 * @return Zero when processed; request->status reports unsupported type or allocation failure.
 * @note The returned handle remains stable until its final descriptor/reference is collected.
 */
int net_socket_open(struct reliefos_net_socket_open *request, uint32_t owner_pid,
                    uint32_t owner_uid)
{
    struct net_socket *socket;
    if (!request) {
        return -1;
    }
    request->socket = -1;
    request->status = RELIEFOS_NET_STATUS_PROTOCOL_UNSUPPORTED;
    if (request->domain != RELIEFOS_NET_AF_INET ||
        request->type != RELIEFOS_NET_SOCK_STREAM ||
        (request->protocol != 0 &&
         request->protocol != RELIEFOS_NET_IPPROTO_TCP)) {
        return 0;
    }
    socket = net_socket_alloc(owner_pid, owner_uid);
    if (!socket) {
        request->status = RELIEFOS_NET_STATUS_SOCKET_LIMIT;
        return 0;
    }
    request->socket = socket->handle;
    request->status = RELIEFOS_NET_STATUS_OK;
    return 0;
}

/**
 * @brief Start one resolved TCP connection and initialize all per-connection state.
 * @param socket Stable registered socket object.
 * @param remote_ip Peer IPv4 address in host byte order.
 * @param remote_port Peer TCP port.
 * @param timeout_ms Explicit connection timeout in milliseconds.
 * @param asynchronous Return after the SYN when true, otherwise poll to completion.
 * @return ReliefOS network status; asynchronous start returns TCP_TIMEOUT while SYN_SENT.
 * @note Queue, sequence, congestion, retry, peer-window, FIN, and error state are reset.
 */
static uint32_t net_socket_connect_ip(struct net_socket *socket,
                                      uint32_t remote_ip, uint16_t remote_port,
                                      uint32_t timeout_ms, bool asynchronous)
{
    uint8_t dst_mac[6];
    uint32_t arp_ip;
    uint32_t syn_seq;
    uint64_t start;
    uint64_t next_retransmit;
    uint32_t spins = 0;
    int ret;

    if (!socket || !remote_ip || !remote_port) {
        return RELIEFOS_NET_STATUS_BAD_ARGUMENT;
    }
    arp_ip = net_route_arp_ip(remote_ip);
    ret = net_resolve_mac(arp_ip, timeout_ms, dst_mac);
    if (ret < 0) {
        return ret == -1 ? RELIEFOS_NET_STATUS_TX_FAILED
                         : RELIEFOS_NET_STATUS_ARP_TIMEOUT;
    }

    socket->state = RELIEFOS_NET_TCP_SYN_SENT;
    socket->rx_head = 0;
    socket->rx_window_valid = false;
    socket->status = RELIEFOS_NET_STATUS_TCP_TIMEOUT;
    socket->error = 0;
    socket->shutdown_read = false;
    socket->shutdown_write = false;
    socket->fin_pending = false;
    socket->fin_sent = false;
    socket->fin_acked = false;
    socket->local_ip = net_config.local_ip;
    socket->remote_ip = remote_ip;
    socket->remote_port = remote_port;
    socket->local_seq = 0x4c4e0000u ^ (net_sequence++ << 8) ^
                        (uint32_t)time_uptime_ms();
    socket->local_port = (uint16_t)(49152u + (socket->local_seq & 0x3fffu));
    socket->remote_seq = 0;
    socket->acked_seq = 0;
    socket->peer_window = 0;
    socket->peer_window_seq = 0;
    socket->peer_window_ack = 0;
    socket->peer_window_valid = false;
    socket->tx_cwnd = 3u * NET_TCP_MSS;
    socket->tx_ssthresh = NET_SOCKET_TX_CAP;
    socket->tx_rto_ms = NET_TCP_INITIAL_RTO_MS;
    socket->tx_retries = 0;
    socket->tx_persist_deadline = 0;
    socket->tx_persist_probe = false;
    socket->tx_sent = 0;
    socket->tx_len = 0;
    socket->tx_head = 0;
    socket->tx_retransmit_deadline = 0;
    socket->rx_len = 0;
    socket->rx_head = 0;
    socket->fin_received = 0;
    net_memcpy(socket->dst_mac, dst_mac, 6);
    net_socket_touch(socket);

    syn_seq = socket->local_seq;
    if (net_send_tcp_to_mac(socket->dst_mac, net_config.local_ip,
                            socket->remote_ip, socket->local_port,
                            socket->remote_port, syn_seq, 0,
                            TCP_FLAG_SYN, 0, 0) < 0) {
        net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_TX_FAILED);
        return socket->status;
    }
    ++socket->local_seq;
    start = time_uptime_ms();
    next_retransmit = start + NET_TCP_SYN_RETRANSMIT_MS;
    if (asynchronous) {
        socket->syn_deadline = start + timeout_ms;
        socket->syn_retransmit = next_retransmit;
        net_socket_work_schedule(socket);
        return RELIEFOS_NET_STATUS_TCP_TIMEOUT;
    }
    while (!net_timeout_expired(start, timeout_ms, spins++)) {
        uint64_t now;
        net_poll_once(0, 0, 0, 0);
        if (socket->state == RELIEFOS_NET_TCP_ESTABLISHED) {
            return RELIEFOS_NET_STATUS_OK;
        }
        if (socket->state == RELIEFOS_NET_TCP_CLOSED) {
            return socket->status;
        }
        now = time_uptime_ms();
        if (now >= next_retransmit) {
            if (net_send_tcp_to_mac(socket->dst_mac, net_config.local_ip,
                                    socket->remote_ip, socket->local_port,
                                    socket->remote_port, syn_seq, 0,
                                    TCP_FLAG_SYN, 0, 0) < 0) {
                net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_TX_FAILED);
                return socket->status;
            }
            next_retransmit = now + NET_TCP_SYN_RETRANSMIT_MS;
        }
        net_cpu_relax();
    }
    net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_TCP_TIMEOUT);
    return socket->status;
}

/**
 * @brief Resolve and connect a TCP socket, publishing the selected endpoints.
 * @param request Socket handle, hostname/port, timeout, and output status/endpoints.
 * @param owner_pid Required owner process identifier.
 * @return Zero when processed; request->status carries connection outcome.
 * @note The legacy control ABI retains its explicit bounded timeout; Linux fd calls
 * use their connection path and are not governed by SO_RCVTIMEO/SO_SNDTIMEO.
 */
int net_socket_connect(struct reliefos_net_socket_connect *request,
                       uint32_t owner_pid)
{
    struct net_socket *socket;
    uint32_t remote_ips[RELIEFOS_NET_DNS_MAX_ADDRESSES];
    uint32_t remote_count = 0;
    uint32_t timeout_ms;
    uint32_t host_len;
    uint32_t status = RELIEFOS_NET_STATUS_TCP_FAILED;
    uint32_t literal_ip = 0;
    int literal = 0;

    if (!request) {
        return -1;
    }
    request->status = RELIEFOS_NET_STATUS_BAD_ARGUMENT;
    request->remote_ip = 0;
    request->local_ip = 0;
    request->local_port = 0;
    if (!request->host[0] || request->port == 0 || request->port > 65535u) {
        return 0;
    }
    socket = net_socket_find(request->socket, owner_pid, 1);
    if (!socket) {
        request->status = RELIEFOS_NET_STATUS_SOCKET_BAD_HANDLE;
        return 0;
    }
    if (socket->state != RELIEFOS_NET_TCP_CLOSED) {
        request->status = RELIEFOS_NET_STATUS_BAD_ARGUMENT;
        return 0;
    }
    if (!e1000_is_ready()) {
        request->status = RELIEFOS_NET_STATUS_NO_DEVICE;
        return 0;
    }
    timeout_ms = net_socket_clamp_timeout(request->timeout_ms);
    host_len = net_strlen(request->host, RELIEFOS_NET_HOSTNAME_LEN);
    if (host_len == 0 || host_len >= RELIEFOS_NET_HOSTNAME_LEN) {
        request->status = RELIEFOS_NET_STATUS_BAD_ARGUMENT;
        return 0;
    }
    literal = net_parse_ipv4_literal(request->host, host_len, &literal_ip) == 0;
    {
        uint32_t config_status = net_ensure_ipv4_config(timeout_ms, literal ? 0 : 1);
        if (config_status != RELIEFOS_NET_STATUS_OK) {
            request->status = config_status;
            return 0;
        }
    }
    if (literal) {
        remote_ips[0] = literal_ip;
        remote_count = 1;
    } else if (net_http_resolve_hosts(request->host, host_len, timeout_ms,
                                      remote_ips, &remote_count,
                                      &status) < 0) {
        request->status = status;
        return 0;
    }
    for (uint32_t i = 0; i < remote_count; ++i) {
        status = net_socket_connect_ip(socket, remote_ips[i],
                                       (uint16_t)request->port, timeout_ms, false);
        if (status == RELIEFOS_NET_STATUS_OK) {
            request->status = status;
            request->remote_ip = socket->remote_ip;
            request->local_ip = socket->local_ip;
            request->local_port = socket->local_port;
            return 0;
        }
    }
    request->status = status;
    return 0;
}

/**
 * @brief Copy as much as fits into a TCP socket's bounded transmit ring.
 * @param request Socket handle, borrowed payload, requested length, and result fields.
 * @param owner_pid Optional owner process filter.
 * @return Zero when processed; request reports queued bytes and network status.
 * @note Accepted data remains in the stable socket object until cumulative ACKs advance.
 */
int net_socket_send(struct reliefos_net_socket_io *request, uint32_t owner_pid)
{
    struct net_socket *socket;
    const uint8_t *data;
    uint32_t available;
    uint32_t accepted;

    if (!request) {
        return -1;
    }
    request->transferred = 0;
    request->status = RELIEFOS_NET_STATUS_BAD_ARGUMENT;
    if (request->length && !request->buffer) {
        return 0;
    }
    socket = net_socket_find(request->socket, owner_pid, 1);
    if (!socket) {
        request->status = RELIEFOS_NET_STATUS_SOCKET_BAD_HANDLE;
        return 0;
    }
    if (socket->state == RELIEFOS_NET_TCP_CLOSED || socket->shutdown_write ||
        socket->fin_sent) {
        request->status = RELIEFOS_NET_STATUS_SOCKET_CLOSED;
        return 0;
    }
    if (!net_socket_write_open(socket)) {
        request->status = RELIEFOS_NET_STATUS_SOCKET_NOT_CONNECTED;
        return 0;
    }
    if (request->length == 0) {
        request->status = RELIEFOS_NET_STATUS_OK;
        return 0;
    }
    net_poll_packets();
    socket = net_socket_find(request->socket, owner_pid, 1);
    if (!socket) {
        request->status = RELIEFOS_NET_STATUS_SOCKET_BAD_HANDLE;
        return 0;
    }
    if (socket->state == RELIEFOS_NET_TCP_CLOSED || socket->shutdown_write ||
        socket->fin_sent) {
        request->status = RELIEFOS_NET_STATUS_SOCKET_CLOSED;
        return 0;
    }
    if (!net_socket_write_open(socket)) {
        request->status = RELIEFOS_NET_STATUS_SOCKET_NOT_CONNECTED;
        return 0;
    }
    data = (const uint8_t *)request->buffer;
    available = NET_SOCKET_TX_CAP - socket->tx_len;
    if (!available) {
        request->status = RELIEFOS_NET_STATUS_TCP_TIMEOUT;
        return 0;
    }
    accepted = request->length < available ? request->length : available;
    uint32_t tail = (socket->tx_head + socket->tx_len) % NET_SOCKET_TX_CAP;
    uint32_t first = NET_SOCKET_TX_CAP - tail;
    if (first > accepted) first = accepted;
    net_memcpy(socket->tx + tail, data, first);
    net_memcpy(socket->tx, data + first, accepted - first);
    socket->tx_len += accepted;
    net_socket_work_schedule(socket);
    net_socket_touch(socket);
    request->transferred = accepted;
    request->status = RELIEFOS_NET_STATUS_OK;
    if (available == accepted) kernel_poll_readiness_notify();
    net_poll_packets();
    return 0;
}

/**
 * @brief Copy queued TCP bytes with optional consumption, or report EOF/status.
 * @param request Socket, destination buffer, requested length, and output status.
 * @param owner_pid Optional process owner filter.
 * @param consume Advance the ring and reopen its window only when true.
 * @return Zero on a handled request, or a negative internal error.
 * Caller holds execution ownership and the socket lifetime. A peek never
 * advances receive state or advertises additional buffer space to the peer.
 */
static int net_socket_receive(struct reliefos_net_socket_io *request, uint32_t owner_pid,
                               bool consume)
{
    struct net_socket *socket;
    uint8_t *dst;
    uint32_t timeout_ms;
    uint64_t start;
    uint32_t spins = 0;

    if (!request) {
        return -1;
    }
    request->transferred = 0;
    request->status = RELIEFOS_NET_STATUS_BAD_ARGUMENT;
    if (request->length && !request->buffer) {
        return 0;
    }
    socket = net_socket_find(request->socket, owner_pid, 1);
    if (!socket) {
        request->status = RELIEFOS_NET_STATUS_SOCKET_BAD_HANDLE;
        return 0;
    }
    if (!request->length || socket->shutdown_read) { request->status = RELIEFOS_NET_STATUS_OK; return 0; }
    if (socket->state != RELIEFOS_NET_TCP_ESTABLISHED &&
        socket->state != RELIEFOS_NET_TCP_TIME_WAIT) {
        request->status = socket->state == RELIEFOS_NET_TCP_CLOSED
                              ? RELIEFOS_NET_STATUS_SOCKET_CLOSED
                              : RELIEFOS_NET_STATUS_SOCKET_NOT_CONNECTED;
        return 0;
    }
    dst = (uint8_t *)request->buffer;
    timeout_ms = net_socket_clamp_timeout(request->timeout_ms);
    start = time_uptime_ms();
    while (socket->rx_len == 0 && !socket->fin_received &&
           socket->state != RELIEFOS_NET_TCP_CLOSED &&
           !net_timeout_expired(start, timeout_ms, spins++)) {
        net_poll_once(0, 0, 0, 0);
        net_cpu_relax();
    }
    if (socket->rx_len) {
        uint32_t copy_len = request->length;
        if (copy_len > socket->rx_len) {
            copy_len = socket->rx_len;
        }
        if (copy_len) {
            uint32_t first = NET_SOCKET_RX_CAP - socket->rx_head;
            if (first > copy_len) first = copy_len;
            net_memcpy(dst, socket->rx + socket->rx_head, first);
            net_memcpy(dst + first, socket->rx, copy_len - first);
            if (consume) {
                socket->rx_head = (socket->rx_head + copy_len) % NET_SOCKET_RX_CAP;
                socket->rx_len -= copy_len;
                uint32_t remaining = net_socket_receive_window(socket);
                uint32_t window = net_socket_select_window(socket);
                /* Linux tcp_cleanup_rbuf() sends a window update after a
                 * significant increase, including reopening a zero window. */
                if (!socket->fin_received && window > remaining && window >= 2u * remaining) {
                    (void)net_send_tcp_to_mac(socket->dst_mac, net_config.local_ip,
                                          socket->remote_ip, socket->local_port,
                                          socket->remote_port, socket->local_seq,
                                          socket->remote_seq, TCP_FLAG_ACK, 0, 0);
                }
            }
        }
        request->transferred = copy_len;
        request->status = RELIEFOS_NET_STATUS_OK;
        return 0;
    }
    if (socket->fin_received) {
        request->status = RELIEFOS_NET_STATUS_OK;
        return 0;
    }
    if (socket->state == RELIEFOS_NET_TCP_CLOSED) {
        request->status = socket->status;
        return 0;
    }
    request->status = RELIEFOS_NET_STATUS_TCP_TIMEOUT;
    if (net_socket_trace_tls(socket)) {
        console_warn("[net] tls rx timeout socket=%d state=%u expected=%u queued=%u fin=%u\n",
                       socket->handle, socket->state, socket->remote_seq,
                       socket->rx_len, socket->fin_received);
    }
    return 0;
}

/**
 * @brief Consume available TCP bytes through the common receive path.
 * @param request Borrowed handle, writable length-byte buffer and output fields.
 * @param owner_pid Optional owner filter, zero to use the held socket identity.
 * @return Zero with status/progress, or a negative internal validation error.
 * Execution ownership and an independent socket lifetime pin are required.
 */
int net_socket_recv(struct reliefos_net_socket_io *request, uint32_t owner_pid)
{
    return net_socket_receive(request, owner_pid, true);
}

/**
 * @brief Copy available TCP bytes without consuming or reopening its receive ring.
 * @param request Borrowed handle, writable length-byte buffer and output fields.
 * @param owner_pid Optional owner filter, zero to use the held socket identity.
 * @return Zero with status/progress, or a negative internal validation error.
 * Execution ownership and a socket lifetime pin are required; no references
 * escape this call. Repeated peeks observe the same unconsumed byte prefix.
 */
int net_socket_peek(struct reliefos_net_socket_io *request, uint32_t owner_pid)
{
    return net_socket_receive(request, owner_pid, false);
}

/**
 * @brief Queue a write-side FIN after previously accepted TCP bytes are acknowledged.
 * @param request Socket handle and output status.
 * @param owner_pid Optional owner process filter.
 * @return Zero when processed; request->status reports an invalid handle.
 * @note The network poll service sends deferred data/FIN work after this call returns.
 */
int net_socket_close(struct reliefos_net_socket_close *request,
                     uint32_t owner_pid)
{
    struct net_socket *socket;
    if (!request) {
        return -1;
    }
    request->status = RELIEFOS_NET_STATUS_SOCKET_BAD_HANDLE;
    socket = net_socket_find(request->socket, owner_pid, 1);
    if (!socket) {
        return 0;
    }
    if (net_socket_write_open(socket)) {
        socket->shutdown_write = true;
        socket->fin_pending = true;
        net_socket_work_schedule(socket);
        (void)kernel_wait_queue_wake_all(&socket->wait_write);
        kernel_poll_readiness_notify();
        net_socket_service(socket);
    } else if (socket->state == RELIEFOS_NET_TCP_SYN_SENT) {
        net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_SOCKET_CLOSED);
    } else if (socket->state == RELIEFOS_NET_TCP_CLOSED) {
        socket->status = RELIEFOS_NET_STATUS_SOCKET_CLOSED;
        net_socket_touch(socket);
    }
    request->status = RELIEFOS_NET_STATUS_OK;
    return 0;
}

/**
 * Net connection visible.
 * @param socket Value supplied by the caller.
 * @param viewer Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_connection_visible(const struct net_socket *socket,
                                  const struct task *viewer)
{
    if (!socket || !viewer) {
        return 0;
    }
    if (viewer->role == RELIEFOS_AUTH_ROLE_ADMIN) {
        return 1;
    }
    if (viewer->flags & TASK_FLAG_SERVICE) {
        return 1;
    }
    return viewer->uid != 0 && socket->owner_uid == viewer->uid;
}

/**
 * @brief Count visible live TCP sockets and fill the caller's bounded snapshot.
 * @param request Snapshot capacity, optional entry array, and live-count output.
 * @param viewer Task identity used to apply admin/service/user visibility rules.
 * @return Zero or a negative error for an invalid request.
 * @note request->count is not capped by the legacy fixed-size entry array capacity.
 */
int net_connections(struct reliefos_net_connection_list *request,
                    const struct task *viewer)
{
    uint32_t now = net_now32();
    uint32_t count = 0;
    if (!request) {
        return -1;
    }
    net_socket_gc();
    for (struct net_socket *socket = net_sockets; socket; socket = socket->registry_next) {
        if (!socket->used || !net_connection_visible(socket, viewer)) {
            continue;
        }
        if (request->entries && count < request->capacity) {
            request->entries[count] = (struct reliefos_net_connection_info){
                .socket = socket->handle,
                .owner_pid = socket->owner_pid,
                .state = socket->state,
                .status = socket->status,
                .local_ip = socket->local_ip,
                .remote_ip = socket->remote_ip,
                .local_port = socket->local_port,
                .remote_port = socket->remote_port,
                .age_ms = now - socket->created_ms,
                .tx_bytes = socket->tx_bytes,
                .rx_bytes = socket->rx_bytes,
            };
        }
        ++count;
    }
    request->count = count;
    return 0;
}

/**
 * @brief Report readability, writability, hangup, and errors for an IPv4 socket.
 * @param handle Internal socket handle.
 * @param owner_pid Optional owner filter.
 * @param events Requested readiness bits.
 * @return Ready poll bits, including POLLNVAL for an unknown handle.
 * @note Peer FIN reports POLLRDHUP when requested; POLLHUP waits until the
 *       local write half also closes. POLLOUT includes an immediate write error.
 */
short net_socket_poll_fd(int32_t handle, uint32_t owner_pid, short events)
{
    net_poll_packets();
    struct net_socket *socket = net_socket_find(handle, owner_pid, 1);
    short result = 0;
    if (!socket) return POLLNVAL;
    if (socket->state == RELIEFOS_NET_TCP_SYN_SENT && socket->syn_deadline) {
        uint64_t now = time_uptime_ms();
        if (now >= socket->syn_deadline) {
            socket->error = LINUX_ETIMEDOUT;
            net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_TCP_TIMEOUT);
        }
    }
    if (socket->state == RELIEFOS_NET_TCP_ESTABLISHED ||
        socket->state == RELIEFOS_NET_TCP_TIME_WAIT) {
        if ((events & POLLIN) &&
            (socket->rx_len || socket->fin_received || socket->shutdown_read))
            result |= POLLIN;
        if ((events & POLLRDHUP) && socket->fin_received)
            result |= POLLRDHUP;
        if ((events & POLLOUT) &&
            (socket->shutdown_write || socket->fin_sent ||
             (net_socket_write_open(socket) &&
              socket->tx_len < NET_SOCKET_TX_CAP)))
            result |= POLLOUT;
        if (socket->fin_received && (socket->shutdown_write || socket->fin_sent))
            result |= POLLHUP;
    } else if (socket->state == RELIEFOS_NET_TCP_CLOSED) {
        result |= POLLHUP;
        if (events & POLLOUT) result |= POLLOUT;
    }
    if (socket->error) result |= POLLERR;
    return result;
}

void net_socket_pin_fd(int32_t handle)
{
    struct net_socket *s = net_socket_find(handle, 0, 1);
    if (s) s->fd_owned = true;
}

void net_socket_release_fd(int32_t handle)
{
    struct net_socket *s = net_socket_find(handle, 0, 1);
    if (!s) return;
    struct reliefos_net_socket_close request = {.socket = handle};
    (void)net_socket_close(&request, 0);
    s->fd_owned = false;
}

int net_socket_error(int32_t handle, bool clear)
{
    struct net_socket *s = net_socket_find(handle, 0, 1);
    if (!s) return LINUX_EBADF;
    int error = s->error;
    if (clear) s->error = 0;
    return error;
}

int net_socket_available(int32_t handle)
{
    struct net_socket *s = net_socket_find(handle, 0, 1);
    return s ? (int)s->rx_len : -LINUX_EBADF;
}

int net_socket_connect_fd(int32_t handle, uint32_t ip, uint16_t port, bool nonblock)
{
    struct net_socket *s = net_socket_find(handle, 0, 1);
    if (!s) return -LINUX_EBADF;
    if (s->state == RELIEFOS_NET_TCP_SYN_SENT) return -LINUX_EALREADY;
    if (s->state != RELIEFOS_NET_TCP_CLOSED) return -LINUX_EISCONN;
    if (!e1000_is_ready()) return -LINUX_ENETDOWN;
    if (!ip || !port) return -LINUX_EINVAL;
    if (net_ensure_ipv4_config(3000, 0) != RELIEFOS_NET_STATUS_OK) return -LINUX_ENETUNREACH;
    s->error = 0;
    uint32_t status = net_socket_connect_ip(s, ip, port, 10000, nonblock);
    if (s->state == RELIEFOS_NET_TCP_SYN_SENT) return -LINUX_EINPROGRESS;
    if (status == RELIEFOS_NET_STATUS_OK) return 0;
    if (s->error) return -s->error;
    return status == RELIEFOS_NET_STATUS_ARP_TIMEOUT ? -LINUX_EHOSTUNREACH : -LINUX_ETIMEDOUT;
}

/**
 * @brief Shut down reads or queue a write-side FIN after pending data drains.
 * @param handle Internal socket handle.
 * @param how SHUT_RD, SHUT_WR, or SHUT_RDWR.
 * @return Zero on success or a negative errno.
 */
int net_socket_shutdown_fd(int32_t handle, int how)
{
    struct net_socket *s = net_socket_find(handle, 0, 1);
    if (!s) return -LINUX_EBADF;
    if (how < 0 || how > 2) return -LINUX_EINVAL;
    if (s->state != RELIEFOS_NET_TCP_ESTABLISHED && s->state != RELIEFOS_NET_TCP_TIME_WAIT)
        return -LINUX_ENOTCONN;
    if (how != 1) { s->shutdown_read = true; s->rx_len = 0; }
    if (how != 0 && !s->shutdown_write) {
        s->shutdown_write = true;
        s->fin_pending = true;
        net_socket_work_schedule(s);
        (void)kernel_wait_queue_wake_all(&s->wait_write);
        kernel_poll_readiness_notify();
        net_socket_service(s);
    }
    return 0;
}

int net_socket_address(int32_t handle, uint32_t owner_pid,
                       uint32_t *local_ip, uint16_t *local_port,
                       uint32_t *remote_ip, uint16_t *remote_port)
{
    struct net_socket *socket = net_socket_find(handle, owner_pid, 1);
    if (!socket) return -9;
    if (local_ip) *local_ip = socket->local_ip;
    if (local_port) *local_port = socket->local_port;
    if (remote_ip) *remote_ip = socket->remote_ip;
    if (remote_port) *remote_port = socket->remote_port;
    return 0;
}

/**
 * @brief Retire unowned TCP sockets after process exit without dropping accepted data.
 * @param owner_pid Process identifier whose network state is being retired.
 * @note Sockets with queued/unacknowledged bytes or an initiated FIN stay
 *       registered for the network pump; queued bytes drain before the FIN.
 *       Idle, unconnected sockets can be closed immediately.
 */
void net_close_owner_sockets(uint32_t owner_pid)
{
    if (!owner_pid) {
        return;
    }
    for (struct net_socket *socket = net_sockets; socket; socket = socket->registry_next) {
        if (!socket->used || socket->fd_owned || socket->owner_pid != owner_pid ||
            socket->state == RELIEFOS_NET_TCP_CLOSED) continue;
        if (socket->state == RELIEFOS_NET_TCP_TIME_WAIT || socket->fin_pending ||
            socket->fin_sent || socket->tx_len || socket->tx_sent) {
            if (net_socket_write_open(socket)) {
                socket->shutdown_write = true;
                socket->fin_pending = true;
                net_socket_work_schedule(socket);
                (void)kernel_wait_queue_wake_all(&socket->wait_write);
                kernel_poll_readiness_notify();
            }
            net_socket_service(socket);
            continue;
        }
        net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_SOCKET_CLOSED);
    }
}

/**
 * @brief Poll network progress and park a writer while its transmit queue is full.
 * @param handle Internal socket handle.
 * @return One while parked, zero when ready to retry, or a negative errno.
 */
int net_socket_wait_writable(int32_t handle)
{
    struct task *task = sched_current_task();
    struct net_socket *socket;
    uint64_t timeout_ticks;
    uint64_t now;
    uint64_t wake_tick;
    if (!task) return -LINUX_ESRCH;
    socket = net_socket_find(handle, 0, 1);
    if (!socket) return -LINUX_EBADF;
    kernel_wait_queue_remove(&socket->wait_write, task);
    net_poll_packets();
    socket = net_socket_find(handle, 0, 1);
    if (!socket) return -LINUX_EBADF;
    if (socket->state == RELIEFOS_NET_TCP_CLOSED || socket->shutdown_write ||
        (socket->state == RELIEFOS_NET_TCP_TIME_WAIT &&
         !net_socket_write_open(socket)))
        return -LINUX_EPIPE;
    if (!net_socket_write_open(socket) || socket->tx_len < NET_SOCKET_TX_CAP)
        return 0;

    timeout_ticks = socket->send_timeout;
    now = time_ticks();
    if (!task->socket_io_deadline) {
        task->socket_io_timed = timeout_ticks != UINT64_MAX;
        task->socket_io_deadline = timeout_ticks == UINT64_MAX ||
                UINT64_MAX - now < timeout_ticks
            ? UINT64_MAX : now + timeout_ticks;
    }
    if (task->socket_io_deadline <= now) return -LINUX_EAGAIN;
    if (kernel_wait_queue_add(&socket->wait_write, task) < 0)
        return -LINUX_ENOBUFS;
    wake_tick = now == UINT64_MAX ? UINT64_MAX : now + 1u;
    if (task->socket_io_deadline < wake_tick) wake_tick = task->socket_io_deadline;
    /* The one-tick fallback polls NIC input even when no other syscall runs. */
    sched_sleep_current_until(wake_tick);
    return 1;
}

/**
 * @brief Park a blocking stream reader until data, EOF, or an error is available.
 * @param handle Internal socket handle.
 * @return One while parked, zero when it should retry, or a negative errno.
 */
int net_socket_wait_readable(int32_t handle)
{
    struct task *task = sched_current_task();
    struct net_socket *socket;
    uint64_t now;
    uint64_t wake_tick;
    if (!task) return -LINUX_ESRCH;
    socket = net_socket_find(handle, 0, 1);
    if (!socket) return -LINUX_EBADF;
    kernel_wait_queue_remove(&socket->wait_read, task);
    net_poll_packets();
    socket = net_socket_find(handle, 0, 1);
    if (!socket) return -LINUX_EBADF;
    if (socket->rx_len || socket->fin_received ||
        socket->state == RELIEFOS_NET_TCP_CLOSED) return 0;

    now = time_ticks();
    if (!task->socket_io_deadline) {
        task->socket_io_timed = socket->receive_timeout != UINT64_MAX;
        task->socket_io_deadline = socket->receive_timeout == UINT64_MAX ||
                UINT64_MAX - now < socket->receive_timeout
            ? UINT64_MAX : now + socket->receive_timeout;
    }
    if (task->socket_io_deadline <= now) return -LINUX_EAGAIN;
    if (kernel_wait_queue_add(&socket->wait_read, task) < 0)
        return -LINUX_ENOBUFS;
    wake_tick = now == UINT64_MAX ? UINT64_MAX : now + 1u;
    if (task->socket_io_deadline < wake_tick) wake_tick = task->socket_io_deadline;
    /* RX is poll-driven on this NIC, so one-tick polling complements the queue. */
    sched_sleep_current_until(wake_tick);
    return 1;
}

/**
 * @brief Return the configured TCP timeout in scheduler ticks.
 * @param handle Internal socket handle.
 * @param send Select SO_SNDTIMEO when true, SO_RCVTIMEO otherwise.
 * @return Timeout ticks, or UINT64_MAX for an infinite wait or invalid handle.
 */
uint64_t net_socket_get_timeout_ticks(int32_t handle, bool send)
{
    struct net_socket *socket = net_socket_find(handle, 0, 1);
    if (!socket) return UINT64_MAX;
    return send ? socket->send_timeout : socket->receive_timeout;
}

/**
 * @brief Configure the TCP send or receive timeout in scheduler ticks.
 * @param handle Internal socket handle.
 * @param send Select SO_SNDTIMEO when true, SO_RCVTIMEO otherwise.
 * @param ticks Timeout ticks, or UINT64_MAX for an infinite wait.
 * @return Zero on success or a negative errno.
 */
int net_socket_set_timeout_ticks(int32_t handle, bool send, uint64_t ticks)
{
    struct net_socket *socket = net_socket_find(handle, 0, 1);
    if (!socket) return -LINUX_EBADF;
    if (send) socket->send_timeout = ticks;
    else socket->receive_timeout = ticks;
    return 0;
}
