/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_UAPI_LINUX_IF_ADDR_H
#define RELIEFNT_UAPI_LINUX_IF_ADDR_H
#include <linux/types.h>
struct ifaddrmsg { uint8_t ifa_family, ifa_prefixlen, ifa_flags, ifa_scope; uint32_t ifa_index; };
struct ifa_cacheinfo { uint32_t ifa_prefered, ifa_valid, cstamp, tstamp; };
#define IFA_ADDRESS 1
#define IFA_LOCAL 2
#define IFA_LABEL 3
#define IFA_BROADCAST 4
#define IFA_CACHEINFO 6
#define IFA_FLAGS 8
#define IFA_F_NODAD 0x02
#define IFA_F_DADFAILED 0x08
#define IFA_F_DEPRECATED 0x20
#define IFA_F_TENTATIVE 0x40
#define IFA_F_PERMANENT 0x80
#endif
