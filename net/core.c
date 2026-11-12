#include "internal.h"
#include <reliefnt/net_ipv6.h>

/**
 * Net log config.
 * @param prefix Value supplied by the caller.
 */
static void net_log_config(const char *prefix);

const uint8_t net_broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

uint32_t net_sequence = 1;

uint16_t net_ipv4_id = 1;



struct net_socket *net_sockets;
static bool net_registration_pending;

/**
 * Net log config.
 * @param prefix Value supplied by the caller.
 */
static void net_log_config(const char *prefix)
{
    console_info("%s ip=%u.%u.%u.%u gateway=%u.%u.%u.%u dns=%u.%u.%u.%u\n",
                 prefix,
                 (net_config.local_ip >> 24) & 0xffu,
                 (net_config.local_ip >> 16) & 0xffu,
                 (net_config.local_ip >> 8) & 0xffu,
                 net_config.local_ip & 0xffu,
                 (net_config.gateway_ip >> 24) & 0xffu,
                 (net_config.gateway_ip >> 16) & 0xffu,
                 (net_config.gateway_ip >> 8) & 0xffu,
                 net_config.gateway_ip & 0xffu,
                 (net_config.dns_ip >> 24) & 0xffu,
                 (net_config.dns_ip >> 16) & 0xffu,
                 (net_config.dns_ip >> 8) & 0xffu,
                 net_config.dns_ip & 0xffu);
}

/**
 * Net init.
 */
void net_init(void)
{
    net_load_dns_policy();
    e1000_init();
    net_ipv6_init();
    net_set_static_fallback();
    if (!e1000_is_ready()) {
        console_info("[reliefnt] net unavailable: no active Ethernet link\n");
        return;
    }
    net_log_config("[reliefnt] interface ready; OpenRC DHCP client owns configuration");
}

/**
 * Net is ready.
 * @return The value or status produced by the operation.
 */
int net_is_ready(void)
{
    for (uint32_t index = netdev_next(0); index; index = netdev_next(index))
        if (net_interface_is_ready(index)) return 1;
    return 0;
}

/**
 * @brief Close TCP sockets and reset cached link configuration after NIC removal.
 * @note Every socket waiter is woken before the network stack stops using its state.
 */
void net_driver_detached(void)
{
    /* Compatibility entry point for callers retiring the primary NIC. */
    uint32_t index = netdev_next(0);
    if (index) net_device_detached(index);
}

/** @brief Publish network configuration for a newly admitted hardware device.
 * @param ifindex Stable index owned by the device registry. Allocation failure
 * leaves the device unconfigured and is reported in the kernel log. */
void net_device_attached(uint32_t ifindex)
{
    int result = net_interface_register(ifindex);
    if (result < 0) {
        net_registration_pending = true;
        console_error("[reliefnt] network interface %u registration failed: %d\n", ifindex, result);
        return;
    }
    net_ipv6_link_changed_on(ifindex, net_interface_is_up(ifindex));
    netlink_ipv6_changed();
}

/** @brief Retire sockets and discovery state belonging to one removed device.
 * @param ifindex Non-reused hardware interface index. Other interfaces retain
 * their addresses, routes, neighbors and connections. Execution ownership held. */
void net_device_detached(uint32_t ifindex)
{
    for (struct net_socket *s = net_sockets; s; s = s->registry_next) {
        if (s->used && s->ifindex == ifindex) {
            s->error = LINUX_ENETDOWN;
            net_socket_mark_closed(s, RELIEFOS_NET_STATUS_NO_DEVICE);
        }
    }
    net_udp_device_detached(ifindex);
    net_packet_device_detached(ifindex);
    net_ipv6_detached_on(ifindex);
    net_arp_cache_clear_on(ifindex);
    net_interface_remove(ifindex);
    netlink_ipv6_changed();
}

/** @brief Report whether a failed device-state allocation needs another pass.
 * @return True while the bounded maintenance pump must retry registration. */
bool net_device_registration_pending(void)
{
    return net_registration_pending;
}
/** @brief Retry one missing device's network state after transient allocation failure.
 * Failed retries remain silent. Success enables discovery, and a subsequent
 * scan clears the retry hint after every published NIC has network state. */
void net_device_retry_registration(void)
{
    if (!net_registration_pending) return;
    for (uint32_t index = netdev_next(0); index; index = netdev_next(index)) {
        if (net_interface_get(index)) continue;
        if (!net_interface_register(index)) {
            net_ipv6_link_changed_on(index, net_interface_is_up(index));
            netlink_ipv6_changed();
        }
        return;
    }
    net_registration_pending = false;
}
