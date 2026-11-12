/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_NET_DEVICE_H
#define RELIEFNT_NET_DEVICE_H

#include <reliefnt/e1000.h>

/* Private kernel/module contract.  This is independent of the user ABI. */
#define RELIEFNT_NETDEV_OPS_VERSION 1u

struct net_proc_counters;

struct reliefnt_netdev_ops {
    uint32_t version, struct_size;
    int (*is_ready)(void *context);
    const uint8_t *(*mac)(void *context);
    int (*send)(void *context, const void *frame, uint32_t length);
    int (*poll)(void *context, void *frame, uint32_t capacity, uint32_t *out_length);
    void (*get_info)(void *context, struct e1000_info *out);
};

/** @brief Register one NIC owned by the currently initializing driver.
 * @param ops Immutable v1 callbacks retained until the owner is stopped.
 * @param context Driver-owned per-device state retained through callback drain.
 * @param out_ifindex Receives a positive, never-reused interface index.
 * @return Zero, -EINVAL, -ENOMEM or -EOVERFLOW. Init task context; the
 * registration becomes visible only after successful owner initialization.
 */
int driver_register_netdev(const struct reliefnt_netdev_ops *ops, void *context,
                           uint32_t *out_ifindex);

/** @brief Return the smallest live NIC index greater than after.
 * @param after Previous index, or zero. @return The next index or zero.
 */
uint32_t netdev_next(uint32_t after);
/** @brief Query carrier readiness of the exact NIC.
 * @param ifindex Stable NIC index. @return Nonzero only when ready.
 */
int netdev_is_ready(uint32_t ifindex);
/** @brief Return the NIC MAC in core-owned storage.
 * @param ifindex Stable NIC index. @return Six bytes, or a zero address when
 * absent. Network execution ownership keeps the returned bytes stable.
 */
const uint8_t *netdev_mac(uint32_t ifindex);
/** @brief Send one Ethernet frame through the exact NIC.
 * @param ifindex Stable NIC index. @param frame Borrowed bytes.
 * @param length Number of bytes. @return Zero or negative Linux errno.
 */
int netdev_send(uint32_t ifindex, const void *frame, uint32_t length);
/** @brief Receive one frame from the exact NIC.
 * @param ifindex Stable NIC index. @param frame Writable buffer.
 * @param capacity Buffer bytes. @param out_length Optional returned size.
 * @return One frame, zero if empty, or negative Linux errno.
 */
int netdev_poll(uint32_t ifindex, void *frame, uint32_t capacity, uint32_t *out_length);
/** @brief Copy an exact NIC identity, clearing the output if absent.
 * @param ifindex Stable NIC index. @param out Caller-owned nullable output.
 */
void netdev_get_info(uint32_t ifindex, struct e1000_info *out);
/** @brief Copy counters observed for the exact NIC, or zeros if absent.
 * @param ifindex Stable NIC index. @param out Caller-owned nullable output.
 */
void netdev_counters(uint32_t ifindex, struct net_proc_counters *out);

/** @brief Notify the network stack after a NIC becomes visible.
 * @param ifindex Newly published stable index. Caller owns network execution.
 */
void net_device_attached(uint32_t ifindex);
/** @brief Notify the network stack after a NIC has been removed.
 * @param ifindex Removed stable index. Caller owns network execution.
 */
void net_device_detached(uint32_t ifindex);

#endif
