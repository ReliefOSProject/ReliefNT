#include "../internal.h"

/**
 * Net dhcp add option.
 * @param payload Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param code Value supplied by the caller.
 * @param data Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
static uint32_t net_dhcp_add_option(uint8_t *payload, uint32_t pos,
                                    uint32_t cap, uint8_t code,
                                    const uint8_t *data, uint8_t len);

/**
 * Net dhcp add u8.
 * @param payload Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param code Value supplied by the caller.
 * @param value Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_dhcp_add_u8(uint8_t *payload, uint32_t pos,
                                uint32_t cap, uint8_t code, uint8_t value);

/**
 * Net dhcp add u32.
 * @param payload Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param code Value supplied by the caller.
 * @param value Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_dhcp_add_u32(uint8_t *payload, uint32_t pos,
                                 uint32_t cap, uint8_t code, uint32_t value);

/**
 * Net build dhcp packet.
 * @param payload Value supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @param xid Value supplied by the caller.
 * @param msg_type Value supplied by the caller.
 * @param requested_ip Value supplied by the caller.
 * @param server_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_build_dhcp_packet(uint8_t *payload, uint32_t cap,
                                      uint32_t xid, uint8_t msg_type,
                                      uint32_t requested_ip,
                                      uint32_t server_ip);

/**
 * Net dhcp parse packet.
 * @param payload Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param xid Value supplied by the caller.
 * @param offer Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_dhcp_parse_packet(const uint8_t *payload, uint32_t len,
                                 uint32_t xid, struct net_dhcp_offer *offer);

/**
 * Net dhcp send.
 * @param xid Value supplied by the caller.
 * @param msg_type Value supplied by the caller.
 * @param requested_ip Value supplied by the caller.
 * @param server_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_dhcp_send(uint32_t xid, uint8_t msg_type,
                         uint32_t requested_ip, uint32_t server_ip);

/**
 * Net dhcp wait.
 * @param xid Value supplied by the caller.
 * @param timeout_ms Value supplied by the caller.
 * @param expected_type Value supplied by the caller.
 * @param offer Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_dhcp_wait(uint32_t xid, uint32_t timeout_ms,
                         uint8_t expected_type, struct net_dhcp_offer *offer);

/**
 * Net dhcp request.
 * @param timeout_ms Value supplied by the caller.
 * @param out_config Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_dhcp_request(uint32_t timeout_ms,
                                 struct reliefos_net_config *out_config);


/**
 * Net dhcp add option.
 * @param payload Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param code Value supplied by the caller.
 * @param data Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
static uint32_t net_dhcp_add_option(uint8_t *payload, uint32_t pos,
                                    uint32_t cap, uint8_t code,
                                    const uint8_t *data, uint8_t len)
{
    if (pos + 2u + len > cap) {
        return pos;
    }
    payload[pos++] = code;
    payload[pos++] = len;
    net_memcpy(payload + pos, data, len);
    return pos + len;
}

/**
 * Net dhcp add u8.
 * @param payload Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param code Value supplied by the caller.
 * @param value Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_dhcp_add_u8(uint8_t *payload, uint32_t pos,
                                uint32_t cap, uint8_t code, uint8_t value)
{
    return net_dhcp_add_option(payload, pos, cap, code, &value, 1);
}

/**
 * Net dhcp add u32.
 * @param payload Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param code Value supplied by the caller.
 * @param value Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_dhcp_add_u32(uint8_t *payload, uint32_t pos,
                                 uint32_t cap, uint8_t code, uint32_t value)
{
    uint8_t data[4];
    net_put_u32(data, value);
    return net_dhcp_add_option(payload, pos, cap, code, data, 4);
}

/**
 * Net build dhcp packet.
 * @param payload Value supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @param xid Value supplied by the caller.
 * @param msg_type Value supplied by the caller.
 * @param requested_ip Value supplied by the caller.
 * @param server_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_build_dhcp_packet(uint8_t *payload, uint32_t cap,
                                      uint32_t xid, uint8_t msg_type,
                                      uint32_t requested_ip,
                                      uint32_t server_ip)
{
    uint32_t pos = 240;
    uint8_t params[] = {1, 3, 6, 51, 54};
    uint8_t client_id[7];
    if (cap < NET_DHCP_PACKET_LEN) {
        return 0;
    }
    net_memzero(payload, cap);
    payload[0] = 1;
    payload[1] = 1;
    payload[2] = 6;
    payload[3] = 0;
    net_put_u32(payload + 4, xid);
    net_put_u16(payload + 10, 0x8000u);
    net_memcpy(payload + 28, e1000_mac(), 6);
    net_put_u32(payload + 236, NET_DHCP_MAGIC);
    pos = net_dhcp_add_u8(payload, pos, cap, 53, msg_type);
    client_id[0] = 1;
    net_memcpy(client_id + 1, e1000_mac(), 6);
    pos = net_dhcp_add_option(payload, pos, cap, 61, client_id, sizeof(client_id));
    pos = net_dhcp_add_option(payload, pos, cap, 55, params, sizeof(params));
    if (requested_ip) {
        pos = net_dhcp_add_u32(payload, pos, cap, 50, requested_ip);
    }
    if (server_ip) {
        pos = net_dhcp_add_u32(payload, pos, cap, 54, server_ip);
    }
    if (pos < cap) {
        payload[pos++] = 255;
    }
    return pos < NET_DHCP_PACKET_LEN ? NET_DHCP_PACKET_LEN : pos;
}

/**
 * Net dhcp parse packet.
 * @param payload Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param xid Value supplied by the caller.
 * @param offer Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_dhcp_parse_packet(const uint8_t *payload, uint32_t len,
                                 uint32_t xid, struct net_dhcp_offer *offer)
{
    uint32_t pos;
    if (!payload || !offer || len < 240 ||
        payload[0] != 2 ||
        payload[1] != 1 ||
        payload[2] != 6 ||
        net_get_u32(payload + 4) != xid ||
        !net_memeq(payload + 28, e1000_mac(), 6) ||
        net_get_u32(payload + 236) != NET_DHCP_MAGIC) {
        return -1;
    }
    *offer = (struct net_dhcp_offer){
        .yiaddr = net_get_u32(payload + 16),
    };
    pos = 240;
    while (pos < len) {
        uint8_t code = payload[pos++];
        uint8_t opt_len;
        const uint8_t *opt;
        if (code == 0) {
            continue;
        }
        if (code == 255) {
            break;
        }
        if (pos >= len) return -1;
        opt_len = payload[pos++];
        if (pos + opt_len > len) return -1;
        opt = payload + pos;
        if (code == 53 && opt_len >= 1) {
            offer->msg_type = opt[0];
        } else if (code == 1 && opt_len >= 4) {
            offer->subnet_mask = net_get_u32(opt);
        } else if (code == 3 && opt_len >= 4) {
            offer->router_ip = net_get_u32(opt);
        } else if (code == 6 && opt_len >= 4) {
            offer->dns_ip = net_get_u32(opt);
        } else if (code == 51 && opt_len >= 4) {
            offer->lease_seconds = net_get_u32(opt);
        } else if (code == 54 && opt_len >= 4) {
            offer->server_ip = net_get_u32(opt);
        }
        pos += opt_len;
    }
    return offer->msg_type ? 0 : -1;
}

/**
 * Net dhcp send.
 * @param xid Value supplied by the caller.
 * @param msg_type Value supplied by the caller.
 * @param requested_ip Value supplied by the caller.
 * @param server_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_dhcp_send(uint32_t xid, uint8_t msg_type,
                         uint32_t requested_ip, uint32_t server_ip)
{
    uint8_t payload[NET_DHCP_PACKET_MAX];
    uint32_t len = net_build_dhcp_packet(payload, sizeof(payload), xid,
                                         msg_type, requested_ip, server_ip);
    if (!len) {
        return -1;
    }
    return net_send_udp_to_mac(net_broadcast_mac, 0, 0xffffffffu,
                               NET_DHCP_CLIENT_PORT, NET_DHCP_SERVER_PORT,
                               payload, len);
}

/**
 * Net dhcp wait.
 * @param xid Value supplied by the caller.
 * @param timeout_ms Value supplied by the caller.
 * @param expected_type Value supplied by the caller.
 * @param offer Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_dhcp_wait(uint32_t xid, uint32_t timeout_ms,
                         uint8_t expected_type, struct net_dhcp_offer *offer)
{
    uint8_t payload[NET_DHCP_PACKET_MAX];
    struct net_udp_wait udp_wait;
    uint64_t start = time_uptime_ms();
    uint32_t spins = 0;
    while (!net_timeout_expired(start, timeout_ms, spins++)) {
        udp_wait = (struct net_udp_wait){
            .src_port = NET_DHCP_SERVER_PORT,
            .dst_port = NET_DHCP_CLIENT_PORT,
            .payload = payload,
            .capacity = sizeof(payload),
        };
        net_poll_once(0, 0, &udp_wait, 0);
        if (udp_wait.done &&
            net_dhcp_parse_packet(payload, udp_wait.length, xid, offer) == 0) {
            if (offer->msg_type == expected_type) {
                return 0;
            }
            if (offer->msg_type == NET_DHCP_NAK) {
                return -3;
            }
        }
        net_cpu_relax();
    }
    return -2;
}

/**
 * Net dhcp request.
 * @param timeout_ms Value supplied by the caller.
 * @param out_config Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_dhcp_request(uint32_t timeout_ms,
                                 struct reliefos_net_config *out_config)
{
    struct net_dhcp_offer offer;
    uint32_t xid;
    int ret;
    if (!e1000_is_ready()) {
        if (out_config) {
            *out_config = net_config;
        }
        return RELIEFOS_NET_STATUS_NO_DEVICE;
    }
    timeout_ms = timeout_ms ? timeout_ms : 3000u;
    if (timeout_ms > 10000u) {
        timeout_ms = 10000u;
    }
    xid = 0x4c4e0000u | (net_sequence++ & 0xffffu);
    if (net_dhcp_send(xid, NET_DHCP_DISCOVER, 0, 0) < 0) {
        if (out_config) {
            *out_config = net_config;
        }
        return RELIEFOS_NET_STATUS_TX_FAILED;
    }
    ret = net_dhcp_wait(xid, timeout_ms, NET_DHCP_OFFER, &offer);
    if (ret < 0 || !offer.yiaddr) {
        if (out_config) {
            *out_config = net_config;
        }
        return ret == -2 ? RELIEFOS_NET_STATUS_DHCP_TIMEOUT
                         : RELIEFOS_NET_STATUS_DHCP_FAILED;
    }
    if (net_dhcp_send(xid, NET_DHCP_REQUEST, offer.yiaddr, offer.server_ip) < 0) {
        if (out_config) {
            *out_config = net_config;
        }
        return RELIEFOS_NET_STATUS_TX_FAILED;
    }
    uint32_t offered_ip = offer.yiaddr, offered_server = offer.server_ip;
    ret = net_dhcp_wait(xid, timeout_ms, NET_DHCP_ACK, &offer);
    if (ret < 0 || offer.yiaddr != offered_ip || offer.server_ip != offered_server ||
        !offer.subnet_mask || !offer.lease_seconds) {
        if (out_config) {
            *out_config = net_config;
        }
        return ret == -2 ? RELIEFOS_NET_STATUS_DHCP_TIMEOUT
                         : RELIEFOS_NET_STATUS_DHCP_FAILED;
    }
    net_apply_dhcp_offer(&offer);
    if (out_config) {
        *out_config = net_config;
    }
    return RELIEFOS_NET_STATUS_OK;
}

/**
 * Net dhcp renew.
 * @param request Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_dhcp_renew(struct reliefos_net_dhcp *request)
{
    if (!request) return -LINUX_EINVAL;
    request->status = RELIEFOS_NET_STATUS_PROTOCOL_UNSUPPORTED;
    request->config = net_config;
    return -LINUX_EOPNOTSUPP;
}
