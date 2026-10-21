#include "internal.h"

/**
 * Net log config.
 * @param prefix Value supplied by the caller.
 */
static void net_log_config(const char *prefix);

const uint8_t net_broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

uint32_t net_sequence = 1;

uint16_t net_ipv4_id = 1;

struct reliefos_net_config net_config;

bool net_interface_up = true;

struct net_socket net_sockets[RELIEFOS_NET_SOCKET_MAX];

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
    net_set_static_fallback();
    if (!e1000_is_ready()) {
        console_info("[reliefnt] net unavailable: no active e1000\n");
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
    return e1000_is_ready();
}

/**
 * Net driver detached.
 */
void net_driver_detached(void)
{
    for (unsigned i = 0; i < RELIEFOS_NET_SOCKET_MAX; ++i) {
        struct net_socket *s = &net_sockets[i];
        if (s->used) { s->error = LINUX_ENETDOWN; net_socket_mark_closed(s, RELIEFOS_NET_STATUS_NO_DEVICE); }
    }
    net_arp_cache_clear();
    net_set_static_fallback();
    net_update_config_flags();
}
