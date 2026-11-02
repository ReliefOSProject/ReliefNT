/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_UAPI_LINUX_IN6_H
#define RELIEFNT_UAPI_LINUX_IN6_H
#include <linux/socket.h>

/* Linux x86-64 IPv6 socket ABI; address octets are in network byte order. */
struct in6_addr {
    union { uint8_t u6_addr8[16]; uint16_t u6_addr16[8]; uint32_t u6_addr32[4]; } in6_u;
};
#define s6_addr in6_u.u6_addr8
#define s6_addr16 in6_u.u6_addr16
#define s6_addr32 in6_u.u6_addr32
struct sockaddr_in6 {
    sa_family_t sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};
struct ipv6_mreq { struct in6_addr ipv6mr_multiaddr; int32_t ipv6mr_ifindex; };
struct in6_pktinfo { struct in6_addr ipi6_addr; uint32_t ipi6_ifindex; };
struct in6_ifreq { struct in6_addr ifr6_addr; uint32_t ifr6_prefixlen; int32_t ifr6_ifindex; };
#define IPPROTO_IPV6 41
#define IPPROTO_HOPOPTS 0
#define IPPROTO_ROUTING 43
#define IPPROTO_FRAGMENT 44
#define IPPROTO_ICMPV6 58
#define IPPROTO_NONE 59
#define IPPROTO_DSTOPTS 60
#define SOL_IPV6 41
#define IPV6_CHECKSUM 7
#define IPV6_2292HOPLIMIT 8
#define IPV6_UNICAST_HOPS 16
#define IPV6_MULTICAST_IF 17
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_ADD_MEMBERSHIP 20
#define IPV6_DROP_MEMBERSHIP 21
#define IPV6_JOIN_GROUP IPV6_ADD_MEMBERSHIP
#define IPV6_LEAVE_GROUP IPV6_DROP_MEMBERSHIP
#define IPV6_MTU_DISCOVER 23
#define IPV6_MTU 24
#define IPV6_RECVERR 25
#define IPV6_V6ONLY 26
#define IPV6_HDRINCL 36
#define IPV6_RECVPKTINFO 49
#define IPV6_PKTINFO 50
#define IPV6_RECVHOPLIMIT 51
#define IPV6_HOPLIMIT 52
#define IPV6_DONTFRAG 62
#define IPV6_RECVTCLASS 66
#define IPV6_TCLASS 67
#define IPV6_PMTUDISC_DONT 0
#define IPV6_PMTUDISC_WANT 1
#define IPV6_PMTUDISC_DO 2
#define IPV6_PMTUDISC_PROBE 3
_Static_assert(sizeof(struct in6_addr)==16,"Linux in6_addr size");
_Static_assert(sizeof(struct sockaddr_in6)==28,"Linux sockaddr_in6 size");
_Static_assert(__builtin_offsetof(struct sockaddr_in6,sin6_scope_id)==24,"Linux scope offset");
#endif
