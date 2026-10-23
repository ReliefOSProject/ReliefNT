#include "../internal.h"

/**
 * Net now32.
 * @return The value or status produced by the operation.
 */
static uint32_t net_now32(void);

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
 * Net socket connect ip.
 * @param socket Value supplied by the caller.
 * @param remote_ip Value supplied by the caller.
 * @param remote_port Value supplied by the caller.
 * @param timeout_ms Value supplied by the caller.
 * @return The value or status produced by the operation.
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
/**
 * Net now32.
 * @return The value or status produced by the operation.
 */
static uint32_t net_now32(void)
{
    return (uint32_t)time_uptime_ms();
}

/**
 * Net socket clear.
 * @param socket Value supplied by the caller.
 */
static void net_socket_clear(struct net_socket *socket)
{
    if (socket) {
        kernel_free(socket->rx);
        net_memzero(socket, sizeof(*socket));
    }
}

/**
 * Net socket touch.
 * @param socket Value supplied by the caller.
 */
void net_socket_touch(struct net_socket *socket)
{
    if (socket) {
        socket->changed_ms = net_now32();
    }
}

/**
 * Net socket gc.
 */
static void net_socket_gc(void)
{
    uint32_t now = net_now32();
    for (uint32_t i = 0; i < RELIEFOS_NET_SOCKET_MAX; ++i) {
        struct net_socket *socket = &net_sockets[i];
        if (!socket->used || socket->fd_owned) {
            continue;
        }
        if ((socket->state == RELIEFOS_NET_TCP_CLOSED ||
             socket->state == RELIEFOS_NET_TCP_TIME_WAIT) &&
            now - socket->changed_ms >= NET_SOCKET_CLOSE_HOLD_MS) {
            net_socket_clear(socket);
        }
    }
}

/**
 * Net socket find.
 * @param handle Value supplied by the caller.
 * @param owner_pid Value supplied by the caller.
 * @param allow_closed Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static struct net_socket *net_socket_find(int32_t handle, uint32_t owner_pid,
                                          int allow_closed)
{
    if (handle <= 0) {
        return 0;
    }
    for (uint32_t i = 0; i < RELIEFOS_NET_SOCKET_MAX; ++i) {
        struct net_socket *socket = &net_sockets[i];
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
 * Net socket mark closed.
 * @param socket Value supplied by the caller.
 * @param status Output storage updated by the function.
 */
void net_socket_mark_closed(struct net_socket *socket, uint32_t status)
{
    if (!socket) {
        return;
    }
    socket->state = RELIEFOS_NET_TCP_CLOSED;
    socket->status = status;
    net_socket_touch(socket);
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
 * Net socket alloc.
 * @param owner_pid Value supplied by the caller.
 * @param owner_uid Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static struct net_socket *net_socket_alloc(uint32_t owner_pid, uint32_t owner_uid)
{
    struct net_socket *slot = 0;
    net_socket_gc();
    for (uint32_t i = 0; i < RELIEFOS_NET_SOCKET_MAX; ++i) {
        if (!net_sockets[i].used) {
            slot = &net_sockets[i];
            break;
        }
    }
    if (!slot) {
        for (uint32_t i = 0; i < RELIEFOS_NET_SOCKET_MAX; ++i) {
            if (!net_sockets[i].fd_owned && net_sockets[i].state == RELIEFOS_NET_TCP_CLOSED) {
                slot = &net_sockets[i];
                break;
            }
        }
    }
    if (!slot) {
        for (uint32_t i = 0; i < RELIEFOS_NET_SOCKET_MAX; ++i) {
            if (!net_sockets[i].fd_owned && net_sockets[i].state == RELIEFOS_NET_TCP_TIME_WAIT) {
                slot = &net_sockets[i];
                break;
            }
        }
    }
    if (!slot) {
        return 0;
    }
    net_socket_clear(slot);
    slot->rx = kernel_malloc(NET_SOCKET_RX_CAP);
    if (!slot->rx) return 0;
    slot->used = 1;
    slot->handle = net_next_socket_handle++;
    if (net_next_socket_handle <= 0) {
        net_next_socket_handle = 1;
    }
    slot->owner_pid = owner_pid;
    slot->owner_uid = owner_uid;
    slot->state = RELIEFOS_NET_TCP_CLOSED;
    slot->status = RELIEFOS_NET_STATUS_SOCKET_CLOSED;
    slot->created_ms = net_now32();
    slot->changed_ms = slot->created_ms;
    return slot;
}

/**
 * Net socket clamp timeout.
 * @param timeout_ms Value supplied by the caller.
 * @return The value or status produced by the operation.
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
 * Net socket open.
 * @param request Value supplied by the caller.
 * @param owner_pid Value supplied by the caller.
 * @param owner_uid Value supplied by the caller.
 * @return The value or status produced by the operation.
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
 * Net socket connect ip.
 * @param socket Value supplied by the caller.
 * @param remote_ip Value supplied by the caller.
 * @param remote_port Value supplied by the caller.
 * @param timeout_ms Value supplied by the caller.
 * @return The value or status produced by the operation.
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
    socket->local_ip = net_config.local_ip;
    socket->remote_ip = remote_ip;
    socket->remote_port = remote_port;
    socket->local_seq = 0x4c4e0000u ^ (net_sequence++ << 8) ^
                        (uint32_t)time_uptime_ms();
    socket->local_port = (uint16_t)(49152u + (socket->local_seq & 0x3fffu));
    socket->remote_seq = 0;
    socket->acked_seq = 0;
    socket->rx_len = 0;
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
 * Net socket connect.
 * @param request Value supplied by the caller.
 * @param owner_pid Value supplied by the caller.
 * @return The value or status produced by the operation.
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
 * Net socket send.
 * @param request Value supplied by the caller.
 * @param owner_pid Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_socket_send(struct reliefos_net_socket_io *request, uint32_t owner_pid)
{
    struct net_socket *socket;
    const uint8_t *data;
    uint32_t timeout_ms;

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
    if (socket->shutdown_write) { request->status = RELIEFOS_NET_STATUS_SOCKET_CLOSED; return 0; }
    if (socket->state == RELIEFOS_NET_TCP_TIME_WAIT ||
        socket->state == RELIEFOS_NET_TCP_CLOSED) {
        request->status = RELIEFOS_NET_STATUS_SOCKET_CLOSED;
        return 0;
    }
    if (socket->state != RELIEFOS_NET_TCP_ESTABLISHED) {
        request->status = RELIEFOS_NET_STATUS_SOCKET_NOT_CONNECTED;
        return 0;
    }
    timeout_ms = net_socket_clamp_timeout(request->timeout_ms);
    data = (const uint8_t *)request->buffer;
    while (request->transferred < request->length) {
        uint32_t chunk = request->length - request->transferred;
        uint32_t seq = socket->local_seq;
        uint32_t target_seq;
        uint64_t start;
        uint64_t next_retransmit;
        uint32_t spins = 0;
        if (chunk > NET_TCP_MSS) {
            chunk = NET_TCP_MSS;
        }
        target_seq = seq + chunk;
        if (net_socket_trace_tls(socket)) {
            console_debug("[net] tls tx socket=%d seq=%u ack=%u bytes=%u target=%u\n",
                           socket->handle, seq, socket->remote_seq, chunk,
                           target_seq);
        }
        if (net_send_tcp_to_mac(socket->dst_mac, net_config.local_ip,
                                socket->remote_ip, socket->local_port,
                                socket->remote_port, seq,
                                socket->remote_seq,
                                TCP_FLAG_PSH | TCP_FLAG_ACK,
                                data + request->transferred, chunk) < 0) {
            request->status = RELIEFOS_NET_STATUS_TX_FAILED;
            return 0;
        }
        socket->local_seq = target_seq;
        socket->tx_bytes += chunk;
        net_socket_touch(socket);
        start = time_uptime_ms();
        next_retransmit = start + NET_TCP_DATA_RETRANSMIT_MS;
        while (!net_timeout_expired(start, timeout_ms, spins++)) {
            uint64_t now;
            net_poll_once(0, 0, 0, 0);
            if (socket->state == RELIEFOS_NET_TCP_CLOSED) {
                request->status = socket->status;
                return 0;
            }
            if (net_tcp_seq_after_or_equal(socket->acked_seq, target_seq)) {
                request->transferred += chunk;
                break;
            }
            now = time_uptime_ms();
            if (now >= next_retransmit) {
                if (net_send_tcp_to_mac(socket->dst_mac, net_config.local_ip,
                                        socket->remote_ip, socket->local_port,
                                        socket->remote_port, seq,
                                        socket->remote_seq,
                                        TCP_FLAG_PSH | TCP_FLAG_ACK,
                                        data + request->transferred,
                                        chunk) < 0) {
                    request->status = RELIEFOS_NET_STATUS_TX_FAILED;
                    return 0;
                }
                next_retransmit = now + NET_TCP_DATA_RETRANSMIT_MS;
            }
            net_cpu_relax();
        }
        if (!net_tcp_seq_after_or_equal(socket->acked_seq, target_seq)) {
            request->status = RELIEFOS_NET_STATUS_TCP_TIMEOUT;
            if (net_socket_trace_tls(socket)) {
                console_warn("[net] tls tx timeout socket=%d target=%u acked=%u state=%u\n",
                               socket->handle, target_seq, socket->acked_seq,
                               socket->state);
            }
            return 0;
        }
    }
    request->status = RELIEFOS_NET_STATUS_OK;
    return 0;
}

/**
 * Net socket recv.
 * @param request Value supplied by the caller.
 * @param owner_pid Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_socket_recv(struct reliefos_net_socket_io *request, uint32_t owner_pid)
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
        request->transferred = copy_len;
        request->status = RELIEFOS_NET_STATUS_OK;
        return 0;
    }
    if (socket->fin_received || socket->state == RELIEFOS_NET_TCP_TIME_WAIT) {
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
 * Net socket close.
 * @param request Value supplied by the caller.
 * @param owner_pid Value supplied by the caller.
 * @return The value or status produced by the operation.
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
    if (socket->state == RELIEFOS_NET_TCP_ESTABLISHED) {
        /**
 * Net socket touch.
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
                                  socket->remote_seq,
                                  TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
        ++socket->local_seq;
        socket->state = RELIEFOS_NET_TCP_TIME_WAIT;
        socket->status = RELIEFOS_NET_STATUS_OK;
        net_socket_touch(socket);
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
 * Net connections.
 * @param request Value supplied by the caller.
 * @param viewer Value supplied by the caller.
 * @return The value or status produced by the operation.
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
    for (uint32_t i = 0; i < RELIEFOS_NET_SOCKET_MAX; ++i) {
        struct net_socket *socket = &net_sockets[i];
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
 * Net close owner sockets.
 * @param owner_pid Value supplied by the caller.
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
        } else if (now >= socket->syn_retransmit) {
            (void)net_send_tcp_to_mac(socket->dst_mac, socket->local_ip, socket->remote_ip,
                socket->local_port, socket->remote_port, socket->local_seq - 1, 0, TCP_FLAG_SYN, 0, 0);
            socket->syn_retransmit = now + NET_TCP_SYN_RETRANSMIT_MS;
        }
    }
    if (socket->state == RELIEFOS_NET_TCP_ESTABLISHED) {
        if ((events & POLLIN) && (socket->rx_len || socket->fin_received || socket->shutdown_read)) result |= POLLIN;
        if (events & POLLOUT) result |= POLLOUT;
    } else if (socket->state == RELIEFOS_NET_TCP_TIME_WAIT) {
        result |= POLLIN | POLLHUP;
    } else if (socket->state == RELIEFOS_NET_TCP_CLOSED) {
        result |= POLLHUP | (events & POLLOUT);
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

int net_socket_shutdown_fd(int32_t handle, int how)
{
    struct net_socket *s = net_socket_find(handle, 0, 1);
    if (!s) return -LINUX_EBADF;
    if (how < 0 || how > 2) return -LINUX_EINVAL;
    if (s->state != RELIEFOS_NET_TCP_ESTABLISHED && s->state != RELIEFOS_NET_TCP_TIME_WAIT)
        return -LINUX_ENOTCONN;
    if (how != 1) { s->shutdown_read = true; s->rx_len = 0; }
    if (how != 0 && !s->shutdown_write) {
        if (net_send_tcp_to_mac(s->dst_mac, s->local_ip, s->remote_ip, s->local_port,
            s->remote_port, s->local_seq, s->remote_seq, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0) < 0)
            return -LINUX_EIO;
        ++s->local_seq;
        s->shutdown_write = true;
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

void net_close_owner_sockets(uint32_t owner_pid)
{
    if (!owner_pid) {
        return;
    }
    for (uint32_t i = 0; i < RELIEFOS_NET_SOCKET_MAX; ++i) {
        struct net_socket *socket = &net_sockets[i];
        if (socket->used && !socket->fd_owned && socket->owner_pid == owner_pid &&
            socket->state != RELIEFOS_NET_TCP_CLOSED) {
            net_socket_mark_closed(socket, RELIEFOS_NET_STATUS_SOCKET_CLOSED);
        }
    }
}
