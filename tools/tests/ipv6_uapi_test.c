/* SPDX-License-Identifier: Apache-2.0 */
#include <stddef.h>
#include <stdio.h>
#if __has_include("../../include/uapi/linux/in6.h")
#include "../../include/uapi/linux/in6.h"
int main(void)
{
    if (sizeof(struct in6_addr) != 16 || sizeof(struct sockaddr_in6) != 28 ||
        offsetof(struct sockaddr_in6, sin6_addr) != 8 ||
        offsetof(struct sockaddr_in6, sin6_scope_id) != 24 ||
        sizeof(struct ipv6_mreq) != 20 || AF_INET6 != 10 || IPV6_V6ONLY != 26)
        return 2;
    printf("16 28 8 24 20 10 26\n");
    return 0;
}
#else
int main(void) { puts("IPv6 native socket ABI is missing"); return 1; }
#endif
