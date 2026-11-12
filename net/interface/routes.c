/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/heap.h>
#include <reliefnt/net_interface.h>
#include <linux/errno.h>
#include <linux/ipv6_route.h>

struct ipv4_route_node {
    struct ipv4_route_node *next;
    struct net_ipv4_route route;
};
static struct ipv4_route_node *ipv4_routes;

/** @brief Remove all explicitly configured routes belonging to a detached NIC. */
void net_ipv4_routes_remove_interface(uint32_t index)
{
    struct ipv4_route_node **slot = &ipv4_routes;
    while (*slot) {
        struct ipv4_route_node *entry = *slot;
        if (entry->route.ifindex != index) { slot = &entry->next; continue; }
        *slot = entry->next;
        kernel_free(entry);
    }
}

/** @brief Append a route to an optional bounded snapshot. */
static void ipv4_snapshot_add(struct net_ipv4_route *out, uint32_t capacity,
                              uint32_t *count, struct net_ipv4_route route)
{
    if (out && *count < capacity) out[*count] = route;
    ++*count;
}

uint32_t net_ipv4_routes(struct net_ipv4_route *out, uint32_t capacity)
{
    uint32_t count = 0;
    ipv4_snapshot_add(out, capacity, &count, (struct net_ipv4_route){
        .destination = 0x7f000000u, .mask = 0xff000000u,
        .ifindex = NET_IF_LOOPBACK, .flags = RTF_UP});
    for (uint32_t index = net_interface_next(NET_IF_LOOPBACK); index;
         index = net_interface_next(index)) {
        struct net_interface *iface = net_interface_get(index);
        if (!iface || !iface->config.local_ip) continue;
        ipv4_snapshot_add(out, capacity, &count, (struct net_ipv4_route){
            .destination = iface->config.local_ip & iface->config.subnet_mask,
            .mask = iface->config.subnet_mask, .ifindex = index, .flags = RTF_UP});
        if (iface->config.gateway_ip)
            ipv4_snapshot_add(out, capacity, &count, (struct net_ipv4_route){
                .gateway = iface->config.gateway_ip, .ifindex = index,
                .metric = iface->default_metric, .flags = RTF_UP | RTF_GATEWAY});
    }
    for (struct ipv4_route_node *entry = ipv4_routes; entry; entry = entry->next)
        ipv4_snapshot_add(out, capacity, &count, entry->route);
    return count;
}

/** @brief Replace the current candidate when the route has a better priority. */
static void ipv4_consider(const struct net_ipv4_route *candidate, uint32_t destination,
                          uint32_t scope, struct net_ipv4_route *best, bool *found)
{
    if ((scope && candidate->ifindex != scope) ||
        (destination & candidate->mask) != candidate->destination ||
        !net_interface_is_ready(candidate->ifindex)) return;
    if (!*found || candidate->mask > best->mask ||
        (candidate->mask == best->mask && candidate->metric < best->metric) ||
        (candidate->mask == best->mask && candidate->metric == best->metric &&
         candidate->ifindex < best->ifindex)) {
        *best = *candidate;
        *found = true;
    }
}

int net_ipv4_route_lookup(uint32_t destination, uint32_t source, uint32_t scope,
                          struct net_ipv4_path *out)
{
    if (!out) return -LINUX_EINVAL;
    if (scope && !net_interface_exists(scope)) return -LINUX_ENODEV;
    if (source && !net_interface_for_ipv4(source)) return -LINUX_EADDRNOTAVAIL;

    uint32_t local = net_interface_for_ipv4(destination);
    if (local) {
        if (scope && scope != local) return -LINUX_ENETUNREACH;
        if (!net_interface_is_up(local)) return -LINUX_ENETDOWN;
        *out = (struct net_ipv4_path){.source = source ? source :
            (local == NET_IF_LOOPBACK ? 0x7f000001u : destination),
            .destination = destination, .next_hop = destination, .ifindex = local};
        return 0;
    }
    if (destination == UINT32_MAX) {
        uint32_t index = scope;
        if (!index) {
            for (index = net_interface_next(NET_IF_LOOPBACK); index;
                 index = net_interface_next(index))
                if (net_interface_is_ready(index)) break;
        }
        if (!index || index == NET_IF_LOOPBACK) return -LINUX_ENETUNREACH;
        if (!net_interface_is_ready(index)) return -LINUX_ENETDOWN;
        struct net_interface *iface = net_interface_get(index);
        *out = (struct net_ipv4_path){.source = source ? source : iface->config.local_ip,
            .destination = destination, .next_hop = destination, .ifindex = index};
        return 0;
    }

    struct net_ipv4_route best = {0};
    bool found = false;
    for (uint32_t index = net_interface_next(NET_IF_LOOPBACK); index;
         index = net_interface_next(index)) {
        struct net_interface *iface = net_interface_get(index);
        if (!iface || !iface->config.local_ip) continue;
        struct net_ipv4_route connected = {
            .destination = iface->config.local_ip & iface->config.subnet_mask,
            .mask = iface->config.subnet_mask, .ifindex = index, .flags = RTF_UP};
        ipv4_consider(&connected, destination, scope, &best, &found);
        if (iface->config.gateway_ip) {
            struct net_ipv4_route def = {.gateway = iface->config.gateway_ip,
                .ifindex = index, .metric = iface->default_metric, .flags = RTF_UP | RTF_GATEWAY};
            ipv4_consider(&def, destination, scope, &best, &found);
        }
    }
    for (struct ipv4_route_node *entry = ipv4_routes; entry; entry = entry->next)
        ipv4_consider(&entry->route, destination, scope, &best, &found);
    if (!found) return -LINUX_ENETUNREACH;
    struct net_interface *iface = net_interface_get(best.ifindex);
    if (!source && (!iface || !iface->config.local_ip)) return -LINUX_EADDRNOTAVAIL;
    *out = (struct net_ipv4_path){.source = source ? source : iface->config.local_ip,
        .destination = destination, .next_hop = best.gateway ? best.gateway : destination,
        .ifindex = best.ifindex};
    return 0;
}

int net_ipv4_route_update(const struct net_ipv4_route *input, bool remove, bool replace)
{
    if (!input || ((~input->mask) & ((~input->mask) + 1u)) ||
        (input->flags & ~(RTF_UP | RTF_GATEWAY | RTF_HOST))) return -LINUX_EINVAL;
    struct net_ipv4_route route = *input;
    route.destination &= route.mask;
    if (!!route.gateway != !!(route.flags & RTF_GATEWAY)) return -LINUX_EINVAL;
    if ((route.flags & RTF_HOST) && route.mask != UINT32_MAX) return -LINUX_EINVAL;
    struct net_interface *iface = net_interface_get(route.ifindex);
    if (!iface) return -LINUX_ENODEV;
    if (!remove && route.gateway && (!iface->config.local_ip ||
        (route.gateway & iface->config.subnet_mask) !=
        (iface->config.local_ip & iface->config.subnet_mask))) return -LINUX_ENETUNREACH;
    route.flags |= RTF_UP;

    /* The compatibility configuration owns one default gateway per NIC.
     * This also keeps DHCP's existing default-route updates observable. */
    if (!route.mask && route.gateway) {
        if (remove) {
            if (!iface->config.gateway_ip || iface->config.gateway_ip != route.gateway)
                return -LINUX_ESRCH;
            iface->config.gateway_ip = 0;
            iface->default_metric = 0;
            return 0;
        }
        if (iface->config.gateway_ip && !replace) return -LINUX_EEXIST;
        iface->config.gateway_ip = route.gateway;
        iface->default_metric = route.metric;
        return 0;
    }

    struct ipv4_route_node **slot = &ipv4_routes;
    while (*slot) {
        struct net_ipv4_route *r = &(*slot)->route;
        if (r->destination == route.destination && r->mask == route.mask &&
            r->ifindex == route.ifindex && r->gateway == route.gateway) break;
        slot = &(*slot)->next;
    }
    if (remove) {
        if (!*slot) return -LINUX_ESRCH;
        struct ipv4_route_node *entry = *slot;
        *slot = entry->next;
        kernel_free(entry);
        return 0;
    }
    if (*slot) {
        if (!replace) return -LINUX_EEXIST;
        (*slot)->route = route;
        return 0;
    }
    struct ipv4_route_node *entry = kernel_malloc(sizeof(*entry));
    if (!entry) return -LINUX_ENOMEM;
    entry->route = route;
    entry->next = NULL;
    *slot = entry;
    return 0;
}
