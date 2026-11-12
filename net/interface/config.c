#include <reliefnt/kernel_file.h>
#include <reliefnt/socket_usercopy.h>
#include <reliefnt/net_ipv6.h>
#include <linux/ipv6_route.h>
#include "../internal.h"

/**
 * Net line value.
 * @param line Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param key Value supplied by the caller.
 * @param out_value Value supplied by the caller.
 * @param out_len Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_line_value(const char *line, uint32_t len, const char *key,
                          const char **out_value, uint32_t *out_len);

/**
 * Net value eq.
 * @param value Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param expected Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_value_eq(const char *value, uint32_t len, const char *expected);

/**
 * Net parse ipv4.
 * @param text NUL-terminated text supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param out_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_parse_ipv4(const char *text, uint32_t len, uint32_t *out_ip);

/**
 * @brief Parse bounded DNS policy through one retained namespace/open instance.
 * @param path Absolute trusted configuration filename, required.
 * @return Zero or minus one for lookup/type/read failure. Runs synchronously
 * under execution ownership; no bare inode or pathname escapes the reader.
 */
static int net_load_dns_policy_file(const char *path);

/**
 * Net effective dns ip.
 * @param dhcp_dns_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_effective_dns_ip(uint32_t dhcp_dns_ip);

static void net_expire_lease(void);

static uint32_t net_dns_mode = RELIEFOS_NET_DNS_MODE_DHCP;
static uint32_t net_dns_custom_ip;
static uint32_t net_dhcp_dns_ip;
static uint64_t net_dhcp_acquired_ms;
/**
 * Net line value.
 * @param line Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param key Value supplied by the caller.
 * @param out_value Value supplied by the caller.
 * @param out_len Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_line_value(const char *line, uint32_t len, const char *key,
                          const char **out_value, uint32_t *out_len)
{
    uint32_t key_len = net_strlen(key, NET_NETWORK_CONFIG_MAX);
    if (!line || !key || !out_value || !out_len || !key_len || len <= key_len ||
        line[key_len] != '=' || !net_text_eq_len(line, key, key_len)) {
        return 0;
    }
    *out_value = line + key_len + 1u;
    *out_len = len - key_len - 1u;
    return 1;
}

/**
 * Net value eq.
 * @param value Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param expected Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_value_eq(const char *value, uint32_t len, const char *expected)
{
    uint32_t index = 0;
    while (expected && expected[index]) {
        if (index >= len || value[index] != expected[index]) {
            return 0;
        }
        ++index;
    }
    return index == len;
}

/**
 * Net parse ipv4.
 * @param text NUL-terminated text supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param out_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int net_parse_ipv4(const char *text, uint32_t len, uint32_t *out_ip)
{
    uint32_t ip = 0;
    uint32_t pos = 0;
    for (uint32_t octet = 0; octet < 4u; ++octet) {
        uint32_t value = 0;
        uint32_t digits = 0;
        while (pos < len && text[pos] >= '0' && text[pos] <= '9') {
            value = value * 10u + (uint32_t)(text[pos] - '0');
            if (value > 255u || ++digits > 3u) {
                return 0;
            }
            ++pos;
        }
        if (!digits) {
            return 0;
        }
        ip = (ip << 8) | value;
        if (octet != 3u) {
            if (pos >= len || text[pos++] != '.') {
                return 0;
            }
        }
    }
    if (pos != len || ip == 0 || ip == 0xffffffffu) {
        return 0;
    }
    *out_ip = ip;
    return 1;
}

/**
 * @brief Parse bounded DNS policy through one retained namespace/open instance.
 * @param path Absolute trusted configuration filename, required.
 * @return Zero or minus one for lookup/type/read failure. Runs synchronously
 * under execution ownership; no bare inode or pathname escapes the reader.
 */
static int net_load_dns_policy_file(const char *path)
{
    char cfg[NET_NETWORK_CONFIG_MAX];
    uint32_t got = 0;
    uint32_t pos = 0;

    if (vfs_kernel_read(vfs_system_namespace(),path,0,cfg,sizeof(cfg)-1u,&got)<0) {
        return -1;
    }
    cfg[got] = 0;
    while (pos < got) {
        const char *value;
        uint32_t value_len;
        uint32_t start = pos;
        uint32_t line_len;
        while (pos < got && cfg[pos] != '\n' && cfg[pos] != '\r') {
            ++pos;
        }
        line_len = pos - start;
        while (pos < got && (cfg[pos] == '\n' || cfg[pos] == '\r')) {
            ++pos;
        }
        if (net_line_value(cfg + start, line_len, "dns_mode", &value, &value_len)) {
            if (net_value_eq(value, value_len, "dhcp")) {
                net_dns_mode = RELIEFOS_NET_DNS_MODE_DHCP;
            } else if (net_value_eq(value, value_len, "custom")) {
                net_dns_mode = RELIEFOS_NET_DNS_MODE_CUSTOM;
            } else if (net_value_eq(value, value_len, "cloudflare")) {
                net_dns_mode = RELIEFOS_NET_DNS_MODE_CLOUDFLARE;
            }
        } else if (net_line_value(cfg + start, line_len, "dns_custom", &value, &value_len)) {
            uint32_t parsed_ip;
            if (net_parse_ipv4(value, value_len, &parsed_ip)) {
                net_dns_custom_ip = parsed_ip;
            }
        }
    }
    return 0;
}

/**
 * Net load dns policy.
 */
void net_load_dns_policy(void)
{
    net_dns_mode = RELIEFOS_NET_DNS_MODE_DHCP;
    net_dns_custom_ip = 0;
    if (net_load_dns_policy_file(NET_NETWORK_CONFIG_PATH) < 0) {
        (void)net_load_dns_policy_file(NET_NETWORK_CONFIG_BACKUP_PATH);
    }
    if (net_dns_mode == RELIEFOS_NET_DNS_MODE_CUSTOM && !net_dns_custom_ip) {
        net_dns_mode = RELIEFOS_NET_DNS_MODE_CLOUDFLARE;
    }
}

/**
 * Net effective dns ip.
 * @param dhcp_dns_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t net_effective_dns_ip(uint32_t dhcp_dns_ip)
{
    if (net_dns_mode == RELIEFOS_NET_DNS_MODE_CUSTOM && net_dns_custom_ip) {
        return net_dns_custom_ip;
    }
    if (net_dns_mode == RELIEFOS_NET_DNS_MODE_DHCP) {
        return dhcp_dns_ip;
    }
    return RELIEFOS_NET_CLOUDFLARE_DNS_IP;
}

/**
 * Net update config flags.
 */
void net_update_config_flags(void)
{
    uint32_t flags = 0;
    struct e1000_info info;
    e1000_get_info(&info);
    if (info.present) flags |= RELIEFOS_NET_CONFIG_FLAG_PRESENT;
    if (info.active && net_interface_up) flags |= RELIEFOS_NET_CONFIG_FLAG_ACTIVE;
    if (net_config.source == RELIEFOS_NET_CONFIG_SOURCE_DHCP) {
        flags |= RELIEFOS_NET_CONFIG_FLAG_DHCP;
    }
    net_config.flags = flags;
    net_memzero(net_config.mac, sizeof(net_config.mac));
    if (info.present) net_memcpy(net_config.mac, info.mac, 6);
}

/**
 * Net set static fallback.
 */
void net_set_static_fallback(void)
{
    uint32_t index = net_interface_next(NET_IF_LOOPBACK);
    struct net_interface *iface = net_interface_get(index);
    net_arp_cache_clear_on(index);
    if (iface) { iface->broadcast = 0; iface->default_metric = 0; }
    net_dhcp_dns_ip = 0;
    net_dhcp_acquired_ms = 0;
    net_config = (struct reliefos_net_config){
        .flags = 0,
        .source = RELIEFOS_NET_CONFIG_SOURCE_NONE,
        .local_ip = 0,
        .subnet_mask = 0,
        .gateway_ip = 0,
        .dns_ip = net_effective_dns_ip(0),
        .dhcp_server_ip = 0,
        .lease_seconds = 0,
    };
    net_update_config_flags();
}

static void net_expire_lease(void)
{
    if (net_config.source == RELIEFOS_NET_CONFIG_SOURCE_DHCP && net_config.lease_seconds != UINT32_MAX &&
        time_uptime_ms() - net_dhcp_acquired_ms >= (uint64_t)net_config.lease_seconds * 1000)
        net_set_static_fallback();
}

/**
 * Net apply dhcp offer.
 * @param offer Value supplied by the caller.
 */
void net_apply_dhcp_offer(const struct net_dhcp_offer *offer)
{
    uint32_t index = net_interface_next(NET_IF_LOOPBACK);
    struct net_interface *iface = net_interface_get(index);
    net_arp_cache_clear_on(index);
    net_config.local_ip = offer->yiaddr;
    net_config.subnet_mask = offer->subnet_mask ? offer->subnet_mask : RELIEFOS_NET_DEFAULT_SUBNET_MASK;
    if (iface) {
        iface->broadcast = net_config.local_ip | ~net_config.subnet_mask;
        iface->default_metric = 0;
    }
    net_config.gateway_ip = offer->router_ip;
    net_dhcp_dns_ip = offer->dns_ip;
    net_config.dns_ip = net_effective_dns_ip(net_dhcp_dns_ip);
    net_config.dhcp_server_ip = offer->server_ip;
    net_config.lease_seconds = offer->lease_seconds;
    net_dhcp_acquired_ms = time_uptime_ms();
    net_config.source = RELIEFOS_NET_CONFIG_SOURCE_DHCP;
    net_update_config_flags();
}

/**
 * Net route arp ip.
 * @param target_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint32_t net_route_arp_ip(uint32_t target_ip)
{
    if ((target_ip & net_config.subnet_mask) ==
        (net_config.local_ip & net_config.subnet_mask)) {
        return target_ip;
    }
    return net_config.gateway_ip ? net_config.gateway_ip : target_ip;
}

/**
 * Net ensure ipv4 config.
 * @param timeout_ms Value supplied by the caller.
 * @param require_dns Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint32_t net_ensure_ipv4_config(uint32_t timeout_ms, int require_dns)
{
    net_expire_lease();
    if (!e1000_is_ready() || !net_interface_up) {
        return RELIEFOS_NET_STATUS_NO_DEVICE;
    }
    if (net_config.local_ip &&
        (!require_dns || net_config.dns_ip)) {
        return RELIEFOS_NET_STATUS_OK;
    }
    (void)timeout_ms;
    return RELIEFOS_NET_STATUS_NO_ADDRESS;
}

/**
 * Net get config.
 * @param config Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_get_config(struct reliefos_net_config *config)
{
    if (!config) {
        return -1;
    }
    net_expire_lease();
    net_update_config_flags();
    *config = net_config;
    if (config->source == RELIEFOS_NET_CONFIG_SOURCE_DHCP && config->lease_seconds != UINT32_MAX)
        config->lease_seconds -= (uint32_t)((time_uptime_ms() - net_dhcp_acquired_ms) / 1000);
    return 0;
}

/**
 * Net set dns policy.
 * @param request Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_set_dns_policy(struct reliefos_net_dns_policy *request)
{
    uint32_t mode;
    uint32_t custom_dns_ip;
    if (!request) {
        return -1;
    }
    mode = request->mode;
    custom_dns_ip = request->custom_dns_ip;
    request->status = RELIEFOS_NET_STATUS_BAD_ARGUMENT;
    net_update_config_flags();
    if (mode == RELIEFOS_NET_DNS_MODE_QUERY) {
        request->mode = net_dns_mode;
        request->custom_dns_ip = net_dns_custom_ip;
        request->status = RELIEFOS_NET_STATUS_OK;
        request->config = net_config;
        return 0;
    }
    request->config = net_config;
    if (mode > RELIEFOS_NET_DNS_MODE_CUSTOM ||
        (mode == RELIEFOS_NET_DNS_MODE_CUSTOM &&
         (custom_dns_ip == 0 || custom_dns_ip == 0xffffffffu))) {
        return 0;
    }
    net_dns_mode = mode;
    net_dns_custom_ip = mode == RELIEFOS_NET_DNS_MODE_CUSTOM
                            ? custom_dns_ip
                            : 0;
    net_config.dns_ip = net_effective_dns_ip(net_dhcp_dns_ip);
    net_update_config_flags();
    request->mode = net_dns_mode;
    request->custom_dns_ip = net_dns_custom_ip;
    request->status = RELIEFOS_NET_STATUS_OK;
    request->config = net_config;
    return 0;
}

/**
 * Net parse ipv4 literal.
 * @param text NUL-terminated text supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param out_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_parse_ipv4_literal(const char *text, uint32_t len,
                                  uint32_t *out_ip)
{
    uint32_t parts[4] = {0, 0, 0, 0};
    uint32_t part = 0;
    uint32_t value = 0;
    uint32_t digits = 0;
    if (!text || !out_ip || len == 0) {
        return -1;
    }
    for (uint32_t i = 0; i <= len; ++i) {
        char ch = i < len ? text[i] : '.';
        if (ch >= '0' && ch <= '9') {
            value = value * 10u + (uint32_t)(ch - '0');
            if (value > 255u) {
                return -1;
            }
            ++digits;
            continue;
        }
        if (ch != '.' || digits == 0 || part >= 4) {
            return -1;
        }
        parts[part++] = value;
        value = 0;
        digits = 0;
    }
    if (part != 4) {
        return -1;
    }
    *out_ip = (parts[0] << 24) | (parts[1] << 16) |
              (parts[2] << 8) | parts[3];
    return 0;
}

/**
 * Net device info.
 * @param flags Identifier or flags controlling the operation.
 * @param mac_value Value supplied by the caller.
 * @param local_ip Value supplied by the caller.
 */
void net_device_info(uint32_t *flags, uint64_t *mac_value, uint32_t *local_ip)
{
    struct e1000_info info;
    uint64_t mac = 0;
    e1000_get_info(&info);
    for (uint32_t i = 0; i < 6; ++i) {
        mac |= (uint64_t)info.mac[i] << (i * 8u);
    }
    if (flags) {
        *flags = info.present ? RELIEFOS_DEVICE_FLAG_PRESENT : 0;
        if (info.active) {
            *flags |= RELIEFOS_DEVICE_FLAG_ACTIVE;
        }
    }
    if (mac_value) {
        *mac_value = mac;
    }
    if (local_ip) {
        *local_ip = net_config.local_ip;
    }
}

/** @brief Apply Linux IPv6 address and route ioctls for an IPv6 socket.
 * @param request Supported address or route mutation command.
 * @param address User pointer to a native in6_ifreq or in6_rtmsg.
 * @return Zero or negative errno. The current task must hold CAP_NET_ADMIN;
 * imported bytes are owned locally and no user reference crosses a wait.
 */
int net_ipv6_interface_ioctl(uint32_t request, uint64_t address)
{
    struct task *task = sched_current_task();
    if (request != SIOCSIFADDR && request != SIOCDIFADDR &&
        request != SIOCADDRT && request != SIOCDELRT)
        return net_interface_ioctl(request, address);
    if (!task || !(task->cap_effective & (1ULL << CAP_NET_ADMIN))) return -LINUX_EPERM;
    if (request == SIOCSIFADDR || request == SIOCDIFADDR) {
        struct in6_ifreq input;
        int rc = socket_user_import(task, address, &input, sizeof(input));
        if (rc) return rc;
        if (input.ifr6_prefixlen > 128 || input.ifr6_ifindex <= 0) return -LINUX_EINVAL;
        struct net_ipv6_address entry = {.address = input.ifr6_addr,
            .ifindex = (uint32_t)input.ifr6_ifindex, .prefix_length = input.ifr6_prefixlen,
            .flags = NET_IPV6_ADDR_PERMANENT, .preferred_until = UINT64_MAX,
            .valid_until = UINT64_MAX};
        if (entry.address.s6_addr[0] == 0xfe && (entry.address.s6_addr[1] & 0xc0) == 0x80)
            entry.scope = 253;
        else if (!__builtin_memcmp(entry.address.s6_addr,
                    (const uint8_t[16]){0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},16))
            entry.scope = 254;
        return net_ipv6_address_update(&entry, request == SIOCDIFADDR);
    }
    struct in6_rtmsg input;
    int rc = socket_user_import(task, address, &input, sizeof(input));
    if (rc) return rc;
    if (input.rtmsg_dst_len > 128 || input.rtmsg_src_len || input.rtmsg_type ||
        input.rtmsg_info || input.rtmsg_ifindex <= 0 ||
        (input.rtmsg_flags & ~(RTF_UP | RTF_GATEWAY | RTF_HOST))) return -LINUX_EINVAL;
    static const struct in6_addr zero;
    if (__builtin_memcmp(&input.rtmsg_src, &zero, sizeof(zero))) return -LINUX_EOPNOTSUPP;
    if ((input.rtmsg_flags & RTF_GATEWAY) &&
        !__builtin_memcmp(&input.rtmsg_gateway, &zero, sizeof(zero))) return -LINUX_EINVAL;
    struct net_ipv6_route route = {.destination = input.rtmsg_dst, .gateway = input.rtmsg_gateway,
        .prefix_length = input.rtmsg_dst_len, .ifindex = (uint32_t)input.rtmsg_ifindex,
        .metric = input.rtmsg_metric, .flags = input.rtmsg_flags, .valid_until = UINT64_MAX};
    return net_ipv6_route_update(&route, request == SIOCDELRT);
}
