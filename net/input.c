#include "internal.h"

#define NET_TCP_MAINTENANCE_POLL_BUDGET 4u

static uint64_t net_tcp_maintenance_last_tick;
static bool net_tcp_maintenance_tick_valid;

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
 * @brief Receive one NIC frame, dispatch it, and service a bounded TCP work batch.
 * @param arp_wait Optional synchronous ARP waiter.
 * @param ping_wait Optional synchronous ping waiter.
 * @param udp_wait Optional synchronous UDP waiter.
 * @param tcp_wait Optional synchronous TCP waiter.
 * @note Up to four active sockets are serviced after receive dispatch even when
 *       no frame arrived; inactive registry entries are not scanned.
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
    net_tcp_service_sockets();
}

void net_poll_packets(void)
{
    for (unsigned i = 0; i < 32; ++i) net_poll_once(0, 0, 0, 0);
}

/**
 * @brief Test whether active TCP work is due for its next maintenance batch.
 * @return True when the current tick permits a bounded poll pass.
 * @note Constant-time, requires kernel execution ownership, and never scans
 *       idle sockets.
 */
bool net_tcp_maintenance_due(void)
{
    return net_tcp_maintenance_pending() && net_interface_up &&
        (!net_tcp_maintenance_tick_valid ||
         time_ticks() != net_tcp_maintenance_last_tick);
}

/**
 * @brief Run the bounded timer-paced TCP receive/transmit maintenance pump.
 * @note Called from task-context scheduler boundaries with the kernel execution
 *       transaction held. An active-work hint and tick gate avoid scanning
 *       idle sockets or polling the NIC on every user return.
 */
void net_tcp_maintenance(void)
{
    if (!net_tcp_maintenance_due()) return;
    net_tcp_maintenance_tick_valid = true;
    net_tcp_maintenance_last_tick = time_ticks();

    for (uint32_t i = 0; i < NET_TCP_MAINTENANCE_POLL_BUDGET &&
         net_tcp_maintenance_pending(); ++i) {
        net_poll_once(0, 0, 0, 0);
    }
}
