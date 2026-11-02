/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_UAPI_LINUX_NETLINK_H
#define RELIEFNT_UAPI_LINUX_NETLINK_H
#include <linux/socket.h>
#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#define NETLINK_ROUTE 0
#define SOL_NETLINK 270
#define NETLINK_ADD_MEMBERSHIP 1
#define NETLINK_DROP_MEMBERSHIP 2
struct sockaddr_nl { sa_family_t nl_family; uint16_t nl_pad; uint32_t nl_pid, nl_groups; };
struct nlmsghdr { uint32_t nlmsg_len; uint16_t nlmsg_type, nlmsg_flags; uint32_t nlmsg_seq, nlmsg_pid; };
struct nlmsgerr { int32_t error; struct nlmsghdr msg; };
#define NLMSG_ALIGN(n) (((n) + 3u) & ~3u)
#define NLMSG_HDRLEN ((uint32_t)sizeof(struct nlmsghdr))
#define NLMSG_LENGTH(n) (NLMSG_HDRLEN + (n))
#define NLMSG_SPACE(n) NLMSG_ALIGN(NLMSG_LENGTH(n))
#define NLMSG_DATA(h) ((void *)((uint8_t *)(h) + NLMSG_HDRLEN))
#define NLMSG_NOOP 1
#define NLMSG_ERROR 2
#define NLMSG_DONE 3
#define NLMSG_OVERRUN 4
#define NLM_F_REQUEST 0x001
#define NLM_F_MULTI 0x002
#define NLM_F_ACK 0x004
#define NLM_F_ECHO 0x008
#define NLM_F_ROOT 0x100
#define NLM_F_MATCH 0x200
#define NLM_F_DUMP (NLM_F_ROOT | NLM_F_MATCH)
#define NLM_F_REPLACE 0x100
#define NLM_F_EXCL 0x200
#define NLM_F_CREATE 0x400
#define NLM_F_APPEND 0x800
_Static_assert(sizeof(struct sockaddr_nl)==12, "Linux sockaddr_nl");
_Static_assert(sizeof(struct nlmsghdr)==16, "Linux nlmsghdr");
#endif
