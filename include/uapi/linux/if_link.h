/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_UAPI_LINUX_IF_LINK_H
#define RELIEFNT_UAPI_LINUX_IF_LINK_H
#include <linux/types.h>
struct ifinfomsg { uint8_t ifi_family, __ifi_pad; uint16_t ifi_type; int32_t ifi_index; uint32_t ifi_flags, ifi_change; };
#define IFLA_ADDRESS 1
#define IFLA_BROADCAST 2
#define IFLA_IFNAME 3
#define IFLA_MTU 4
#define IFLA_OPERSTATE 16
#endif
