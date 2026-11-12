/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_NET_IPV6_H
#define RELIEFNT_NET_IPV6_H
#include <linux/in6.h>
#include <reliefnt/types.h>
struct text_stream;
#define NET_IPV6_IF_LOOPBACK 1u
#define NET_IPV6_IF_ETHERNET 2u /* Legacy default when no NIC is registered. */
#define NET_IPV6_ADDR_TENTATIVE 0x40u
#define NET_IPV6_ADDR_DADFAILED 0x08u
#define NET_IPV6_ADDR_DEPRECATED 0x20u
#define NET_IPV6_ADDR_PERMANENT 0x80u
#define NET_IPV6_ADDR_NODAD 0x02u

/* Network byte-order addresses; deadlines are uptime milliseconds, UINT64_MAX is infinite.
 * All APIs below require existing kernel execution ownership, never raw IRQ context.
 * Snapshots are copies and do not transfer references or expose internal pointers. */
struct net_ipv6_address {
    struct in6_addr address;
    uint32_t ifindex, prefix_length, flags, scope;
    uint64_t preferred_until, valid_until;
};
struct net_ipv6_route {
    struct in6_addr destination, gateway;
    uint32_t prefix_length, ifindex, metric, flags;
    uint64_t valid_until;
};
struct net_ipv6_neighbor {
    struct in6_addr address;
    uint8_t mac[6];
    uint32_t ifindex, state;
};
struct net_ipv6_path {
    struct in6_addr source, destination, next_hop;
    uint32_t ifindex, mtu;
};
/** @brief Initialize loopback and Ethernet IPv6 host state without blocking. */
void net_ipv6_init(void);
/** @brief Retire state of the first live NIC, or the legacy default when none exists. */
void net_ipv6_detached(void);
/** @brief Retire state owned by the specified NIC, including after registry removal.
 * @param ifindex Stable interface index. */
void net_ipv6_detached_on(uint32_t ifindex);
/** @brief Apply Ethernet state to the first live NIC or the legacy default.
 * @param up New administrative state. */
void net_ipv6_link_changed(bool up);
/** @brief Apply administrative state to one NIC under network execution ownership.
 * @param ifindex Stable interface index. @param up New administrative state.
 * @return Zero, -ENOMEM when link state cannot be allocated, or -ENODEV for an invalid interface.
 * Before initialization, state is applied later by the initialization scan. */
int net_ipv6_link_changed_on(uint32_t ifindex,bool up);
/** @brief Advance bounded DAD, router/neighbor discovery and queued loopback work. */
void net_ipv6_poll(void);
/** @brief Indicate receive/timer work requiring the task-context network pump.
 * @return True when bounded maintenance should remain enabled. */
bool net_ipv6_maintenance_pending(void);
/** @brief Validate a frame on the first live NIC or the legacy default.
 * @param frame Borrowed complete Ethernet frame. @param length Available bytes. */
void net_ipv6_input(const uint8_t *frame,uint32_t length);
/** @brief Validate and dispatch a frame received on the specified NIC.
 * @param ifindex Stable receiving interface index. @param frame Borrowed Ethernet frame.
 * @param length Available bytes. */
void net_ipv6_input_on(uint32_t ifindex,const uint8_t *frame,uint32_t length);
/** @brief Select a route and usable source without borrowing mutable state.
 * @param destination Required destination. @param scope Explicit interface or zero.
 * @param bound Optional local source, zero-address means automatic. @param out_path Output.
 * @return Zero or negative Linux errno, including ENETUNREACH/EADDRNOTAVAIL. */
int net_ipv6_route(const struct in6_addr *destination,uint32_t scope,
                   const struct in6_addr *bound,struct net_ipv6_path *out_path);
/** @brief Send or retain a checked transport message for resolved IPv6 output.
 * @param path Selected route copy. @param protocol Next-header protocol.
 * @param data Borrowed transport message including header and checksum. @param length Bytes.
 * @param hops Hop limit, -1 selects default. @param flowinfo Network-order flow information.
 * @param dont_fragment Reject oversize payload instead of source fragmentation.
 * @param multicast_loop Also deliver locally to joined multicast groups when true.
 * @return Accepted payload bytes or negative Linux errno. Loopback is deferred to poll. */
int net_ipv6_output(const struct net_ipv6_path *path,uint8_t protocol,
                    const void *data,uint32_t length,int hops,uint32_t flowinfo,
                    bool dont_fragment,bool multicast_loop);
/** @brief Compute an IPv6 upper-layer pseudo-header checksum.
 * @param source Source. @param destination Destination. @param protocol Next header.
 * @param data Borrowed upper-layer bytes. @param length Upper-layer byte length.
 * @return Internet checksum, zero when validating an intact checksummed message. */
uint16_t net_ipv6_checksum(const struct in6_addr *source,const struct in6_addr *destination,
                           uint8_t protocol,const void *data,uint32_t length);
/** @brief Copy address entries and return the total live count.
 * @param out Optional output. @param capacity Entry capacity. @return Total count. */
uint32_t net_ipv6_addresses(struct net_ipv6_address *out,uint32_t capacity);
/** @brief Copy routes and return their total live count.
 * @param out Optional output. @param capacity Entry capacity. @return Total count. */
uint32_t net_ipv6_routes(struct net_ipv6_route *out,uint32_t capacity);
/** @brief Copy neighbor entries and return their total live count.
 * @param out Optional output. @param capacity Entry capacity. @return Total count. */
uint32_t net_ipv6_neighbors(struct net_ipv6_neighbor *out,uint32_t capacity);
/** @brief Add/update or remove an address with DAD and lifetime handling.
 * @param address Checked native configuration. @param remove Remove when true.
 * @return Zero or negative Linux errno; authorization belongs to caller. */
int net_ipv6_address_update(const struct net_ipv6_address *address,bool remove);
/** @brief Add/update or remove a route.
 * @param route Checked native route. @param remove Remove when true.
 * @return Zero or negative Linux errno; authorization belongs to caller. */
int net_ipv6_route_update(const struct net_ipv6_route *route,bool remove);
/** @brief Retain/release multicast membership and update MLD host state.
 * @param address Multicast group. @param ifindex Selected interface. @param delta +1/-1.
 * @return Zero or negative Linux errno. */
int net_ipv6_multicast_change(const struct in6_addr *address,uint32_t ifindex,int delta);
/** @brief Apply IPv6 address/route socket ioctls using checked user memory.
 * @param request Linux ioctl. @param address User argument. @return Zero/negative errno. */
int net_ipv6_interface_ioctl(uint32_t request,uint64_t address);

/* Transport callbacks consume only validated, reassembled packets under execution ownership. */
/** @brief Deliver one checksummed UDP payload to matching sockets.
 * @param source Peer. @param destination Local. @param source_port Peer host-order port.
 * @param destination_port Local host-order port. @param data Borrowed payload.
 * @param length Payload bytes. @param ifindex Interface. @param hops Received hop limit.
 * @return True when delivered; false permits ICMPv6 port-unreachable. */
bool net_udp6_input(const struct in6_addr *source,const struct in6_addr *destination,
                     uint16_t source_port,uint16_t destination_port,const void *data,
                     uint32_t length,uint32_t ifindex,uint8_t hops);
/** @brief Deliver a checked TCP segment to the shared TCP engine.
 * @param source Peer. @param destination Local. @param data TCP bytes.
 * @param length TCP length. @param ifindex Receiving interface. */
void net_tcp6_input(const struct in6_addr *source,const struct in6_addr *destination,
                     const void *data,uint32_t length,uint32_t ifindex);
/** @brief Deliver an IPv6 upper-layer message to matching raw sockets.
 * @param source Peer. @param destination Local. @param protocol Next header.
 * @param data Borrowed bytes. @param length Length. @param ifindex Interface.
 * @param hops Received hop limit. */
void net_raw6_input(const struct in6_addr *source,const struct in6_addr *destination,
                     uint8_t protocol,const void *data,uint32_t length,
                     uint32_t ifindex,uint8_t hops);
/** @brief Associate a validated ICMPv6 error quote with transport state.
 * @param packet Quoted original IPv6 packet. @param length Available quote bytes.
 * @param error Positive Linux errno. @param mtu Reported MTU or zero. */
void net_ipv6_socket_error(const uint8_t *packet,uint32_t length,int error,uint32_t mtu);
/** @brief Deliver an asynchronous error for a specific receiving or transmitting interface.
 * @param ifindex Interface index, zero to skip interface filtering. @param packet Quoted packet.
 * @param length Available quote bytes. @param error Positive Linux errno. @param mtu MTU or zero. */
void net_ipv6_socket_error_on(uint32_t ifindex,const uint8_t *packet,uint32_t length,int error,uint32_t mtu);
/** @brief Render tcp6/udp6/raw6 socket state without driving I/O.
 * @param stream Slice output. @param leaf Requested standard proc basename. */
void net_ipv6_transport_proc_emit(struct text_stream *stream,const char *leaf);
/** @brief Notify subscribed route-netlink consumers of IPv6 state changes. */
void netlink_ipv6_changed(void);
#endif
