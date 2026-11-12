/*
 * Compatibility wrappers for the first published Ethernet controller.
 * New networking code should address a specific NIC through net_device.h.
 */
#ifndef RELIEFNT_E1000_H
#define RELIEFNT_E1000_H

#include <reliefnt/types.h>

struct e1000_info {
    uint32_t present;
    uint32_t active;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t reserved;
    uint8_t mac[6];
};

/**
 * @brief Retained startup hook; drivers initialize their own controllers.
 */
void e1000_init(void);
/**
 * @brief Return the first published NIC's current carrier status.
 * @return Nonzero when carrier is available; zero otherwise.
 */
int e1000_is_ready(void);
/** @brief Borrow the first published NIC's core-owned station address.
 * @return Six bytes or the zero address. Caller holds network execution ownership.
 */
const uint8_t *e1000_mac(void);
/**
 * @brief Transmit through the first published NIC.
 * @param frame Borrowed frame bytes. @param len Number of bytes.
 * @return Zero or a negative Linux errno, including -ENODEV when absent.
 */
int e1000_send(const void *frame, uint32_t len);
/**
 * @brief Receive a packet from the first published NIC.
 * @param frame Writable buffer. @param capacity Buffer bytes.
 * @param out_len Optional frame length output.
 * @return One frame, zero if empty, or a negative Linux errno.
 */
int e1000_poll(void *frame, uint32_t capacity, uint32_t *out_len);
/**
 * @brief Copy the first published NIC's identity, or zeros when absent.
 * @param info Nullable caller-owned identity output.
 */
void e1000_get_info(struct e1000_info *info);

#endif
