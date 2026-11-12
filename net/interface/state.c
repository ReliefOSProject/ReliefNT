/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/heap.h>
#include <reliefnt/net_device.h>
#include <reliefnt/net_interface.h>
#include <linux/errno.h>

/* All state is serialized by the existing network execution owner. */
static struct net_interface *interfaces;
static struct reliefos_net_config fallback_config;
static bool fallback_up = true;
static bool loopback_up = true;

/* Defined by the IPv4 routing table; remove before releasing interface state. */
void net_ipv4_routes_remove_interface(uint32_t index);

int net_interface_register(uint32_t index)
{
    if (index <= NET_IF_LOOPBACK) return -LINUX_EINVAL;
    struct net_interface **slot = &interfaces;
    while (*slot && (*slot)->index < index) slot = &(*slot)->next;
    if (*slot && (*slot)->index == index) return 0;
    struct net_interface *iface = kernel_malloc(sizeof(*iface));
    if (!iface) return -LINUX_ENOMEM;
    __builtin_memset(iface, 0, sizeof(*iface));
    iface->index = index;
    iface->up = true;
    iface->next = *slot;
    *slot = iface;
    return 0;
}

void net_interface_remove(uint32_t index)
{
    struct net_interface **slot = &interfaces;
    while (*slot && (*slot)->index != index) slot = &(*slot)->next;
    if (!*slot) return;
    struct net_interface *iface = *slot;
    net_ipv4_routes_remove_interface(index);
    *slot = iface->next;
    kernel_free(iface);
}

struct net_interface *net_interface_get(uint32_t index)
{
    for (struct net_interface *iface = interfaces; iface; iface = iface->next)
        if (iface->index == index) return iface;
    return NULL;
}

uint32_t net_interface_next(uint32_t after)
{
    if (after < NET_IF_LOOPBACK) return NET_IF_LOOPBACK;
    for (struct net_interface *iface = interfaces; iface; iface = iface->next)
        if (iface->index > after) return iface->index;
    return 0;
}

bool net_interface_exists(uint32_t index)
{
    return index == NET_IF_LOOPBACK || net_interface_get(index) != NULL;
}

bool net_interface_name(uint32_t index, char out[NET_IF_NAME_LEN])
{
    if (!out || !net_interface_exists(index)) return false;
    __builtin_memset(out, 0, NET_IF_NAME_LEN);
    if (index == NET_IF_LOOPBACK) {
        out[0] = 'l'; out[1] = 'o';
        return true;
    }
    out[0] = 'e'; out[1] = 't'; out[2] = 'h';
    uint32_t value = index - 2, count = 0;
    char digits[10];
    do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    for (uint32_t i = 0; i < count; ++i) out[3 + i] = digits[count - i - 1];
    return true;
}

uint32_t net_interface_index(const char *name)
{
    if (!name) return 0;
    if (!__builtin_strcmp(name, "lo")) return NET_IF_LOOPBACK;
    if (name[0] != 'e' || name[1] != 't' || name[2] != 'h' ||
        name[3] < '0' || name[3] > '9') return 0;
    if (name[3] == '0' && name[4]) return 0;
    uint32_t value = 0, i = 3;
    for (; name[i]; ++i) {
        if (i >= NET_IF_NAME_LEN - 1 || name[i] < '0' || name[i] > '9' ||
            value > (UINT32_MAX - 2u - (uint32_t)(name[i] - '0')) / 10u)
            return 0;
        value = value * 10u + (uint32_t)(name[i] - '0');
    }
    uint32_t index = value + 2u;
    return net_interface_get(index) ? index : 0;
}

bool net_interface_is_up(uint32_t index)
{
    if (index == NET_IF_LOOPBACK) return loopback_up;
    struct net_interface *iface = net_interface_get(index);
    return iface && iface->up;
}

int net_interface_set_up(uint32_t index, bool up)
{
    if (index == NET_IF_LOOPBACK) { loopback_up = up; return 0; }
    struct net_interface *iface = net_interface_get(index);
    if (!iface) return -LINUX_ENODEV;
    iface->up = up;
    return 0;
}

bool net_interface_is_ready(uint32_t index)
{
    if (index == NET_IF_LOOPBACK) return loopback_up;
    struct net_interface *iface = net_interface_get(index);
    return iface && iface->up && netdev_is_ready(index);
}

uint32_t net_interface_for_ipv4(uint32_t ip)
{
    if ((ip >> 24) == 127) return NET_IF_LOOPBACK;
    if (!ip) return 0;
    for (struct net_interface *iface = interfaces; iface; iface = iface->next)
        if (iface->config.local_ip == ip) return iface->index;
    return 0;
}

int net_interface_get_config(uint32_t index, struct reliefos_net_config *out)
{
    if (!out) return -LINUX_EINVAL;
    if (index == NET_IF_LOOPBACK) {
        *out = (struct reliefos_net_config){.local_ip = 0x7f000001u,
            .subnet_mask = 0xff000000u};
        return 0;
    }
    struct net_interface *iface = net_interface_get(index);
    if (!iface) return -LINUX_ENODEV;
    struct e1000_info info = {0};
    netdev_get_info(index, &info);
    *out = iface->config;
    out->flags &= ~(RELIEFOS_NET_CONFIG_FLAG_PRESENT | RELIEFOS_NET_CONFIG_FLAG_ACTIVE |
                    RELIEFOS_NET_CONFIG_FLAG_DHCP);
    if (info.present) out->flags |= RELIEFOS_NET_CONFIG_FLAG_PRESENT;
    if (info.active && iface->up) out->flags |= RELIEFOS_NET_CONFIG_FLAG_ACTIVE;
    if (out->source == RELIEFOS_NET_CONFIG_SOURCE_DHCP) out->flags |= RELIEFOS_NET_CONFIG_FLAG_DHCP;
    __builtin_memcpy(out->mac, info.mac, sizeof(out->mac));
    return 0;
}

struct reliefos_net_config *net_interface_legacy_config(void)
{
    return interfaces ? &interfaces->config : &fallback_config;
}

bool *net_interface_legacy_up(void)
{
    return interfaces ? &interfaces->up : &fallback_up;
}
