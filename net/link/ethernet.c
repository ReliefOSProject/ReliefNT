#include "../internal.h"



/**
 * Net write eth.
 * @param frame Value supplied by the caller.
 * @param dst_mac Value supplied by the caller.
 * @param type Value supplied by the caller.
 */
void net_write_eth_on(uint32_t ifindex, uint8_t *frame, const uint8_t *dst_mac,
                          uint16_t type)
{
    const uint8_t *src_mac = netdev_mac(ifindex);
    net_memcpy(frame, dst_mac, 6);
    net_memcpy(frame + 6, src_mac, 6);
    net_put_u16(frame + 12, type);
}

/**
 * @brief Accept local/broadcast Ethernet and IPv6 multicast for IP-level filtering.
 * @param frame Borrowed Ethernet header of at least 14 bytes.
 * @return Nonzero for a locally receivable destination.
 */
int net_frame_for_us_on(uint32_t ifindex, const uint8_t *frame)
{
    return net_memeq(frame, netdev_mac(ifindex), 6) ||
           net_memeq(frame, net_broadcast_mac, 6) ||
           (frame[0] == 0x33 && frame[1] == 0x33 &&
            net_get_u16(frame + 12) == 0x86dd);
}

/** @brief Construct an Ethernet header for the primary compatibility interface.
 * @param frame Writable header. @param dst_mac Destination. @param type EtherType. */
void net_write_eth(uint8_t *frame, const uint8_t *dst_mac, uint16_t type)
{
    net_write_eth_on(netdev_next(0), frame, dst_mac, type);
}
/** @brief Match a link destination on the primary compatibility interface.
 * @param frame Readable header. @return Nonzero for an accepted destination. */
int net_frame_for_us(const uint8_t *frame)
{
    return net_frame_for_us_on(netdev_next(0), frame);
}
