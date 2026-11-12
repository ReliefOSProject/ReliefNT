/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"
#include <reliefnt/socket_usercopy.h>
#include <linux/ipv6_route.h>

/** @brief Export whole IPv4 ifreq records for all configured interfaces. */
static int interface_conf(struct task *task, uint64_t address)
{
    struct ifconf conf;
    int rc = socket_user_import(task, address, &conf, sizeof(conf));
    if (rc) return rc;
    rc = socket_user_prepare(task, address, sizeof(conf), true);
    if (rc) return rc;
    if (!user_range_writable(address, sizeof(conf))) return -LINUX_EFAULT;
    if (conf.ifc_len < 0) return -LINUX_EINVAL;
    uint32_t count = 0;
    for (uint32_t index = net_interface_next(0); index; index = net_interface_next(index)) {
        struct reliefos_net_config config;
        if (!net_interface_get_config(index, &config) && config.local_ip) ++count;
    }
    uint32_t available = count * sizeof(struct ifreq);
    if (!conf.ifc_buf) conf.ifc_len = (int32_t)available;
    else {
        uint32_t room = (uint32_t)conf.ifc_len / sizeof(struct ifreq);
        if (room > count) room = count;
        uint32_t bytes = room * sizeof(struct ifreq);
        if (bytes) {
            rc = socket_user_prepare(task, conf.ifc_buf, bytes, true);
            if (rc) return rc;
            if (!user_range_writable(conf.ifc_buf, bytes)) return -LINUX_EFAULT;
            uint32_t written = 0;
            for (uint32_t index = net_interface_next(0); index && written < room;
                 index = net_interface_next(index)) {
                struct reliefos_net_config config;
                if (net_interface_get_config(index, &config) || !config.local_ip) continue;
                struct ifreq result = {0};
                (void)net_interface_name(index, result.ifr_name);
                result.data.addr.sa_family = AF_INET;
                net_put_u32((uint8_t *)&result.data.addr + 4, config.local_ip);
                net_memcpy((void *)(uintptr_t)(conf.ifc_buf + written * sizeof(result)),
                           &result, sizeof(result));
                ++written;
            }
        }
        conf.ifc_len = (int32_t)bytes;
    }
    net_memcpy((void *)(uintptr_t)address, &conf, sizeof(conf));
    return 0;
}

/** @brief Select the best connected NIC for a legacy gateway-only request. */
static uint32_t interface_for_gateway(uint32_t gateway)
{
    uint32_t selected = 0, best_mask = 0;
    for (uint32_t index = net_interface_next(NET_IF_LOOPBACK); index;
         index = net_interface_next(index)) {
        struct net_interface *iface = net_interface_get(index);
        if (!iface || !iface->config.local_ip ||
            (gateway & iface->config.subnet_mask) !=
            (iface->config.local_ip & iface->config.subnet_mask)) continue;
        if (!selected || iface->config.subnet_mask > best_mask) {
            selected = index; best_mask = iface->config.subnet_mask;
        }
    }
    return selected;
}

/** @brief Apply an authorized IPv4 route ioctl without retaining user pointers. */
static int interface_route(struct task *task, uint32_t request, uint64_t address)
{
    struct rtentry input;
    int rc = socket_user_import(task, address, &input, sizeof(input));
    if (rc) return rc;
    if (input.rt_dst.sa_family != AF_INET) return -LINUX_EAFNOSUPPORT;
    uint32_t mask = net_get_u32((uint8_t *)&input.rt_genmask + 4);
    if (input.rt_genmask.sa_family != AF_INET && (input.rt_genmask.sa_family || mask))
        return -LINUX_EAFNOSUPPORT;
    if (input.rt_gateway.sa_family && input.rt_gateway.sa_family != AF_INET)
        return -LINUX_EAFNOSUPPORT;
    if ((input.rt_flags & ~(RTF_UP | RTF_GATEWAY | RTF_HOST)) || input.rt_metric < 0)
        return -LINUX_EOPNOTSUPP;
    struct net_ipv4_route route = {
        .destination = net_get_u32((uint8_t *)&input.rt_dst + 4), .mask = mask,
        .gateway = input.rt_gateway.sa_family == AF_INET ?
            net_get_u32((uint8_t *)&input.rt_gateway + 4) : 0,
        .metric = (uint32_t)input.rt_metric, .flags = input.rt_flags,
    };
    if ((route.flags & RTF_HOST) && !route.mask) route.mask = UINT32_MAX;
    if (input.rt_dev) {
        char name[NET_IF_NAME_LEN] = {0};
        bool terminated = false;
        for (uint32_t i = 0; i < sizeof(name); ++i) {
            if (input.rt_dev > UINT64_MAX - i) return -LINUX_EFAULT;
            rc = socket_user_import(task, input.rt_dev + i, &name[i], 1);
            if (rc) return rc;
            if (!name[i]) { terminated = true; break; }
        }
        if (!terminated) return -LINUX_EINVAL;
        route.ifindex = net_interface_index(name);
        if (!route.ifindex) return -LINUX_ENODEV;
    } else if (route.gateway) route.ifindex = interface_for_gateway(route.gateway);
    else if (request == SIOCDELRT && !route.mask) {
        for (uint32_t index = net_interface_next(NET_IF_LOOPBACK); index;
             index = net_interface_next(index)) {
            struct net_interface *iface = net_interface_get(index);
            if (iface && iface->config.gateway_ip) { route.ifindex = index; break; }
        }
    } else {
        struct net_ipv4_path path;
        if (!net_ipv4_route_lookup(route.destination, 0, 0, &path)) route.ifindex = path.ifindex;
    }
    if (!route.ifindex) return -LINUX_ENETUNREACH;
    if (request == SIOCDELRT && !route.mask && !route.gateway) {
        struct net_interface *iface = net_interface_get(route.ifindex);
        if (!iface || !iface->config.gateway_ip) return -LINUX_ESRCH;
        route.gateway = iface->config.gateway_ip;
        route.flags |= RTF_GATEWAY;
    }
    rc = net_ipv4_route_update(&route, request == SIOCDELRT, false);
    if (!rc) {
        net_arp_cache_clear_on(route.ifindex);
        netlink_ipv6_changed();
    }
    return rc;
}

int net_interface_configure_ipv4(uint32_t index, uint32_t address,
                                 uint32_t mask, uint32_t broadcast)
{
    struct net_interface *iface = net_interface_get(index);
    if (!iface) return index == NET_IF_LOOPBACK ? -LINUX_EOPNOTSUPP : -LINUX_ENODEV;
    if (((~mask) & ((~mask) + 1u)) || address >= 0xe0000000u) return -LINUX_EINVAL;
    iface->config.local_ip = address;
    iface->config.subnet_mask = mask;
    iface->broadcast = broadcast;
    if (!address) iface->config.gateway_ip = 0;
    iface->config.source = address ? RELIEFOS_NET_CONFIG_SOURCE_STATIC : RELIEFOS_NET_CONFIG_SOURCE_NONE;
    iface->config.lease_seconds = iface->config.dhcp_server_ip = 0;
    net_arp_cache_clear_on(index);
    netlink_ipv6_changed();
    return 0;
}

int net_interface_ioctl(uint32_t request, uint64_t address)
{
    bool change = request == SIOCSIFFLAGS || request == SIOCSIFADDR ||
        request == SIOCSIFNETMASK || request == SIOCSIFBRDADDR ||
        request == SIOCSIFMTU || request == SIOCADDRT || request == SIOCDELRT;
    struct task *task = sched_current_task();
    if (change && (!task || !(task->cap_effective & (1ULL << CAP_NET_ADMIN))))
        return -LINUX_EPERM;
    if (request == SIOCADDRT || request == SIOCDELRT)
        return interface_route(task, request, address);
    if (request == SIOCGIFCONF) return interface_conf(task, address);
    switch (request) {
    case SIOCGIFNAME: case SIOCGIFINDEX: case SIOCGIFHWADDR: case SIOCGIFFLAGS:
    case SIOCSIFFLAGS: case SIOCGIFADDR: case SIOCSIFADDR: case SIOCGIFNETMASK:
    case SIOCSIFNETMASK: case SIOCGIFBRDADDR: case SIOCSIFBRDADDR: case SIOCGIFMTU:
    case SIOCSIFMTU: case SIOCGIFTXQLEN: break;
    default: return -LINUX_ENOTTY;
    }
    struct ifreq req;
    int rc = socket_user_import(task, address, &req, sizeof(req));
    if (rc) return rc;
    if (!change) {
        rc = socket_user_prepare(task, address, sizeof(req), true);
        if (rc) return rc;
        if (!user_range_writable(address, sizeof(req))) return -LINUX_EFAULT;
    }
    uint32_t index;
    if (request == SIOCGIFNAME) {
        index = (uint32_t)req.data.value;
        if (!net_interface_name(index, req.ifr_name)) return -LINUX_ENODEV;
        net_memcpy((void *)(uintptr_t)address, &req, sizeof(req));
        return 0;
    }
    req.ifr_name[IFNAMSIZ - 1] = 0;
    index = net_interface_index(req.ifr_name);
    if (!index) return -LINUX_ENODEV;
    bool loop = index == NET_IF_LOOPBACK;
    struct net_interface *iface = net_interface_get(index);
    switch (request) {
    case SIOCGIFINDEX: req.data.value = (int32_t)index; break;
    case SIOCGIFHWADDR:
        net_memzero(&req.data, sizeof(req.data));
        req.data.addr.sa_family = loop ? 772 : 1;
        if (!loop) net_memcpy(req.data.addr.sa_data, netdev_mac(index), 6);
        break;
    case SIOCGIFFLAGS:
        req.data.flags = (loop ? IFF_LOOPBACK : IFF_BROADCAST | IFF_MULTICAST) |
            (net_interface_is_up(index) ? IFF_UP : 0) |
            (net_interface_is_ready(index) ? IFF_RUNNING : 0);
        break;
    case SIOCSIFFLAGS: {
        uint32_t allowed = IFF_UP | IFF_RUNNING | (loop ? IFF_LOOPBACK : IFF_BROADCAST | IFF_MULTICAST);
        if ((uint16_t)req.data.flags & ~allowed) return -LINUX_EOPNOTSUPP;
        bool up = !!(req.data.flags & IFF_UP);
        if (net_interface_is_up(index) != up) {
            rc = net_interface_set_up(index, up);
            if (rc) return rc;
            if (!loop) net_ipv6_link_changed_on(index, up);
            netlink_ipv6_changed();
        }
        break;
    }
    /* Frames go directly to the driver's bounded DMA ring. There is no
     * separate software transmit backlog to report through this ioctl. */
    case SIOCGIFTXQLEN: req.data.value = 0; break;
    case SIOCGIFMTU: req.data.value = loop ? 65536 : 1500; break;
    case SIOCSIFMTU:
        if (req.data.value != (loop ? 65536 : 1500)) return -LINUX_EOPNOTSUPP;
        break;
    default: {
        struct reliefos_net_config config;
        rc = net_interface_get_config(index, &config);
        if (rc) return rc;
        if (change) {
            if (loop) return -LINUX_EOPNOTSUPP;
            if (req.data.addr.sa_family != AF_INET) return -LINUX_EAFNOSUPPORT;
            uint32_t ip = net_get_u32((uint8_t *)&req.data.addr + 4);
            if (request == SIOCSIFNETMASK && ((~ip) & ((~ip) + 1u))) return -LINUX_EINVAL;
            if (request == SIOCSIFADDR && ip >= 0xe0000000u) return -LINUX_EINVAL;
            uint32_t local = request == SIOCSIFADDR ? ip : iface->config.local_ip;
            uint32_t mask = request == SIOCSIFNETMASK ? ip : iface->config.subnet_mask;
            uint32_t broadcast = request == SIOCSIFBRDADDR ? ip : local | ~mask;
            rc = net_interface_configure_ipv4(index,local,mask,broadcast);
            if (rc) return rc;
        } else {
            if (!config.local_ip) return -LINUX_EADDRNOTAVAIL;
            uint32_t value = request == SIOCGIFADDR ? config.local_ip :
                request == SIOCGIFNETMASK ? config.subnet_mask : loop ? 0 : iface->broadcast;
            net_memzero(&req.data, sizeof(req.data));
            req.data.addr.sa_family = AF_INET;
            net_put_u32((uint8_t *)&req.data.addr + 4, value);
        }
        break;
    }
    }
    if (!change) net_memcpy((void *)(uintptr_t)address, &req, sizeof(req));
    return 0;
}

bool net_interface_ready(void)
{
    uint32_t index = net_interface_next(NET_IF_LOOPBACK);
    return index && net_interface_is_ready(index);
}

bool net_interface_present(void)
{
    return net_interface_next(NET_IF_LOOPBACK) != 0;
}

bool net_interface_admin_up(void)
{
    uint32_t index = net_interface_next(NET_IF_LOOPBACK);
    return index && net_interface_is_up(index);
}
