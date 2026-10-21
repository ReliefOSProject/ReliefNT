#include "../internal.h"



/**
 * Net ntp sync.
 * @param request Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_ntp_sync(struct reliefos_time_sync *request)
{
    struct reliefos_net_dns dns;
    struct net_udp_wait udp_wait;
    uint8_t packet[NET_NTP_PACKET_LEN];
    uint8_t response[NET_NTP_PACKET_LEN];
    uint8_t dst_mac[6];
    uint32_t timeout_ms;
    uint32_t arp_ip;
    uint16_t local_port;
    uint64_t start;
    uint32_t spins = 0;
    int ret;
    if (!request) {
        return -1;
    }
    request->status = RELIEFOS_NET_STATUS_DNS_FAILED;
    request->server_ip = 0;
    request->valid = 0;
    request->unix_seconds = 0;
    if (!request->server[0]) {
        const char *fallback = "pool.ntp.org";
        uint32_t i = 0;
        while (fallback[i] && i + 1U < sizeof(request->server)) {
            request->server[i] = fallback[i];
            ++i;
        }
        request->server[i] = 0;
    }
    timeout_ms = request->timeout_ms ? request->timeout_ms : 4000u;
    if (timeout_ms > 10000u) {
        timeout_ms = 10000u;
    }
    dns = (struct reliefos_net_dns){0};
    for (uint32_t i = 0; request->server[i] && i + 1U < sizeof(dns.name); ++i) {
        dns.name[i] = request->server[i];
    }
    dns.timeout_ms = timeout_ms;
    (void)net_dns_resolve(&dns);
    if (dns.status != RELIEFOS_NET_STATUS_OK || !dns.address_count ||
        !dns.addresses[0]) {
        request->status = dns.status;
        return 0;
    }
    request->server_ip = dns.addresses[0];
    arp_ip = net_route_arp_ip(request->server_ip);
    ret = net_resolve_mac(arp_ip, timeout_ms, dst_mac);
    if (ret < 0) {
        request->status = ret == -1 ? RELIEFOS_NET_STATUS_TX_FAILED
                                    : RELIEFOS_NET_STATUS_ARP_TIMEOUT;
        return 0;
    }
    net_memzero(packet, sizeof(packet));
    packet[0] = 0x23u;
    local_port = (uint16_t)(49152u + ((net_sequence++ >> 1U) & 0x3fffu));
    if (net_send_udp_to_mac(dst_mac, net_config.local_ip, request->server_ip,
                            local_port, NET_NTP_PORT, packet,
                            sizeof(packet)) < 0) {
        request->status = RELIEFOS_NET_STATUS_TX_FAILED;
        return 0;
    }
    start = time_uptime_ms();
    while (!net_timeout_expired(start, timeout_ms, spins++)) {
        udp_wait = (struct net_udp_wait){
            .src_ip = request->server_ip,
            .src_port = NET_NTP_PORT,
            .dst_port = local_port,
            .payload = response,
            .capacity = sizeof(response),
        };
        net_poll_once(0, 0, &udp_wait, 0);
        if (udp_wait.done) {
            uint64_t ntp_seconds;
            if (udp_wait.length < NET_NTP_PACKET_LEN ||
                ((response[0] & 7u) != 4u && (response[0] & 7u) != 5u) ||
                response[1] == 0 || response[1] > 15u) {
                request->status = RELIEFOS_NET_STATUS_HTTP_FAILED;
                return 0;
            }
            ntp_seconds = net_get_u32(response + 40);
            if (ntp_seconds < NET_NTP_UNIX_EPOCH_OFFSET ||
                time_set_wall_clock(ntp_seconds - NET_NTP_UNIX_EPOCH_OFFSET) < 0) {
                request->status = RELIEFOS_NET_STATUS_HTTP_FAILED;
                return 0;
            }
            request->unix_seconds = ntp_seconds - NET_NTP_UNIX_EPOCH_OFFSET;
            request->valid = 1;
            request->status = RELIEFOS_NET_STATUS_OK;
            return 0;
        }
        net_cpu_relax();
    }
    request->status = RELIEFOS_NET_STATUS_DNS_TIMEOUT;
    return 0;
}
