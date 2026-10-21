#include "../internal.h"



/**
 * Net write eth.
 * @param frame Value supplied by the caller.
 * @param dst_mac Value supplied by the caller.
 * @param type Value supplied by the caller.
 */
void net_write_eth(uint8_t *frame, const uint8_t *dst_mac,
                          uint16_t type)
{
    const uint8_t *src_mac = e1000_mac();
    net_memcpy(frame, dst_mac, 6);
    net_memcpy(frame + 6, src_mac, 6);
    net_put_u16(frame + 12, type);
}

/**
 * Net frame for us.
 * @param frame Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_frame_for_us(const uint8_t *frame)
{
    return net_memeq(frame, e1000_mac(), 6) ||
           net_memeq(frame, net_broadcast_mac, 6);
}
