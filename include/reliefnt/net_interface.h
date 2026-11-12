/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_NET_INTERFACE_H
#define RELIEFNT_NET_INTERFACE_H

#include <reliefnt/types.h>
#include <reliefos/net_abi.h>

/* IPv4 values in this interface are in host byte order. Calls run under the
 * network execution owner; pointers returned by get are borrowed until the
 * interface is removed. Interface indices are never reused. */
#define NET_IF_LOOPBACK 1u
#define NET_IF_NAME_LEN 16u

struct net_interface {
    struct net_interface *next;
    uint32_t index;
    struct reliefos_net_config config;
    uint32_t broadcast;
    uint32_t default_metric;
    bool up;
};

struct net_ipv4_route {
    uint32_t destination, mask, gateway, ifindex, metric, flags;
};

struct net_ipv4_path {
    uint32_t source, destination, next_hop, ifindex;
};

/** @brief Register one hardware interface; zero or a negative Linux errno. */
int net_interface_register(uint32_t index);
/** @brief Remove a hardware interface and its routes; no borrowed pointer survives. */
void net_interface_remove(uint32_t index);
/** @brief Return the live hardware state for index, or NULL; loopback is virtual. */
struct net_interface *net_interface_get(uint32_t index);
/** @brief Return the next live index after index, including loopback, or zero. */
uint32_t net_interface_next(uint32_t after);
/** @brief Resolve an exact interface name to its index, or zero if absent. */
uint32_t net_interface_index(const char *name);
/** @brief Write a NUL-terminated live interface name; false when absent. */
bool net_interface_name(uint32_t index, char out[NET_IF_NAME_LEN]);
/** @brief Test for a live hardware interface or loopback. */
bool net_interface_exists(uint32_t index);
/** @brief Test administrative and carrier readiness for an interface. */
bool net_interface_is_ready(uint32_t index);
/** @brief Test the administrative state of an interface. */
bool net_interface_is_up(uint32_t index);
/** @brief Set administrative state after authorization; zero or a negative errno. */
int net_interface_set_up(uint32_t index, bool up);
/** @brief Resolve an assigned IPv4 address to its interface, or zero. */
uint32_t net_interface_for_ipv4(uint32_t ip);
/** @brief Copy the interface's IPv4 configuration, or return a negative errno. */
int net_interface_get_config(uint32_t index, struct reliefos_net_config *out);
/** @brief Replace one NIC's primary IPv4 address, mask, and broadcast. */
int net_interface_configure_ipv4(uint32_t index, uint32_t address,
                                 uint32_t mask, uint32_t broadcast);
/** @brief Access the first live NIC's compatibility state, or stable fallback. */
struct reliefos_net_config *net_interface_legacy_config(void);
/** @brief Access the first live NIC's administrative flag, or stable fallback. */
bool *net_interface_legacy_up(void);

/** @brief Select a live IPv4 route and source; source/scope zero mean automatic. */
int net_ipv4_route_lookup(uint32_t destination, uint32_t source, uint32_t scope,
                          struct net_ipv4_path *out);
/** @brief Add, replace, or remove a checked IPv4 route. */
int net_ipv4_route_update(const struct net_ipv4_route *route, bool remove, bool replace);
/** @brief Copy a route snapshot and return the total number of routes. */
uint32_t net_ipv4_routes(struct net_ipv4_route *out, uint32_t capacity);

/** @brief Set/query a bound device with Linux socket permissions and usercopy.
 * @param set Update when true. @param address User name. @param length Size or socklen pointer.
 * @param index Current/output bound index. @return Zero or negative errno. */
int net_interface_device_option(bool set, uint64_t address, uint64_t length, uint32_t *index);

#endif
