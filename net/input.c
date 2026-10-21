#include "internal.h"

/**
 * Net process frame.
 * @param frame Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param arp_wait Value supplied by the caller.
 * @param ping_wait Value supplied by the caller.
 * @param udp_wait Value supplied by the caller.
 * @param tcp_wait Value supplied by the caller.
 */
static void net_process_frame(const uint8_t *frame, uint32_t len,
                              struct net_arp_wait *arp_wait,
                              struct net_ping_wait *ping_wait,
                              struct net_udp_wait *udp_wait,
                              struct net_tcp_wait *tcp_wait);


/**
 * Net process frame.
 * @param frame Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param arp_wait Value supplied by the caller.
 * @param ping_wait Value supplied by the caller.
 * @param udp_wait Value supplied by the caller.
 * @param tcp_wait Value supplied by the caller.
 */
static void net_process_frame(const uint8_t *frame, uint32_t len,
                              struct net_arp_wait *arp_wait,
                              struct net_ping_wait *ping_wait,
                              struct net_udp_wait *udp_wait,
                              struct net_tcp_wait *tcp_wait)
{
    uint16_t type;
    if (len < 14 || !net_frame_for_us(frame)) {
        return;
    }
    type = net_get_u16(frame + 12);
    if (type == ETH_TYPE_ARP) {
        net_handle_arp(frame, len, arp_wait);
    } else if (type == ETH_TYPE_IPV4) {
        net_handle_ipv4(frame, len, ping_wait, udp_wait, tcp_wait);
    }
}

/**
 * Net poll once.
 * @param arp_wait Value supplied by the caller.
 * @param ping_wait Value supplied by the caller.
 * @param udp_wait Value supplied by the caller.
 * @param tcp_wait Value supplied by the caller.
 */
void net_poll_once(struct net_arp_wait *arp_wait,
                          struct net_ping_wait *ping_wait,
                          struct net_udp_wait *udp_wait,
                          struct net_tcp_wait *tcp_wait)
{
    uint8_t frame[NET_FRAME_MAX];
    uint32_t len = 0;
    int ret = e1000_poll(frame, sizeof(frame), &len);
    if (ret > 0 && len && net_interface_up) {
        net_packet_input(frame, len);
        net_process_frame(frame, len, arp_wait, ping_wait, udp_wait, tcp_wait);
    }
}

void net_poll_packets(void)
{
    for (unsigned i = 0; i < 32; ++i) net_poll_once(0, 0, 0, 0);
}
