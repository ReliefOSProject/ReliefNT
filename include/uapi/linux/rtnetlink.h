/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_UAPI_LINUX_RTNETLINK_H
#define RELIEFNT_UAPI_LINUX_RTNETLINK_H
#include <linux/netlink.h>
#include <linux/if_addr.h>
#include <linux/if_link.h>
struct rtattr { uint16_t rta_len, rta_type; };
struct rtmsg { uint8_t rtm_family, rtm_dst_len, rtm_src_len, rtm_tos;
    uint8_t rtm_table, rtm_protocol, rtm_scope, rtm_type; uint32_t rtm_flags; };
#define RTA_ALIGN(n) (((n) + 3u) & ~3u)
#define RTA_LENGTH(n) ((uint32_t)sizeof(struct rtattr) + (n))
#define RTA_DATA(a) ((void *)((uint8_t *)(a) + sizeof(struct rtattr)))
#define RTM_NEWLINK 16
#define RTM_DELLINK 17
#define RTM_GETLINK 18
#define RTM_SETLINK 19
#define RTM_NEWADDR 20
#define RTM_DELADDR 21
#define RTM_GETADDR 22
#define RTM_NEWROUTE 24
#define RTM_DELROUTE 25
#define RTM_GETROUTE 26
#define RTA_DST 1
#define RTA_SRC 2
#define RTA_IIF 3
#define RTA_OIF 4
#define RTA_GATEWAY 5
#define RTA_PRIORITY 6
#define RTA_PREFSRC 7
#define RTA_TABLE 15
#define RTN_UNICAST 1
#define RTN_LOCAL 2
#define RTPROT_KERNEL 2
#define RTPROT_BOOT 3
#define RTPROT_STATIC 4
#define RTPROT_RA 9
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_SITE 200
#define RT_SCOPE_LINK 253
#define RT_SCOPE_HOST 254
#define RT_TABLE_MAIN 254
#define RT_TABLE_LOCAL 255
#define RTMGRP_LINK 0x1
#define RTMGRP_IPV6_IFADDR 0x100
#define RTMGRP_IPV6_ROUTE 0x400
#define RTNLGRP_LINK 1
#define RTNLGRP_IPV6_IFADDR 9
#define RTNLGRP_IPV6_ROUTE 11
#endif
