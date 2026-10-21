#include "../internal.h"

/**
 * Net dns encode name.
 * @param name NUL-terminated text supplied by the caller.
 * @param out Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param out_len Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_dns_encode_name(const char *name, uint8_t *out,
                               uint32_t cap, uint32_t *out_len);

/**
 * Net dns skip name.
 * @param packet Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param pos Output storage updated by the function.
 * @return The value or status produced by the operation.
 */
static int net_dns_skip_name(const uint8_t *packet, uint32_t len, uint32_t *pos);

/**
 * Net dns build query.
 * @param packet Value supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @param name NUL-terminated text supplied by the caller.
 * @param ident Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_dns_build_query(uint8_t *packet, uint32_t cap,
                                    const char *name, uint16_t ident);

/**
 * Net dns parse response.
 * @param packet Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param ident Value supplied by the caller.
 * @param request Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_dns_parse_response(const uint8_t *packet, uint32_t len,
                                  uint16_t ident, struct reliefos_net_dns *request);


/**
 * Net dns encode name.
 * @param name NUL-terminated text supplied by the caller.
 * @param out Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param out_len Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_dns_encode_name(const char *name, uint8_t *out,
                               uint32_t cap, uint32_t *out_len)
{
    uint32_t label_start = 0;
    uint32_t len = net_strlen(name, RELIEFOS_NET_HOSTNAME_LEN);
    uint32_t pos = 0;
    if (!name || !out || !out_len || len == 0 || len >= RELIEFOS_NET_HOSTNAME_LEN) {
        return -1;
    }
    for (uint32_t i = 0; i <= len; ++i) {
        if (name[i] == '.' || name[i] == 0) {
            uint32_t label_len = i - label_start;
            if (label_len == 0 || label_len > 63 || pos + label_len + 1 >= cap) {
                return -1;
            }
            out[pos++] = (uint8_t)label_len;
            net_memcpy(out + pos, name + label_start, label_len);
            pos += label_len;
            label_start = i + 1;
        }
    }
    if (pos + 1 > cap) {
        return -1;
    }
    out[pos++] = 0;
    *out_len = pos;
    return 0;
}

/**
 * Net dns skip name.
 * @param packet Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param pos Output storage updated by the function.
 * @return The value or status produced by the operation.
 */
static int net_dns_skip_name(const uint8_t *packet, uint32_t len, uint32_t *pos)
{
    uint32_t p;
    if (!packet || !pos) {
        return -1;
    }
    p = *pos;
    while (p < len) {
        uint8_t label = packet[p];
        if (label == 0) {
            *pos = p + 1;
            return 0;
        }
        if ((label & 0xc0u) == 0xc0u) {
            if (p + 2 > len) {
                return -1;
            }
            *pos = p + 2;
            return 0;
        }
        if ((label & 0xc0u) != 0 || p + 1u + label > len) {
            return -1;
        }
        p += 1u + label;
    }
    return -1;
}

/**
 * Net dns build query.
 * @param packet Value supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @param name NUL-terminated text supplied by the caller.
 * @param ident Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_dns_build_query(uint8_t *packet, uint32_t cap,
                                    const char *name, uint16_t ident)
{
    uint32_t qname_len = 0;
    uint32_t pos = 12;
    if (cap < 32) {
        return 0;
    }
    net_memzero(packet, cap);
    if (net_dns_encode_name(name, packet + pos, cap - pos, &qname_len) < 0) {
        return 0;
    }
    net_put_u16(packet, ident);
    net_put_u16(packet + 2, 0x0100u);
    net_put_u16(packet + 4, 1);
    pos = 12 + qname_len;
    net_put_u16(packet + pos, 1);
    net_put_u16(packet + pos + 2, 1);
    return pos + 4;
}

/**
 * Net dns parse response.
 * @param packet Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param ident Value supplied by the caller.
 * @param request Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_dns_parse_response(const uint8_t *packet, uint32_t len,
                                  uint16_t ident, struct reliefos_net_dns *request)
{
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint32_t pos = 12;
    if (!packet || !request || len < 12 || net_get_u16(packet) != ident) {
        return -1;
    }
    flags = net_get_u16(packet + 2);
    if ((flags & 0x8000u) == 0 || (flags & 0x000fu) != 0) {
        return -1;
    }
    qdcount = net_get_u16(packet + 4);
    ancount = net_get_u16(packet + 6);
    for (uint32_t i = 0; i < qdcount; ++i) {
        if (net_dns_skip_name(packet, len, &pos) < 0 || pos + 4 > len) {
            return -1;
        }
        pos += 4;
    }
    request->address_count = 0;
    for (uint32_t i = 0; i < ancount && pos < len; ++i) {
        uint16_t type;
        uint16_t cls;
        uint16_t rdlen;
        if (net_dns_skip_name(packet, len, &pos) < 0 || pos + 10 > len) {
            return -1;
        }
        type = net_get_u16(packet + pos);
        cls = net_get_u16(packet + pos + 2);
        rdlen = net_get_u16(packet + pos + 8);
        pos += 10;
        if (pos + rdlen > len) {
            return -1;
        }
        if (type == 1 && cls == 1 && rdlen == 4 &&
            request->address_count < RELIEFOS_NET_DNS_MAX_ADDRESSES) {
            request->addresses[request->address_count++] = net_get_u32(packet + pos);
        }
        pos += rdlen;
    }
    return request->address_count ? 0 : -2;
}

/**
 * Net dns resolve.
 * @param request Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_dns_resolve(struct reliefos_net_dns *request)
{
    uint8_t query[NET_DNS_PACKET_MAX];
    uint8_t response[NET_DNS_PACKET_MAX];
    uint8_t dst_mac[6];
    struct net_udp_wait udp_wait;
    uint32_t timeout_ms;
    uint32_t query_len;
    uint32_t arp_ip;
    uint16_t ident;
    uint16_t local_port;
    uint64_t start;
    uint32_t spins = 0;
    int ret;

    if (!request || !request->name[0]) {
        if (request) {
            request->status = RELIEFOS_NET_STATUS_BAD_ARGUMENT;
        }
        return 0;
    }
    request->status = RELIEFOS_NET_STATUS_DNS_FAILED;
    request->address_count = 0;
    for (uint32_t i = 0; i < RELIEFOS_NET_DNS_MAX_ADDRESSES; ++i) {
        request->addresses[i] = 0;
    }
    if (!e1000_is_ready()) {
        request->status = RELIEFOS_NET_STATUS_NO_DEVICE;
        return 0;
    }
    timeout_ms = request->timeout_ms ? request->timeout_ms : 3000u;
    if (timeout_ms > 10000u) {
        timeout_ms = 10000u;
    }
    {
        uint32_t lease_status = net_ensure_ipv4_config(timeout_ms, 1);
        if (lease_status != RELIEFOS_NET_STATUS_OK) {
            request->status = lease_status;
            return 0;
        }
    }
    if (!net_config.dns_ip) {
        request->status = RELIEFOS_NET_STATUS_DNS_FAILED;
        return 0;
    }
    ident = (uint16_t)(net_sequence++ & 0xffffu);
    local_port = (uint16_t)(49152u + (ident & 0x3fffu));
    query_len = net_dns_build_query(query, sizeof(query), request->name, ident);
    if (!query_len) {
        request->status = RELIEFOS_NET_STATUS_BAD_ARGUMENT;
        return 0;
    }
    arp_ip = net_route_arp_ip(net_config.dns_ip);
    ret = net_resolve_mac(arp_ip, timeout_ms, dst_mac);
    if (ret < 0) {
        request->status = ret == -1 ? RELIEFOS_NET_STATUS_TX_FAILED
                                    : RELIEFOS_NET_STATUS_ARP_TIMEOUT;
        return 0;
    }
    if (net_send_udp_to_mac(dst_mac, net_config.local_ip, net_config.dns_ip,
                            local_port, NET_DNS_PORT, query, query_len) < 0) {
        request->status = RELIEFOS_NET_STATUS_TX_FAILED;
        return 0;
    }
    start = time_uptime_ms();
    while (!net_timeout_expired(start, timeout_ms, spins++)) {
        udp_wait = (struct net_udp_wait){
            .src_ip = net_config.dns_ip,
            .src_port = NET_DNS_PORT,
            .dst_port = local_port,
            .payload = response,
            .capacity = sizeof(response),
        };
        net_poll_once(0, 0, &udp_wait, 0);
        if (udp_wait.done) {
            ret = net_dns_parse_response(response, udp_wait.length, ident, request);
            request->status = ret == 0 ? RELIEFOS_NET_STATUS_OK
                                       : RELIEFOS_NET_STATUS_DNS_NO_ANSWER;
            return 0;
        }
        net_cpu_relax();
    }
    request->status = RELIEFOS_NET_STATUS_DNS_TIMEOUT;
    return 0;
}
