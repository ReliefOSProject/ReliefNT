#include "../internal.h"

/**
 * Net send icmp echo request.
 * @param dst_mac Value supplied by the caller.
 * @param target_ip Value supplied by the caller.
 * @param ident Value supplied by the caller.
 * @param sequence Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_send_icmp_echo_request(const uint8_t *dst_mac, uint32_t target_ip,
                                      uint16_t ident, uint16_t sequence);


/**
 * Net send icmp echo request.
 * @param dst_mac Value supplied by the caller.
 * @param target_ip Value supplied by the caller.
 * @param ident Value supplied by the caller.
 * @param sequence Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_send_icmp_echo_request(const uint8_t *dst_mac, uint32_t target_ip,
                                      uint16_t ident, uint16_t sequence)
{
    uint8_t frame[98];
    uint8_t *ip = frame + 14;
    uint8_t *icmp = frame + 34;
    uint16_t icmp_len = 8u + NET_ICMP_PAYLOAD_LEN;
    uint16_t total_len = 20u + icmp_len;

    net_memzero(frame, sizeof(frame));
    net_write_eth(frame, dst_mac, ETH_TYPE_IPV4);
    ip[0] = 0x45;
    ip[1] = 0;
    net_put_u16(ip + 2, total_len);
    net_put_u16(ip + 4, net_ipv4_id++);
    net_put_u16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = IPV4_PROTO_ICMP;
    net_put_u32(ip + 12, net_config.local_ip);
    net_put_u32(ip + 16, target_ip);
    net_put_u16(ip + 10, net_checksum(ip, 20));

    icmp[0] = ICMP_ECHO_REQUEST;
    icmp[1] = 0;
    net_put_u16(icmp + 4, ident);
    net_put_u16(icmp + 6, sequence);
    for (uint32_t i = 0; i < NET_ICMP_PAYLOAD_LEN; ++i) {
        icmp[8 + i] = (uint8_t)('A' + (i % 26));
    }
    net_put_u16(icmp + 2, net_checksum(icmp, icmp_len));
    return net_interface_up ? e1000_send(frame, 14u + total_len) : -LINUX_ENETDOWN;
}

/**
 * Net ping.
 * @param request Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_ping(struct reliefos_net_ping *request)
{
    uint8_t next_hop_mac[6];
    struct net_ping_wait wait;
    uint32_t timeout_ms;
    uint32_t target_ip;
    uint32_t arp_ip;
    uint16_t sequence;
    uint64_t start;
    uint32_t spins = 0;

    if (!request || request->target_ip == 0 || request->target_ip == 0xffffffffu) {
        if (request) {
            request->status = RELIEFOS_NET_STATUS_BAD_ARGUMENT;
        }
        return 0;
    }
    request->sent = 0;
    request->received = 0;
    request->rtt_ms = 0;
    if (!e1000_is_ready()) {
        request->status = RELIEFOS_NET_STATUS_NO_DEVICE;
        return 0;
    }

    timeout_ms = request->timeout_ms ? request->timeout_ms : RELIEFOS_NET_DEFAULT_TIMEOUT_MS;
    if (timeout_ms > RELIEFOS_NET_MAX_TIMEOUT_MS) {
        timeout_ms = RELIEFOS_NET_MAX_TIMEOUT_MS;
    }
    {
        uint32_t lease_status = net_ensure_ipv4_config(timeout_ms, 0);
        if (lease_status != RELIEFOS_NET_STATUS_OK) {
            request->status = lease_status;
            return 0;
        }
    }
    target_ip = request->target_ip;
    arp_ip = net_route_arp_ip(target_ip);
    {
        int arp_ret = net_resolve_mac(arp_ip, timeout_ms, next_hop_mac);
        if (arp_ret < 0) {
            request->status = arp_ret == -1
                                  ? RELIEFOS_NET_STATUS_TX_FAILED
                                  : RELIEFOS_NET_STATUS_ARP_TIMEOUT;
            return 0;
        }
    }

    sequence = (uint16_t)(request->sequence ? request->sequence : net_sequence++);
    wait = (struct net_ping_wait){
        .target_ip = target_ip,
        .ident = 0x4c4e,
        .sequence = sequence,
        .done = 0,
    };
    start = time_uptime_ms();
    if (net_send_icmp_echo_request(next_hop_mac, target_ip, wait.ident, wait.sequence) < 0) {
        request->status = RELIEFOS_NET_STATUS_TX_FAILED;
        return 0;
    }
    request->sent = 1;
    while (!net_timeout_expired(start, timeout_ms, spins++)) {
        net_poll_once(0, &wait, 0, 0);
        if (wait.done) {
            uint64_t now = time_uptime_ms();
            request->received = 1;
            request->rtt_ms = (uint32_t)(now >= start ? now - start : 0);
            request->status = RELIEFOS_NET_STATUS_OK;
            return 0;
        }
        net_cpu_relax();
    }
    request->status = RELIEFOS_NET_STATUS_ECHO_TIMEOUT;
    return 0;
}
