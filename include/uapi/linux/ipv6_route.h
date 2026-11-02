/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_UAPI_LINUX_IPV6_ROUTE_H
#define RELIEFNT_UAPI_LINUX_IPV6_ROUTE_H
#include <linux/in6.h>
/* Native x86-64 Linux route-ioctl payload. */
struct in6_rtmsg {
    struct in6_addr rtmsg_dst, rtmsg_src, rtmsg_gateway;
    uint32_t rtmsg_type;
    uint16_t rtmsg_dst_len, rtmsg_src_len;
    uint32_t rtmsg_metric;
    uint64_t rtmsg_info;
    uint32_t rtmsg_flags;
    int32_t rtmsg_ifindex;
};
#define RTF_UP 0x0001u
#define RTF_GATEWAY 0x0002u
#define RTF_HOST 0x0004u
#define RTF_ADDRCONF 0x00040000u
#define RTF_EXPIRES 0x00400000u
_Static_assert(sizeof(struct in6_rtmsg)==80, "Linux in6_rtmsg");
#endif
