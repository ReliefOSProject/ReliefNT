#include "../internal.h"

/**
 * Net http build request.
 * @param dst Value supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @param host Value supplied by the caller.
 * @param path NUL-terminated text supplied by the caller.
 * @param port Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_http_build_request(char *dst, uint32_t cap,
                                       const char *host, const char *path,
                                       uint32_t port);

/**
 * Net http parse status.
 * @param response Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
static uint32_t net_http_parse_status(const char *response, uint32_t len);

/**
 * Net http exchange ip.
 * @param request Value supplied by the caller.
 * @param http_request Value supplied by the caller.
 * @param http_len Value supplied by the caller.
 * @param remote_ip Value supplied by the caller.
 * @param port Value supplied by the caller.
 * @param timeout_ms Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_http_exchange_ip(struct reliefos_net_http_get *request,
                                     const char *http_request,
                                     uint32_t http_len,
                                     uint32_t remote_ip,
                                     uint32_t port,
                                     uint32_t timeout_ms);


/**
 * Net http resolve hosts.
 * @param host Value supplied by the caller.
 * @param host_len Value supplied by the caller.
 * @param timeout_ms Value supplied by the caller.
 * @param out_ips Value supplied by the caller.
 * @param out_count Value supplied by the caller.
 * @param out_status Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_http_resolve_hosts(const char *host, uint32_t host_len,
                                  uint32_t timeout_ms, uint32_t *out_ips,
                                  uint32_t *out_count,
                                  uint32_t *out_status)
{
    struct reliefos_net_dns dns;
    uint32_t literal_ip;
    if (!out_ips || !out_count) {
        return -1;
    }
    *out_count = 0;
    if (net_parse_ipv4_literal(host, host_len, &literal_ip) == 0) {
        out_ips[0] = literal_ip;
        *out_count = 1;
        return 0;
    }
    dns = (struct reliefos_net_dns){0};
    dns.timeout_ms = timeout_ms;
    dns.status = RELIEFOS_NET_STATUS_DNS_FAILED;
    for (uint32_t i = 0; i < host_len && i + 1u < sizeof(dns.name); ++i) {
        dns.name[i] = host[i];
    }
    if (net_dns_resolve(&dns) < 0) {
        if (out_status) {
            *out_status = RELIEFOS_NET_STATUS_DNS_FAILED;
        }
        return -1;
    }
    if (dns.status != RELIEFOS_NET_STATUS_OK || dns.address_count == 0) {
        if (out_status) {
            *out_status = dns.status;
        }
        return -1;
    }
    for (uint32_t i = 0; i < dns.address_count &&
             *out_count < RELIEFOS_NET_DNS_MAX_ADDRESSES; ++i) {
        if (dns.addresses[i]) {
            out_ips[(*out_count)++] = dns.addresses[i];
        }
    }
    if (*out_count == 0) {
        if (out_status) {
            *out_status = RELIEFOS_NET_STATUS_DNS_NO_ANSWER;
        }
        return -1;
    }
    return 0;
}

/**
 * Net http build request.
 * @param dst Value supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @param host Value supplied by the caller.
 * @param path NUL-terminated text supplied by the caller.
 * @param port Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_http_build_request(char *dst, uint32_t cap,
                                       const char *host, const char *path,
                                       uint32_t port)
{
    uint32_t pos = 0;
    if (!dst || cap == 0 || !host || !host[0]) {
        return 0;
    }
    dst[0] = 0;
    net_append_text(dst, &pos, cap, "GET ");
    if (!path || !path[0]) {
        net_append_char(dst, &pos, cap, '/');
    } else {
        if (path[0] != '/') {
            net_append_char(dst, &pos, cap, '/');
        }
        net_append_text(dst, &pos, cap, path);
    }
    net_append_text(dst, &pos, cap, " HTTP/1.0\r\nHost: ");
    net_append_text(dst, &pos, cap, host);
    if (port && port != NET_HTTP_PORT) {
        net_append_char(dst, &pos, cap, ':');
        net_append_u32(dst, &pos, cap, port);
    }
    net_append_text(dst, &pos, cap,
                    "\r\nConnection: close\r\nUser-Agent: ReliefOS/4\r\n\r\n");
    if (pos + 1u >= cap) {
        return 0;
    }
    return pos;
}

/**
 * Net http parse status.
 * @param response Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
static uint32_t net_http_parse_status(const char *response, uint32_t len)
{
    uint32_t pos = 0;
    uint32_t status = 0;
    if (!response || len < 12) {
        return 0;
    }
    if (response[0] != 'H' || response[1] != 'T' ||
        response[2] != 'T' || response[3] != 'P' ||
        response[4] != '/') {
        return 0;
    }
    while (pos < len && response[pos] != ' ' &&
           response[pos] != '\r' && response[pos] != '\n') {
        ++pos;
    }
    while (pos < len && response[pos] == ' ') {
        ++pos;
    }
    for (uint32_t i = 0; i < 3 && pos < len; ++i, ++pos) {
        if (response[pos] < '0' || response[pos] > '9') {
            return 0;
        }
        status = status * 10u + (uint32_t)(response[pos] - '0');
    }
    return status;
}

/**
 * Net http exchange ip.
 * @param request Value supplied by the caller.
 * @param http_request Value supplied by the caller.
 * @param http_len Value supplied by the caller.
 * @param remote_ip Value supplied by the caller.
 * @param port Value supplied by the caller.
 * @param timeout_ms Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_http_exchange_ip(struct reliefos_net_http_get *request,
                                     const char *http_request,
                                     uint32_t http_len,
                                     uint32_t remote_ip,
                                     uint32_t port,
                                     uint32_t timeout_ms)
{
    uint8_t dst_mac[6];
    struct net_tcp_wait wait;
    uint32_t arp_ip;
    uint32_t syn_seq;
    uint32_t local_seq;
    uint32_t acked_remote_seq;
    uint16_t local_port;
    uint64_t start;
    uint64_t next_retransmit;
    uint32_t spins;
    int ret;

    request->status = RELIEFOS_NET_STATUS_TCP_FAILED;
    request->remote_ip = remote_ip;
    request->http_status = 0;
    request->response_len = 0;
    request->response[0] = 0;

    arp_ip = net_route_arp_ip(remote_ip);
    ret = net_resolve_mac(arp_ip, timeout_ms, dst_mac);
    if (ret < 0) {
        request->status = ret == -1 ? RELIEFOS_NET_STATUS_TX_FAILED
                                    : RELIEFOS_NET_STATUS_ARP_TIMEOUT;
        return request->status;
    }

    local_seq = 0x4c4e0000u ^ (net_sequence++ << 8) ^
                (uint32_t)time_uptime_ms();
    local_port = (uint16_t)(49152u + (local_seq & 0x3fffu));
    syn_seq = local_seq;
    wait = (struct net_tcp_wait){
        .src_ip = remote_ip,
        .src_port = (uint16_t)port,
        .dst_port = local_port,
        .payload = (uint8_t *)request->response,
        .capacity = RELIEFOS_NET_HTTP_RESPONSE_MAX - 1u,
    };

    if (net_send_tcp_to_mac(dst_mac, net_config.local_ip, remote_ip,
                            local_port, (uint16_t)port, local_seq, 0,
                            TCP_FLAG_SYN, 0, 0) < 0) {
        request->status = RELIEFOS_NET_STATUS_TX_FAILED;
        return request->status;
    }
    ++local_seq;
    start = time_uptime_ms();
    next_retransmit = start + NET_TCP_SYN_RETRANSMIT_MS;
    spins = 0;
    while (!net_timeout_expired(start, timeout_ms, spins++)) {
        uint64_t now;
        net_poll_once(0, 0, 0, &wait);
        if (wait.reset) {
            request->status = RELIEFOS_NET_STATUS_TCP_RESET;
            return request->status;
        }
        if ((wait.flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) ==
                (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
            wait.acked_seq == local_seq && wait.remote_seq) {
            break;
        }
        now = time_uptime_ms();
        if (now >= next_retransmit) {
            if (net_send_tcp_to_mac(dst_mac, net_config.local_ip, remote_ip,
                                    local_port, (uint16_t)port, syn_seq, 0,
                                    TCP_FLAG_SYN, 0, 0) < 0) {
                request->status = RELIEFOS_NET_STATUS_TX_FAILED;
                return request->status;
            }
            next_retransmit = now + NET_TCP_SYN_RETRANSMIT_MS;
        }
        net_cpu_relax();
    }
    if (!wait.remote_seq || wait.acked_seq != local_seq) {
        request->status = RELIEFOS_NET_STATUS_TCP_TIMEOUT;
        return request->status;
    }
    if (net_send_tcp_to_mac(dst_mac, net_config.local_ip, remote_ip,
                            local_port, (uint16_t)port, local_seq,
                            wait.remote_seq, TCP_FLAG_ACK, 0, 0) < 0) {
        request->status = RELIEFOS_NET_STATUS_TX_FAILED;
        return request->status;
    }

    if (net_send_tcp_to_mac(dst_mac, net_config.local_ip, remote_ip,
                            local_port, (uint16_t)port, local_seq,
                            wait.remote_seq, TCP_FLAG_PSH | TCP_FLAG_ACK,
                            (const uint8_t *)http_request, http_len) < 0) {
        request->status = RELIEFOS_NET_STATUS_TX_FAILED;
        return request->status;
    }
    local_seq += http_len;
    acked_remote_seq = wait.remote_seq;
    start = time_uptime_ms();
    next_retransmit = start + NET_TCP_DATA_RETRANSMIT_MS;
    spins = 0;
    while (!net_timeout_expired(start, timeout_ms, spins++)) {
        uint32_t before = wait.changed;
        uint64_t now;
        net_poll_once(0, 0, 0, &wait);
        if (wait.reset) {
            request->status = RELIEFOS_NET_STATUS_TCP_RESET;
            return request->status;
        }
        if (wait.changed != before && wait.remote_seq != acked_remote_seq) {
            (void)net_send_tcp_to_mac(dst_mac, net_config.local_ip, remote_ip,
                                      local_port, (uint16_t)port, local_seq,
                                      wait.remote_seq, TCP_FLAG_ACK, 0, 0);
            acked_remote_seq = wait.remote_seq;
        }
        if (wait.overflow) {
            request->status = RELIEFOS_NET_STATUS_HTTP_TOO_LARGE;
            break;
        }
        if (wait.fin) {
            request->status = RELIEFOS_NET_STATUS_OK;
            break;
        }
        now = time_uptime_ms();
        if (wait.length == 0 && !wait.fin && wait.acked_seq != local_seq &&
            now >= next_retransmit) {
            if (net_send_tcp_to_mac(dst_mac, net_config.local_ip, remote_ip,
                                    local_port, (uint16_t)port,
                                    local_seq - http_len, wait.remote_seq,
                                    TCP_FLAG_PSH | TCP_FLAG_ACK,
                                    (const uint8_t *)http_request,
                                    http_len) < 0) {
                request->status = RELIEFOS_NET_STATUS_TX_FAILED;
                break;
            }
            next_retransmit = now + NET_TCP_DATA_RETRANSMIT_MS;
        }
        net_cpu_relax();
    }

    request->response_len = wait.length;
    if (request->response_len >= RELIEFOS_NET_HTTP_RESPONSE_MAX) {
        request->response_len = RELIEFOS_NET_HTTP_RESPONSE_MAX - 1u;
    }
    request->response[request->response_len] = 0;
    if (request->status != RELIEFOS_NET_STATUS_HTTP_TOO_LARGE &&
        request->status != RELIEFOS_NET_STATUS_OK) {
        request->status = wait.length ? RELIEFOS_NET_STATUS_OK
                                      : RELIEFOS_NET_STATUS_TCP_TIMEOUT;
    }
    if (request->status == RELIEFOS_NET_STATUS_OK ||
        request->status == RELIEFOS_NET_STATUS_HTTP_TOO_LARGE) {
        request->http_status =
            net_http_parse_status(request->response, request->response_len);
        if (request->status == RELIEFOS_NET_STATUS_OK && !request->http_status) {
            request->status = RELIEFOS_NET_STATUS_HTTP_FAILED;
        }
    }
    /**
 * Net http get.
 * @param dst_mac Value supplied by the caller.
 * @param local_ip Value supplied by the caller.
 * @param remote_ip Value supplied by the caller.
 * @param local_port Value supplied by the caller.
 * @param port Value supplied by the caller.
 * @param local_seq Value supplied by the caller.
 * @param remote_seq Value supplied by the caller.
 * @param TCP_FLAG_ACK Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
    (void)net_send_tcp_to_mac(dst_mac, net_config.local_ip, remote_ip,
                              local_port, (uint16_t)port, local_seq,
                              wait.remote_seq, TCP_FLAG_FIN | TCP_FLAG_ACK,
                              0, 0);
    return request->status;
}

/**
 * Net http get.
 * @param request Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_http_get(struct reliefos_net_http_get *request)
{
    char http_request[NET_HTTP_REQUEST_MAX];
    uint32_t remote_ips[RELIEFOS_NET_DNS_MAX_ADDRESSES];
    uint32_t remote_count = 0;
    uint32_t timeout_ms;
    uint32_t host_len;
    uint32_t path_len;
    uint32_t port;
    uint32_t http_len;
    uint32_t last_status = RELIEFOS_NET_STATUS_HTTP_FAILED;

    if (!request) {
        return -1;
    }
    host_len = net_strlen(request->host, RELIEFOS_NET_HOSTNAME_LEN);
    path_len = net_strlen(request->path, RELIEFOS_NET_HTTP_PATH_LEN);
    request->status = RELIEFOS_NET_STATUS_HTTP_FAILED;
    request->remote_ip = 0;
    request->http_status = 0;
    request->response_len = 0;
    request->response[0] = 0;
    if (host_len == 0 || host_len >= RELIEFOS_NET_HOSTNAME_LEN ||
        path_len >= RELIEFOS_NET_HTTP_PATH_LEN ||
        request->port > 65535u) {
        request->status = RELIEFOS_NET_STATUS_BAD_ARGUMENT;
        return 0;
    }
    if (!e1000_is_ready()) {
        request->status = RELIEFOS_NET_STATUS_NO_DEVICE;
        return 0;
    }
    timeout_ms = request->timeout_ms ? request->timeout_ms
                                     : NET_HTTP_DEFAULT_TIMEOUT_MS;
    if (timeout_ms > NET_HTTP_MAX_TIMEOUT_MS) {
        timeout_ms = NET_HTTP_MAX_TIMEOUT_MS;
    }
    {
        uint32_t config_status = net_ensure_ipv4_config(timeout_ms, 1);
        if (config_status != RELIEFOS_NET_STATUS_OK) {
            request->status = config_status;
            return 0;
        }
    }
    port = request->port ? request->port : NET_HTTP_PORT;
    http_len = net_http_build_request(http_request, sizeof(http_request),
                                      request->host, request->path, port);
    if (!http_len) {
        request->status = RELIEFOS_NET_STATUS_BAD_ARGUMENT;
        return 0;
    }
    if (net_http_resolve_hosts(request->host, host_len, timeout_ms,
                               remote_ips, &remote_count,
                               &request->status) < 0) {
        return 0;
    }
    for (uint32_t i = 0; i < remote_count; ++i) {
        last_status = net_http_exchange_ip(request, http_request, http_len,
                                          remote_ips[i], port, timeout_ms);
        if (last_status == RELIEFOS_NET_STATUS_OK ||
            last_status == RELIEFOS_NET_STATUS_HTTP_TOO_LARGE ||
            last_status == RELIEFOS_NET_STATUS_HTTP_FAILED ||
            last_status == RELIEFOS_NET_STATUS_BAD_ARGUMENT ||
            last_status == RELIEFOS_NET_STATUS_TX_FAILED ||
            last_status == RELIEFOS_NET_STATUS_ARP_TIMEOUT) {
            return 0;
        }
    }
    request->status = last_status;
    return 0;
}
