/* Exercise the real UDP socket implementation with a deterministic IPv6 link. */
#define IPV6_TRANSPORT_FIXTURE 1
#define main ipv4_udp_regression_main
#include "udp_socket_test.c"
#undef main
#define sockets packet_sockets
#include "../../net/socket/packet.c"
#undef sockets
#include <linux/in6.h>
#include <reliefnt/net_ipv6.h>

void kernel_poll_readiness_notify(void) {}
static const struct in6_addr loop6 = {.s6_addr = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}};
uint32_t net_ipv6_addresses(struct net_ipv6_address *out, uint32_t capacity)
{
    if (capacity) out[0] = (struct net_ipv6_address){.address = loop6, .ifindex = 1};
    return 1;
}
int net_ipv6_route(const struct in6_addr *destination, uint32_t scope,
                   const struct in6_addr *bound, struct net_ipv6_path *path)
{
    if (memcmp(destination, &loop6, sizeof(loop6)) || (scope && scope != 1)) return -LINUX_ENETUNREACH;
    *path = (struct net_ipv6_path){.source = udp6_any(bound) ? loop6 : *bound,
        .destination = *destination, .ifindex = 1, .mtu = 65536};
    return 0;
}
uint16_t net_ipv6_checksum(const struct in6_addr *source, const struct in6_addr *destination,
                           uint8_t protocol, const void *data, uint32_t length)
{
    uint32_t sum = 0;
    const uint8_t *parts[] = {source->s6_addr, destination->s6_addr, data};
    uint32_t lengths[] = {16, 16, length};
    for (unsigned p = 0; p < 3; ++p)
        for (uint32_t i = 0; i < lengths[p]; i += 2)
            sum += (uint32_t)parts[p][i] * 256 + (i + 1 < lengths[p] ? parts[p][i + 1] : 0);
    sum += (length >> 16) + (length & 0xffff) + protocol;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}
int net_ipv6_output(const struct net_ipv6_path *path, uint8_t protocol,
                    const void *data, uint32_t length, int hops, uint32_t flowinfo, bool dont_fragment,
                    bool multicast_loop)
{
    const uint8_t *p = data;
    (void)flowinfo; (void)dont_fragment; (void)multicast_loop;
    assert(!net_ipv6_checksum(&path->source, &path->destination, protocol, data, length));
    if (protocol == 17) {
        assert(length >= 8);
        assert(net_udp6_input(&path->source, &path->destination, (p[0] << 8) | p[1],
                              (p[2] << 8) | p[3], p + 8, length - 8, path->ifindex,
                              hops < 0 ? 64 : (uint8_t)hops));
    } else net_raw6_input(&path->source, &path->destination, protocol, data, length,
                          path->ifindex, hops < 0 ? 64 : (uint8_t)hops);
    return (int)length;
}
int net_ipv6_multicast_change(const struct in6_addr *address, uint32_t ifindex, int delta)
{ (void)address; (void)ifindex; (void)delta; return 0; }
bool net_interface_ready(void) { return true; }
const uint8_t *e1000_mac(void) { static uint8_t mac[6]; return mac; }
int e1000_send(const void *frame, uint32_t length) { (void)frame; return (int)length; }
int net_ipv4_send_raw(uint32_t destination, uint32_t source, uint8_t protocol,
                      bool hdrincl, const void *data, uint32_t length)
{ (void)destination; (void)source; (void)protocol; (void)hdrincl; (void)data; return (int)length; }

int main(void)
{
    current.cap_effective = 1ULL << CAP_NET_BIND_SERVICE;
    int fd = (int)call(__NR_socket, AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0, 0, 0);
    assert(fd >= 0);
    int domain = -1;
    uint32_t length = sizeof(domain);
    assert(call(__NR_getsockopt, fd, SOL_SOCKET, SO_DOMAIN,
                (uintptr_t)&domain, (uintptr_t)&length) == 0);
    assert(domain == AF_INET6);
    int receiver = (int)call(__NR_socket, AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0, 0, 0);
    assert(receiver >= 0);
    struct sockaddr_in6 local = {.sin6_family = AF_INET6, .sin6_port = swap16(55001), .sin6_addr = loop6};
    assert(call(__NR_bind, receiver, (uintptr_t)&local, sizeof(local), 0, 0) == 0);
    assert(call(__NR_connect, fd, (uintptr_t)&local, sizeof(local), 0, 0) == 0);
    assert(task_udp_send(&files[fd], "hello6", 6, 0, 0, 0) == 6);
    struct udp_socket conflict = {.ipv6=true,.ifindex=2,.connected=true,
        .remote6=loop6,.remote_scope6=1,.remote_port=55001};
    assert(udp6_send(&conflict, "scope", 5, 0, 0) == -LINUX_EINVAL);
    char payload[16] = {0};
    struct sockaddr_in6 source = {0};
    uint32_t source_length = sizeof(source);
    assert(task_udp_recv(&files[receiver], payload, sizeof(payload), 0, 1,
                         (uintptr_t)&source_length) == -LINUX_EFAULT);
    assert(task_udp_available(&files[receiver]) == 6);
    assert(task_udp_recv(&files[receiver], payload, sizeof(payload), MSG_PEEK, 0, 0) == 6);
    assert(task_udp_available(&files[receiver]) == 6);
    assert(task_udp_recv(&files[receiver], payload, sizeof(payload), 0,
                         (uintptr_t)&source, (uintptr_t)&source_length) == 6);
    assert(source_length == 28 && source.sin6_family == AF_INET6 && !memcmp(&source.sin6_addr, &loop6, 16));
    assert(!memcmp(payload, "hello6", 6));
    char large[4096], received[4096];
    for (unsigned i = 0; i < sizeof(large); ++i) large[i] = (char)(i * 17u);
    assert(task_udp_send(&files[fd], large, sizeof(large), 0, 0, 0) == sizeof(large));
    assert(task_udp_recv(&files[receiver], received, sizeof(received), 0, 0, 0) == sizeof(received));
    assert(!memcmp(large, received, sizeof(large)));
    assert(call(__NR_bind, fd, 1, sizeof(local), 0, 0) == -LINUX_EFAULT);
    int option = 1;
    assert(call(__NR_setsockopt, fd, SOL_IPV6, IPV6_V6ONLY, (uintptr_t)&option, 4) == -LINUX_EINVAL);
    assert(call(__NR_setsockopt, receiver, SOL_IPV6, 12345, (uintptr_t)&option, 4) == -LINUX_ENOPROTOOPT);
    task_udp_release(&files[receiver]);
    files[receiver].used = 0;
    task_udp_release(&files[fd]);
    files[fd].used = 0;
    int v6 = (int)call(__NR_socket, AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0, 0, 0);
    int v4 = open_socket();
    struct sockaddr_in6 any6 = {.sin6_family = AF_INET6, .sin6_port = swap16(55002)};
    struct sockaddr_in any4 = {.sin_family = AF_INET, .sin_port = swap16(55002)};
    assert(call(__NR_bind, v6, (uintptr_t)&any6, sizeof(any6), 0, 0) == 0);
    assert(call(__NR_bind, v4, (uintptr_t)&any4, sizeof(any4), 0, 0) == -LINUX_EADDRINUSE);
    task_udp_release(&files[v6]); files[v6].used = 0;
    v6 = (int)call(__NR_socket, AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0, 0, 0);
    assert(call(__NR_setsockopt, v6, SOL_IPV6, IPV6_V6ONLY, (uintptr_t)&option, 4) == 0);
    assert(call(__NR_bind, v6, (uintptr_t)&any6, sizeof(any6), 0, 0) == 0);
    assert(call(__NR_bind, v4, (uintptr_t)&any4, sizeof(any4), 0, 0) == 0);
    task_udp_release(&files[v6]); files[v6].used = 0;
    task_udp_release(&files[v4]); files[v4].used = 0;
    current.cap_effective |= 1ULL << CAP_NET_RAW;
    int raw = (int)syscall_packet(__NR_socket, AF_INET6, SOCK_RAW | SOCK_NONBLOCK, IPPROTO_ICMPV6, 0, 0, 0);
    assert(raw >= 0);
    assert(syscall_packet(__NR_connect, raw, (uintptr_t)&local, sizeof(local), 0, 0, 0) == 0);
    assert(syscall_packet(__NR_setsockopt, raw, SOL_IPV6, IPV6_RECVPKTINFO, (uintptr_t)&option, 4, 0) == 0);
    assert(syscall_packet(__NR_setsockopt, raw, SOL_IPV6, IPV6_RECVHOPLIMIT, (uintptr_t)&option, 4, 0) == 0);
    assert(syscall_packet(__NR_setsockopt, raw, SOL_IPV6, IPV6_2292HOPLIMIT, (uintptr_t)&option, 4, 0) == 0);
    uint8_t echo[8] = {128,0,0,0,1,2,3,4};
    assert(task_packet_send(&files[raw], echo, sizeof(echo), 0, 0, 0) == sizeof(echo));
    struct net_udp_rx_info info;
    source_length = 0;
    assert(task_packet_recv_ex(&files[raw], payload, sizeof(payload), 0, &source, &source_length, &info) == sizeof(echo));
    assert(source_length == 28 && info.pktinfo && info.hoplimit && info.old_hoplimit && info.ifindex == 1 && info.hops == 64);
    assert(!memcmp(&info.destination, &loop6, 16) && payload[0] == (char)128);
    assert(syscall_packet(__NR_setsockopt, raw, SOL_IPV6, IPV6_CHECKSUM, (uintptr_t)&option, 4, 0) == -LINUX_EINVAL);
    int checksum_offset = 2;
    assert(syscall_packet(__NR_setsockopt, raw, PACKET_SOL_RAW, IPV6_CHECKSUM, (uintptr_t)&checksum_offset, 4, 0) == 0);
    uint32_t filter[8] = {0}; filter[4] = 1;
    assert(syscall_packet(__NR_setsockopt, raw, IPPROTO_ICMPV6, PACKET_ICMP6_FILTER,
                          (uintptr_t)filter, sizeof(filter), 0) == 0);
    assert(task_packet_send(&files[raw], echo, sizeof(echo), 0, 0, 0) == sizeof(echo));
    assert(task_packet_recv(&files[raw], payload, sizeof(payload), 0, 0, 0) == -LINUX_EAGAIN);
    uint32_t filter_read[8] = {0}; uint32_t filter_length = sizeof(filter_read);
    assert(syscall_packet(__NR_getsockopt, raw, IPPROTO_ICMPV6, PACKET_ICMP6_FILTER,
                          (uintptr_t)filter_read, (uintptr_t)&filter_length, 0) == 0);
    assert(filter_length == sizeof(filter) && !memcmp(filter, filter_read, sizeof(filter)));
    int64_t timeout[2] = {-1, 500000};
    assert(syscall_packet(__NR_setsockopt, raw, SOL_SOCKET, SO_RCVTIMEO,
                          (uintptr_t)timeout, sizeof(timeout), 0) == 0);
    files[raw].flags &= ~RELIEFOS_O_NONBLOCK;
    current.socket_io_deadline = 0;
    assert(task_packet_recv(&files[raw], payload, sizeof(payload), 0, 0, 0) == -LINUX_EAGAIN);
    uint32_t timeout_length = sizeof(timeout);
    assert(syscall_packet(__NR_getsockopt, raw, SOL_SOCKET, SO_RCVTIMEO,
                          (uintptr_t)timeout, (uintptr_t)&timeout_length, 0) == 0);
    assert(timeout[0] == 0 && timeout[1] == 0);
    current.socket_io_deadline = 0; current.socket_io_timed = false;
    task_packet_release(&files[raw]); files[raw].used = 0;
    current.cap_effective &= ~(1ULL << CAP_NET_RAW);
    assert(syscall_packet(__NR_socket, AF_INET6, SOCK_RAW, IPPROTO_ICMPV6, 0, 0, 0) == -LINUX_EPERM);
    v6 = (int)call(__NR_socket, AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0, 0, 0);
    v4 = open_socket();
    struct sockaddr_in local4 = {.sin_family = AF_INET, .sin_port = swap16(55003),
                                  .sin_addr.s_addr = swap32(0x7f000001)};
    assert(call(__NR_bind, v4, (uintptr_t)&local4, sizeof(local4), 0, 0) == 0);
    struct sockaddr_in6 mapped = {.sin6_family = AF_INET6, .sin6_port = local4.sin_port,
                                   .sin6_addr = udp6_map(0x7f000001)};
    assert(call(__NR_connect, v6, (uintptr_t)&mapped, sizeof(mapped), 0, 0) == 0);
    assert(task_udp_send(&files[v6], "map", 3, 0, 0, 0) == 3);
    struct sockaddr_in from4; uint32_t from4_length = sizeof(from4);
    assert(task_udp_recv(&files[v4], payload, sizeof(payload), 0,
                         (uintptr_t)&from4, (uintptr_t)&from4_length) == 3);
    assert(task_udp_send(&files[v4], "reply", 5, 0, (uintptr_t)&from4, sizeof(from4)) == 5);
    source_length = sizeof(source);
    assert(task_udp_recv(&files[v6], payload, sizeof(payload), 0,
                         (uintptr_t)&source, (uintptr_t)&source_length) == 5);
    assert(udp6_mapped(&source.sin6_addr) && udp6_unmap(&source.sin6_addr) == 0x7f000001);
    task_udp_release(&files[v6]); files[v6].used = 0;
    task_udp_release(&files[v4]); files[v4].used = 0;
    loopback_bytes = loopback_packets = 0;
    return ipv4_udp_regression_main();
}

bool net_interface_is_ready(uint32_t index) { return index >= 1 && index <= 3; }
const uint8_t *netdev_mac(uint32_t index) { (void)index; return e1000_mac(); }
int netdev_send(uint32_t index, const void *frame, uint32_t length)
{ (void)index; return e1000_send(frame, length); }
int net_ipv4_send_raw_on(uint32_t index, uint32_t destination, uint32_t source,
    uint8_t protocol, bool hdrincl, const void *data, uint32_t length)
{ (void)index; return net_ipv4_send_raw(destination,source,protocol,hdrincl,data,length); }
