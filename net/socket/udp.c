/* IPv4 datagrams share the task_file open-description lifetime. All entry
 * points run under the kernel execution lock, including packet delivery. */
#include <reliefnt/net_udp.h>
#include <reliefnt/net_ipv6.h>
#include <reliefnt/proc_stats.h>
#include <reliefnt/net.h>
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/socket_usercopy.h>
#include <reliefnt/time.h>
#include <reliefnt/futex.h>
#include <reliefnt/wait.h>
#include <linux/socket.h>
#include <linux/errno.h>
#include <linux/capability.h>
#include <linux/poll.h>

#define UDP_QUEUE_LENGTH 16u
#define UDP_MAX_PAYLOAD 1472u
#define UDP6_MAX_PAYLOAD 65527u
#define UDP_SO_BROADCAST 6
#define UDP6_GROUP_LIMIT 16u
#ifndef IPV6_2292HOPLIMIT
#define IPV6_2292HOPLIMIT 8
#endif

struct udp_packet {
    struct udp_packet *next;
    uint32_t source, length;
    struct in6_addr source6, destination6;
    uint32_t ifindex;
    uint8_t hops;
    bool ipv4;
    uint16_t port;
    unsigned char data[];
};
struct udp_socket {
    uint32_t local, remote;
    struct in6_addr local6, remote6;
    uint32_t scope6, remote_scope6, multicast_if;
    int unicast_hops, multicast_hops, mtu_discover;
    uint32_t uid;
    uint64_t ino;
    uint16_t port, remote_port;
    bool bound, connected, broadcast, reuse, ipv6, v6only, multicast_loop;
    bool recv_pktinfo, recv_hoplimit, recv_old_hoplimit, dont_fragment;
    struct { struct in6_addr address; uint32_t ifindex; bool used; } groups[UDP6_GROUP_LIMIT];
    bool receive_timeout_immediate;
    int ifindex;
    int error;
    uint64_t receive_timeout;
    uint32_t queued;
    struct udp_packet *head, *tail;
    struct udp_socket *registry_next;
};
static struct udp_socket *sockets;
static uint64_t proc_udp_ino, loopback_bytes, loopback_packets;
static uint16_t next_port = 49152;
static uint16_t swap16(uint16_t x) { return __builtin_bswap16(x); }
static uint32_t swap32(uint32_t x) { return __builtin_bswap32(x); }
static struct udp_socket *udp_from_file(struct task_file *file)
{ return file && file->kind == TASK_FILE_KIND_UDP ? (void *)(uintptr_t)file->aux : NULL; }

/** @brief Test whether an IPv6 address is unspecified.
 * @param address Network-order address. @return True for the all-zero address. */
static bool udp6_any(const struct in6_addr *address)
{
    static const struct in6_addr zero = {0};
    return !__builtin_memcmp(address, &zero, sizeof(zero));
}

/** @brief Test whether an IPv6 address encodes an IPv4-mapped endpoint.
 * @param address Network-order address. @return True for ::ffff:0:0/96. */
static bool udp6_mapped(const struct in6_addr *address)
{
    static const uint8_t prefix[12] = {0,0,0,0,0,0,0,0,0,0,255,255};
    return !__builtin_memcmp(address->s6_addr, prefix, sizeof(prefix));
}

/** @brief Extract the host-order IPv4 value from a mapped address.
 * @param address IPv4-mapped address. @return Host-order IPv4 value. */
static uint32_t udp6_unmap(const struct in6_addr *address)
{
    const uint8_t *a = address->s6_addr + 12;
    return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3];
}

/** @brief Form a network-order mapped IPv6 address from IPv4.
 * @param ip Host-order IPv4 value. @return IPv4-mapped address. */
static struct in6_addr udp6_map(uint32_t ip)
{
    struct in6_addr address = {0};
    address.s6_addr[10] = address.s6_addr[11] = 255;
    address.s6_addr[12] = (uint8_t)(ip >> 24);
    address.s6_addr[13] = (uint8_t)(ip >> 16);
    address.s6_addr[14] = (uint8_t)(ip >> 8);
    address.s6_addr[15] = (uint8_t)ip;
    return address;
}

/** @brief Check whether two bound sockets overlap in the IPv4 namespace.
 * @param s First socket. @param ip First socket's requested IPv4 address.
 * @param other Existing socket. @return True when both can receive the same IPv4 tuple. */
static bool udp_ipv4_overlap(const struct udp_socket *s, uint32_t ip,
                             const struct udp_socket *other)
{
    if (s->ipv6 && (s->v6only || (!udp6_any(&s->local6) && !udp6_mapped(&s->local6)))) return false;
    if (other->ipv6 && (other->v6only || (!udp6_any(&other->local6) && !udp6_mapped(&other->local6)))) return false;
    return !ip || !other->local || ip == other->local;
}

/**
 * @brief Bind a UDP socket to an address and check registered port conflicts.
 * @param s Socket to bind.
 * @param ip Requested local IPv4 address, or zero for any address.
 * @param port Requested local port, or zero for automatic allocation.
 * @return Zero on success or a negative errno.
 */
static int bind_port(struct udp_socket *s, uint32_t ip, uint16_t port)
{
    if (s->bound) return -LINUX_EINVAL;
    struct reliefos_net_config config;
    net_get_config(&config);
    if (ip && (ip >> 24) != 127 && ip != config.local_ip) return -LINUX_EADDRNOTAVAIL;
    struct task *task = sched_current_task();
    if (port && port < 1024 && !(task->cap_effective & (1ULL << CAP_NET_BIND_SERVICE)))
        return -LINUX_EACCES;
    for (unsigned attempt = 0; attempt < 16384; ++attempt) {
        uint16_t candidate = port ? port : next_port++;
        if (next_port < 49152) next_port = 49152;
        bool conflict = false;
        for (struct udp_socket *other = sockets; other;
             other = other->registry_next) {
            if (other == s || !other->bound || other->port != candidate ||
                (s->reuse && other->reuse)) continue;
            if (udp_ipv4_overlap(s, ip, other)) conflict = true;
            if (s->ipv6 && other->ipv6 && !udp6_mapped(&s->local6) &&
                !udp6_mapped(&other->local6) &&
                (udp6_any(&s->local6) || udp6_any(&other->local6) ||
                 !__builtin_memcmp(&s->local6, &other->local6, sizeof(s->local6))) &&
                (!s->scope6 || !other->scope6 || s->scope6 == other->scope6))
                conflict = true;
        }
        if (!conflict) { s->local = ip; s->port = candidate; s->bound = true; return 0; }
        if (port) return -LINUX_EADDRINUSE;
    }
    return -LINUX_EADDRINUSE;
}

/**
 * @brief Queue one received IPv4 datagram for each matching UDP endpoint.
 * @param source Source IPv4 address.
 * @param destination Destination IPv4 address.
 * @param source_port Source transport port.
 * @param destination_port Destination local port.
 * @param data Datagram payload.
 * @param length Payload bytes.
 * @param ifindex Incoming interface index.
 */
void net_udp_input(uint32_t source, uint32_t destination, uint16_t source_port,
                    uint16_t destination_port, const void *data, uint32_t length, int ifindex)
{
    if (length > UDP_MAX_PAYLOAD) return;
    for (struct udp_socket *s = sockets; s; s = s->registry_next) {
        if (s->ipv6 && (s->v6only ||
            (!udp6_any(&s->local6) && !udp6_mapped(&s->local6)) ||
            (s->connected && !udp6_mapped(&s->remote6)))) continue;
        if (!s->bound || (s->ifindex && s->ifindex != ifindex) || s->port != destination_port ||
            (s->local && s->local != destination) ||
            (s->connected && (s->remote != source || s->remote_port != source_port)) ||
            s->queued >= UDP_QUEUE_LENGTH) continue;
        struct udp_packet *packet = kernel_malloc(sizeof(*packet) + length);
        if (!packet) continue;
        *packet = (struct udp_packet){.source = source, .port = source_port, .length = length,
            .source6 = udp6_map(source), .destination6 = udp6_map(destination), .ifindex = (uint32_t)ifindex, .ipv4 = true};
        __builtin_memcpy(packet->data, data, length);
        if (s->tail) s->tail->next = packet;
        else s->head = packet;
        s->tail = packet;
        ++s->queued;
        kernel_poll_readiness_notify();
        if (destination != 0xffffffffu) break;
    }
}

/** @brief Queue a validated native IPv6 UDP datagram for a matching endpoint.
 * @param source Network-order source. @param destination Network-order destination.
 * @param source_port Host-order source port. @param destination_port Host-order local port.
 * @param data Borrowed datagram bytes. @param length Byte count. @param ifindex Ingress interface.
 * @param hops Received hop limit. @return True if at least one socket accepted the datagram. */
bool net_udp6_input(const struct in6_addr *source, const struct in6_addr *destination,
                    uint16_t source_port, uint16_t destination_port, const void *data,
                    uint32_t length, uint32_t ifindex, uint8_t hops)
{
    if (length > UDP6_MAX_PAYLOAD) return false;
    bool delivered = false;
    for (struct udp_socket *s = sockets; s; s = s->registry_next) {
        if (!s->ipv6 || !s->bound || s->port != destination_port ||
            (s->ifindex && (uint32_t)s->ifindex != ifindex) ||
            (s->scope6 && s->scope6 != ifindex) ||
            (!udp6_any(&s->local6) && __builtin_memcmp(&s->local6, destination, sizeof(*destination))) ||
            (s->connected && (s->remote_port != source_port ||
                __builtin_memcmp(&s->remote6, source, sizeof(*source)))) ||
            s->queued >= UDP_QUEUE_LENGTH) continue;
        if (destination->s6_addr[0] == 0xff) {
            bool joined = false;
            for (unsigned i = 0; i < UDP6_GROUP_LIMIT; ++i)
                if (s->groups[i].used && s->groups[i].ifindex == ifindex &&
                    !__builtin_memcmp(&s->groups[i].address, destination, sizeof(*destination))) joined = true;
            if (!joined) continue;
        }
        struct udp_packet *packet = kernel_malloc(sizeof(*packet) + length);
        if (!packet) continue;
        *packet = (struct udp_packet){.source6 = *source, .destination6 = *destination,
            .port = source_port, .length = length, .ifindex = ifindex, .hops = hops};
        if (length) __builtin_memcpy(packet->data, data, length);
        if (s->tail) s->tail->next = packet;
        else s->head = packet;
        s->tail = packet;
        ++s->queued;
        kernel_poll_readiness_notify();
        delivered = true;
        if (destination->s6_addr[0] != 0xff) break;
    }
    return delivered;
}

static int import_address(uint64_t pointer, uint32_t length, struct sockaddr_in *out)
{
    if ((int32_t)length < 0 || length < sizeof(*out)) return -LINUX_EINVAL;
    int prep=socket_user_prepare(sched_current_task(),pointer,sizeof(*out),false);
    if(prep)return prep;
    if (!user_range_ok(pointer, sizeof(*out))) return -LINUX_EFAULT;
    __builtin_memcpy(out, (void *)(uintptr_t)pointer, sizeof(*out));
    return out->sin_family == AF_INET ? 0 : -LINUX_EAFNOSUPPORT;
}

static int export_address(uint64_t pointer, uint64_t length_pointer, uint32_t ip, uint16_t port)
{
    uint32_t length;int prep=socket_user_length(sched_current_task(),length_pointer,&length);
    if(prep)return prep;
    if ((int32_t)length < 0) return -LINUX_EINVAL;
    if (length > sizeof(struct sockaddr_in)) length = sizeof(struct sockaddr_in);
    prep=socket_user_prepare(sched_current_task(),pointer,length,true);if(prep)return prep;
    if (length && !user_range_writable(pointer, length)) return -LINUX_EFAULT;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = swap16(port),
                                  .sin_addr = {.s_addr = swap32(ip)}};
    __builtin_memcpy((void *)(uintptr_t)pointer, &address, length);
    *(uint32_t *)(uintptr_t)length_pointer = sizeof(address);
    return 0;
}

/** @brief Import the complete Linux IPv6 socket address before changing state.
 * @param pointer User address. @param length Available bytes. @param out Kernel copy.
 * @return Zero or negative EINVAL/EFAULT/EAFNOSUPPORT. */
static int udp6_import(uint64_t pointer, uint32_t length, struct sockaddr_in6 *out)
{
    if ((int32_t)length < 0 || length < sizeof(*out)) return -LINUX_EINVAL;
    int ret = socket_user_prepare(sched_current_task(), pointer, sizeof(*out), false);
    if (ret) return ret;
    if (!user_range_ok(pointer, sizeof(*out))) return -LINUX_EFAULT;
    __builtin_memcpy(out, (void *)(uintptr_t)pointer, sizeof(*out));
    return out->sin6_family == AF_INET6 ? 0 : -LINUX_EAFNOSUPPORT;
}

/** @brief Copy an IPv6 socket address and its actual size to checked user memory.
 * @param pointer User output. @param length_pointer User socklen_t. @param address Value to export.
 * @return Zero or a negative errno without changing socket state. */
static int udp6_export(uint64_t pointer, uint64_t length_pointer, const struct sockaddr_in6 *address)
{
    uint32_t length;
    int ret = socket_user_length(sched_current_task(), length_pointer, &length);
    if (ret) return ret;
    if ((int32_t)length < 0) return -LINUX_EINVAL;
    if (length > sizeof(*address)) length = sizeof(*address);
    ret = socket_user_prepare(sched_current_task(), pointer, length, true);
    if (ret) return ret;
    if (length && !user_range_writable(pointer, length)) return -LINUX_EFAULT;
    if (length) __builtin_memcpy((void *)(uintptr_t)pointer, address, length);
    *(uint32_t *)(uintptr_t)length_pointer = sizeof(*address);
    return 0;
}

/** @brief Bind an IPv6 UDP socket with shared IPv4 port conflict checks.
 * @param s Unbound socket. @param address Checked native socket address.
 * @return Zero or negative errno; failed attempts do not alter the binding. */
static int udp6_bind(struct udp_socket *s, const struct sockaddr_in6 *address)
{
    if (s->bound) return -LINUX_EINVAL;
    const struct in6_addr *ip = &address->sin6_addr;
    if (udp6_mapped(ip) && s->v6only) return -LINUX_EINVAL;
    if (address->sin6_scope_id > 2) return -LINUX_ENODEV;
    if (ip->s6_addr[0] == 0xfe && (ip->s6_addr[1] & 0xc0) == 0x80 && !address->sin6_scope_id)
        return -LINUX_EINVAL;
    if (!udp6_any(ip) && !udp6_mapped(ip) && ip->s6_addr[0] != 0xff) {
        struct net_ipv6_address entries[32];
        uint32_t count = net_ipv6_addresses(entries, 32);
        bool found = false;
        for (uint32_t i = 0; i < count && i < 32; ++i)
            if (!__builtin_memcmp(&entries[i].address, ip, sizeof(*ip)) &&
                !(entries[i].flags & (NET_IPV6_ADDR_TENTATIVE | NET_IPV6_ADDR_DADFAILED)) &&
                (!address->sin6_scope_id || entries[i].ifindex == address->sin6_scope_id)) found = true;
        if (!found) return -LINUX_EADDRNOTAVAIL;
    }
    s->local6 = *ip;
    s->scope6 = address->sin6_scope_id;
    int ret = bind_port(s, udp6_mapped(ip) ? udp6_unmap(ip) : 0, swap16(address->sin6_port));
    if (ret < 0) { s->local6 = (struct in6_addr){0}; s->scope6 = 0; }
    return ret;
}

/** @brief Send native IPv6 UDP using the selected route and pseudo-header checksum.
 * @param s Owning socket. @param data Borrowed payload. @param length Payload bytes.
 * @param address Optional user destination. @param address_length Destination bytes.
 * @return Accepted payload bytes or a negative errno. */
static int udp6_send(struct udp_socket *s, const void *data, uint32_t length,
                     uint64_t address, uint32_t address_length)
{
    struct sockaddr_in6 to = {.sin6_family = AF_INET6, .sin6_addr = s->remote6,
        .sin6_port = swap16(s->remote_port), .sin6_scope_id = s->remote_scope6};
    if (address) { int ret = udp6_import(address, address_length, &to); if (ret) return ret; }
    else if (!s->connected) return -LINUX_EDESTADDRREQ;
    uint16_t port = swap16(to.sin6_port);
    if (!port) return -LINUX_EINVAL;
    if (udp6_mapped(&to.sin6_addr)) {
        if (length > UDP_MAX_PAYLOAD) return -LINUX_EMSGSIZE;
        if (s->v6only || (!udp6_any(&s->local6) && !udp6_mapped(&s->local6))) return -LINUX_ENETUNREACH;
        uint32_t ip = udp6_unmap(&to.sin6_addr);
        if (ip == UINT32_MAX && !s->broadcast) return -LINUX_EACCES;
        if (!s->bound) { int ret = bind_port(s, 0, 0); if (ret) return ret; }
        struct reliefos_net_config config;
        net_get_config(&config);
        if ((ip >> 24) == 127 || (ip && ip == config.local_ip)) {
            if (s->ifindex && s->ifindex != 1) return -LINUX_ENETUNREACH;
            loopback_bytes += length + 28; ++loopback_packets;
            net_udp_input(s->local ? s->local : ip, ip, s->port, port, data, length, 1);
            return (int)length;
        }
        if (s->ifindex == 1) return -LINUX_ENETUNREACH;
        int ret = net_ipv4_send_udp(s->local, ip, s->port, port, data, length);
        return ret < 0 ? ret : (int)length;
    }
    if (udp6_mapped(&s->local6)) return -LINUX_ENETUNREACH;
    uint32_t scope = to.sin6_scope_id ? to.sin6_scope_id : (uint32_t)s->ifindex;
    if (!scope && to.sin6_addr.s6_addr[0] == 0xff) scope = s->multicast_if;
    struct net_ipv6_path path;
    int ret = net_ipv6_route(&to.sin6_addr, scope, &s->local6, &path);
    if (ret < 0) return ret;
    if (!s->bound) { ret = bind_port(s, 0, 0); if (ret < 0) return ret; }
    uint8_t *packet = kernel_malloc(length + 8);
    if (!packet) return -LINUX_ENOMEM;
    packet[0] = (uint8_t)(s->port >> 8); packet[1] = (uint8_t)s->port;
    packet[2] = (uint8_t)(port >> 8); packet[3] = (uint8_t)port;
    packet[4] = (uint8_t)((length + 8) >> 8); packet[5] = (uint8_t)(length + 8);
    packet[6] = packet[7] = 0;
    if (length) __builtin_memcpy(packet + 8, data, length);
    uint16_t checksum = net_ipv6_checksum(&path.source, &path.destination, 17, packet, length + 8);
    if (!checksum) checksum = 0xffff;
    packet[6] = (uint8_t)(checksum >> 8); packet[7] = (uint8_t)checksum;
    int hops = to.sin6_addr.s6_addr[0] == 0xff ? s->multicast_hops : s->unicast_hops;
    ret = net_ipv6_output(&path, 17, packet, length + 8, hops, to.sin6_flowinfo,
                          s->dont_fragment || s->mtu_discover == IPV6_PMTUDISC_DO, s->multicast_loop);
    kernel_free(packet);
    return ret < 0 ? ret : (int)length;
}

/** @brief Send one IPv4, IPv6 or mapped UDP datagram without retaining caller buffers.
 * @param file Pinned UDP OFD. @param data Borrowed validated payload.
 * @param length Payload bytes. @param flags Native send flags.
 * @param address Optional user destination. @param address_length Destination size.
 * @return Accepted bytes, negative errno, or user-memory prerequisite marker. */
int task_udp_send(struct task_file *file, const void *data, uint32_t length,
                   uint32_t flags, uint64_t address, uint32_t address_length)
{
    struct udp_socket *s = udp_from_file(file);
    if (!s) return -LINUX_EBADF;
    if (s->error) { int error = s->error; s->error = 0; return -error; }
    if (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) return -LINUX_EOPNOTSUPP;
    if (length > (s->ipv6 ? UDP6_MAX_PAYLOAD : UDP_MAX_PAYLOAD)) return -LINUX_EMSGSIZE;
    if (s->ipv6) return udp6_send(s, data, length, address, address_length);
    uint32_t ip = s->remote;
    uint16_t port = s->remote_port;
    if (address) {
        struct sockaddr_in to;
        int ret = import_address(address, address_length, &to);
        if (ret < 0) return ret;
        ip = swap32(to.sin_addr.s_addr); port = swap16(to.sin_port);
    } else if (!s->connected) return -LINUX_EDESTADDRREQ;
    if (!port) return -LINUX_EINVAL;
    if (ip == 0xffffffffu && !s->broadcast) return -LINUX_EACCES;
    if (!s->bound) { int ret = bind_port(s, 0, 0); if (ret < 0) return ret; }
    struct reliefos_net_config config;
    net_get_config(&config);
    if ((ip >> 24) == 127 || (ip && ip == config.local_ip)) {
        if (s->ifindex && s->ifindex != 1) return -LINUX_ENETUNREACH;
        loopback_bytes += length + 28;
        ++loopback_packets;
        net_udp_input(s->local ? s->local : ip, ip, s->port, port, data, length, 1);
        return (int)length;
    }
    if (s->ifindex == 1) return -LINUX_ENETUNREACH;
    int ret = net_ipv4_send_udp(s->local, ip, s->port, port, data, length);
    return ret < 0 ? ret : (int)length;
}

/**
 * @brief Receive one queued UDP datagram or wait according to SO_RCVTIMEO.
 * @param file Open description owning the UDP endpoint.
 * @param data User-visible destination buffer.
 * @param length Destination capacity.
 * @param flags Supported receive flags.
 * @param address Optional source-address destination.
 * @param address_length Optional source-address length pointer.
 * @param source Optional caller-owned native sockaddr output.
 * @param source_length Optional actual source-size output.
 * @param info Optional caller-owned IPv6 ingress metadata output.
 * @return Received length, zero for an empty datagram, or negative errno/status.
 * @note A negative timeval seconds value is stored as immediate timeout, distinct
 *       from the all-zero value, which keeps the default infinite wait.
 */
static int udp_recv(struct task_file *file, void *data, uint32_t length,
                    uint32_t flags, uint64_t address, uint64_t address_length,
                    void *source, uint32_t *source_length, struct net_udp_rx_info *info)
{
    struct udp_socket *s = udp_from_file(file);
    if (!s) return -LINUX_EBADF;
    if (s->error) { int error = s->error; s->error = 0; return -error; }
    if (flags & ~(MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC | MSG_WAITALL)) return -LINUX_EOPNOTSUPP;
    net_poll_packets();
    if (!s->head) {
        if ((file->flags & RELIEFOS_O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -LINUX_EAGAIN;
        struct task *task = sched_current_task();
        uint64_t now = time_ticks();
        if (!task->socket_io_deadline) {
            task->socket_io_timed = s->receive_timeout_immediate || s->receive_timeout != 0;
            task->socket_io_deadline = s->receive_timeout_immediate ? now :
                !s->receive_timeout || UINT64_MAX - now < s->receive_timeout
                    ? UINT64_MAX : now + s->receive_timeout;
        }
        if (now >= task->socket_io_deadline) return -LINUX_EAGAIN;
        sched_sleep_current_until(now + 1);
        return KERNEL_SYSCALL_BLOCKED;
    }
    struct udp_packet *packet = s->head;
    struct sockaddr_in6 source6 = {.sin6_family = AF_INET6, .sin6_port = swap16(packet->port),
        .sin6_addr = packet->source6,
        .sin6_scope_id = packet->source6.s6_addr[0] == 0xfe &&
                         (packet->source6.s6_addr[1] & 0xc0) == 0x80 ? packet->ifindex : 0};
    struct sockaddr_in source4 = {.sin_family=AF_INET,.sin_port=swap16(packet->port),
                                  .sin_addr={.s_addr=swap32(packet->source)}};
    if (address) {
        int ret = s->ipv6 ? udp6_export(address, address_length, &source6) :
                            export_address(address, address_length, packet->source, packet->port);
        if (ret < 0) return ret;
    }
    if (source && source_length) {
        *source_length = s->ipv6 ? sizeof(source6) : sizeof(source4);
        __builtin_memcpy(source, s->ipv6 ? (const void *)&source6 : (const void *)&source4, *source_length);
    }
    if (info) *info = (struct net_udp_rx_info){.destination = packet->destination6,
        .ifindex = packet->ifindex, .hops = packet->hops,
        .pktinfo = s->ipv6 && !packet->ipv4 && s->recv_pktinfo,
        .hoplimit = s->ipv6 && !packet->ipv4 && s->recv_hoplimit,
        .old_hoplimit = s->ipv6 && !packet->ipv4 && s->recv_old_hoplimit};
    uint32_t copied = length < packet->length ? length : packet->length;
    __builtin_memcpy(data, packet->data, copied);
    int ret = (flags & MSG_TRUNC) ? (int)packet->length : (int)copied;
    if (!(flags & MSG_PEEK)) {
        s->head = packet->next;
        if (!s->head) s->tail = NULL;
        --s->queued;
        kernel_free(packet);
    }
    return ret;
}

/** @brief Receive one datagram after validating any user source address.
 * @param file Pinned UDP OFD. @param data Validated output. @param length Capacity.
 * @param flags Receive flags. @param address Optional user sockaddr output.
 * @param address_length User socklen pointer when address is nonzero.
 * @return Bytes, negative errno, or blocked marker. */
int task_udp_recv(struct task_file *file, void *data, uint32_t length,
                  uint32_t flags, uint64_t address, uint64_t address_length)
{ return udp_recv(file,data,length,flags,address,address_length,NULL,NULL,NULL); }

/** @brief Return the native UDP source address size.
 * @param file Pinned UDP OFD. @return Size or zero for an invalid description. */
uint32_t task_udp_address_size(const struct task_file *file)
{
    struct udp_socket *s = udp_from_file((struct task_file *)file);
    return !s ? 0 : s->ipv6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
}

/** @brief Receive a datagram into owned storage together with source and metadata.
 * @param file Pinned UDP OFD. @param data Kernel destination. @param length Capacity.
 * @param flags Receive flags. @param source Optional sockaddr storage.
 * @param source_length Optional source-size output. @param info Optional metadata output.
 * @return Bytes, errno, or blocked sentinel; PEEK preserves the queue entry. */
int task_udp_recv_ex(struct task_file *file, void *data, uint32_t length, uint32_t flags,
                     void *source, uint32_t *source_length, struct net_udp_rx_info *info)
{ return udp_recv(file,data,length,flags,0,0,source,source_length,info); }

#ifdef CONFIG_SWAP
/** @brief Receive one datagram into owned kernel payload and source buffers.
 * @param file Pinned UDP OFD. @param data Kernel destination. @param length
 * Capacity. @param flags Native receive flags. @param source Optional kernel
 * sockaddr. @return Byte count, blocking sentinel, or errno; PEEK retains it.
 */
int task_udp_recv_staged(struct task_file *file,void *data,uint32_t length,
                         uint32_t flags,struct sockaddr_in *source)
{
    if (source && task_udp_address_size(file) != sizeof(*source)) return -LINUX_EINVAL;
    uint32_t source_length = 0;
    return udp_recv(file,data,length,flags,0,0,source,source ? &source_length : NULL,NULL);
}
#endif

/** @brief Query a UDP OFD's queued data, send readiness and pending errors.
 * @param file Pinned OFD. @param events Requested poll bits. @return Ready bits. */
short task_udp_poll(struct task_file *file, short events)
{
    struct udp_socket *s = udp_from_file(file);
    if (!s) return POLLNVAL;
    net_poll_packets();
    return (events & POLLOUT) | (s->head ? (events & POLLIN) : 0) | (s->error ? POLLERR : 0);
}

int task_udp_available(struct task_file *file)
{
    struct udp_socket *s = udp_from_file(file);
    return !s ? -LINUX_EBADF : s->head ? (int)s->head->length : 0;
}

/**
 * @brief Remove a final UDP socket description and release its queued datagrams.
 * @param file File description whose final reference is being released.
 */
void task_udp_release(struct task_file *file)
{
    struct udp_socket *s = udp_from_file(file);
    if (!s) return;
    struct udp_socket **link = &sockets;
    while (*link && *link != s) link = &(*link)->registry_next;
    if (*link == s) *link = s->registry_next;
    for (unsigned i = 0; i < UDP6_GROUP_LIMIT; ++i)
        if (s->groups[i].used)
            (void)net_ipv6_multicast_change(&s->groups[i].address, s->groups[i].ifindex, -1);
    while (s->head) {
        struct udp_packet *p = s->head;
        s->head = p->next;
        kernel_free(p);
    }
    kernel_free(s);
    file->aux = 0;
}

/** @brief Read or change implemented IPv6 datagram options using checked user storage.
 * @param s UDP endpoint. @param set True for setsockopt. @param option IPv6 option number.
 * @param pointer User value. @param length_or_pointer Input length or user socklen_t.
 * @return Zero or a negative Linux errno. */
static int udp6_option(struct udp_socket *s, bool set, uint32_t option,
                       uint64_t pointer, uint64_t length_or_pointer)
{
    uint32_t length = (uint32_t)length_or_pointer;
    struct task *task = sched_current_task();
    if (!set) { int ret = socket_user_length(task, length_or_pointer, &length); if (ret) return ret; }
    if ((int32_t)length < 0) return -LINUX_EINVAL;
    if (set && (option == IPV6_JOIN_GROUP || option == IPV6_LEAVE_GROUP)) {
        struct ipv6_mreq group;
        if (length < sizeof(group)) return -LINUX_EINVAL;
        int ret = socket_user_prepare(task, pointer, sizeof(group), false); if (ret) return ret;
        if (!user_range_ok(pointer, sizeof(group))) return -LINUX_EFAULT;
        __builtin_memcpy(&group, (void *)(uintptr_t)pointer, sizeof(group));
        if (group.ipv6mr_multiaddr.s6_addr[0] != 0xff || group.ipv6mr_ifindex < 0) return -LINUX_EINVAL;
        uint32_t ifindex = group.ipv6mr_ifindex ? (uint32_t)group.ipv6mr_ifindex : s->multicast_if;
        if (!ifindex) ifindex = s->ifindex ? (uint32_t)s->ifindex : 2;
        unsigned slot = UDP6_GROUP_LIMIT, empty = UDP6_GROUP_LIMIT;
        for (unsigned i = 0; i < UDP6_GROUP_LIMIT; ++i) {
            if (!s->groups[i].used && empty == UDP6_GROUP_LIMIT) empty = i;
            if (s->groups[i].used && s->groups[i].ifindex == ifindex &&
                !__builtin_memcmp(&s->groups[i].address, &group.ipv6mr_multiaddr, sizeof(group.ipv6mr_multiaddr))) slot = i;
        }
        bool join = option == IPV6_JOIN_GROUP;
        if (join && slot != UDP6_GROUP_LIMIT) return -LINUX_EADDRINUSE;
        if (!join && slot == UDP6_GROUP_LIMIT) return -LINUX_EADDRNOTAVAIL;
        if (join && empty == UDP6_GROUP_LIMIT) return -LINUX_ENOBUFS;
        ret = net_ipv6_multicast_change(&group.ipv6mr_multiaddr, ifindex, join ? 1 : -1);
        if (ret < 0) return ret;
        if (join) { s->groups[empty].address = group.ipv6mr_multiaddr; s->groups[empty].ifindex = ifindex; s->groups[empty].used = true; }
        else s->groups[slot].used = false;
        return 0;
    }
    int value = 0;
    if (set) {
        if (length < sizeof(value)) return -LINUX_EINVAL;
        int ret = socket_user_prepare(task, pointer, sizeof(value), false); if (ret) return ret;
        if (!user_range_ok(pointer, sizeof(value))) return -LINUX_EFAULT;
        __builtin_memcpy(&value, (void *)(uintptr_t)pointer, sizeof(value));
        switch (option) {
        case IPV6_V6ONLY: if (s->bound) return -LINUX_EINVAL; s->v6only = value != 0; break;
        case IPV6_UNICAST_HOPS: if (value < -1 || value > 255) return -LINUX_EINVAL; s->unicast_hops = value; break;
        case IPV6_MULTICAST_HOPS: if (value < -1 || value > 255) return -LINUX_EINVAL; s->multicast_hops = value; break;
        case IPV6_MULTICAST_IF: if (value < 0 || value > 2) return -LINUX_ENODEV; s->multicast_if = (uint32_t)value; break;
        case IPV6_MULTICAST_LOOP: if (value != 0 && value != 1) return -LINUX_EINVAL; s->multicast_loop = value != 0; break;
        case IPV6_RECVPKTINFO: s->recv_pktinfo = value != 0; break;
        case IPV6_RECVHOPLIMIT: s->recv_hoplimit = value != 0; break;
        case IPV6_2292HOPLIMIT: s->recv_old_hoplimit = value != 0; break;
        case IPV6_DONTFRAG: s->dont_fragment = value != 0; break;
        case IPV6_MTU_DISCOVER:
            if (value != IPV6_PMTUDISC_DONT && value != IPV6_PMTUDISC_WANT && value != IPV6_PMTUDISC_DO)
                return -LINUX_EINVAL;
            s->mtu_discover = value; break;
        default: return -LINUX_ENOPROTOOPT;
        }
        return 0;
    }
    switch (option) {
    case IPV6_V6ONLY: value = s->v6only; break;
    case IPV6_UNICAST_HOPS: value = s->unicast_hops; break;
    case IPV6_MULTICAST_HOPS: value = s->multicast_hops; break;
    case IPV6_MULTICAST_IF: value = (int)s->multicast_if; break;
    case IPV6_MULTICAST_LOOP: value = s->multicast_loop; break;
    case IPV6_RECVPKTINFO: value = s->recv_pktinfo; break;
    case IPV6_RECVHOPLIMIT: value = s->recv_hoplimit; break;
    case IPV6_2292HOPLIMIT: value = s->recv_old_hoplimit; break;
    case IPV6_DONTFRAG: value = s->dont_fragment; break;
    case IPV6_MTU_DISCOVER: value = s->mtu_discover; break;
    case IPV6_MTU: {
        struct net_ipv6_path path;
        if (!s->connected || udp6_mapped(&s->remote6)) return -LINUX_ENOTCONN;
        int ret = net_ipv6_route(&s->remote6, s->remote_scope6, &s->local6, &path);
        if (ret < 0) return ret;
        value = (int)path.mtu;
        break;
    }
    default: return -LINUX_ENOPROTOOPT;
    }
    if (length > sizeof(value)) length = sizeof(value);
    int ret = socket_user_prepare(task, pointer, length, true); if (ret) return ret;
    if (length && !user_range_writable(pointer, length)) return -LINUX_EFAULT;
    if (length) __builtin_memcpy((void *)(uintptr_t)pointer, &value, length);
    *(uint32_t *)(uintptr_t)length_or_pointer = length;
    return 0;
}

/**
 * @brief Dispatch IPv4 and IPv6 datagram syscalls against registered UDP sockets.
 * @param number Native syscall number.
 * @param a0 First syscall argument.
 * @param a1 Second syscall argument.
 * @param a2 Third syscall argument.
 * @param a3 Fourth syscall argument.
 * @param a4 Fifth syscall argument.
 * @param a5 Sixth syscall argument.
 * @return Syscall result or negative errno.
 */
int64_t syscall_udp(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                    uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a5;
    struct task *task = sched_current_task();
    if (!task) return -LINUX_EPERM;
    if (number == __NR_socket) {
        if (a2 && a2 != 17) return -LINUX_EPROTONOSUPPORT;
        if (a1 & ~(uint32_t)(SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC)) return -LINUX_EINVAL;
        struct udp_socket *s = kernel_malloc(sizeof(*s));
        if (!s) return -LINUX_ENOMEM;
        struct task_file *file;
        int fd = task_allocate_fd(task, 0, &file);
        if (fd < 0) { kernel_free(s); return fd; }
        *s = (struct udp_socket){.uid = task->euid, .ino = (3ULL << 60) | ++proc_udp_ino,
            .ipv6 = (int)a0 == AF_INET6, .unicast_hops = -1, .multicast_hops = 1,
            .mtu_discover = IPV6_PMTUDISC_WANT, .multicast_loop = true};
        if (!sockets) sockets = s;
        else {
            struct udp_socket *tail = sockets;
            while (tail->registry_next) tail = tail->registry_next;
            tail->registry_next = s;
        }
        file->flags = TASK_FILE_FLAG_SOCKET | TASK_FILE_FLAG_SOCKET_INET | RELIEFOS_O_RDWR |
            (a1 & SOCK_NONBLOCK ? RELIEFOS_O_NONBLOCK : 0);
        file->fd_flags = (a1 & SOCK_CLOEXEC) != 0;
        file->kind = TASK_FILE_KIND_UDP;
        file->socket_ino = s->ino;
        file->aux = (uintptr_t)s;
        file->node.type = RELIEFOS_FS_TYPE_DEVICE;
        return fd;
    }
#ifdef CONFIG_SWAP
    struct task_file *file = task_file_for_io(task, (int32_t)a0);
#else
    struct task_file *file = task_file_for_fd(task, (int32_t)a0);
#endif
    struct udp_socket *s = udp_from_file(file);
    if (!s) return -LINUX_EBADF;
    if (number == __NR_bind || number == __NR_connect) {
#ifdef CONFIG_SWAP
        if(number==__NR_connect && a2>=2) {
            int prep=socket_user_prepare(task,a1,2,false);
            if(prep==KERNEL_SYSCALL_SWAP_CONTENDED)return prep;
        }
#endif
        if (number == __NR_connect && a2 >= 2 && user_range_ok(a1, 2) &&
            *(uint16_t *)(uintptr_t)a1 == AF_UNSPEC) { s->connected = false; return 0; }
        if (s->ipv6) {
            struct sockaddr_in6 address;
            int ret = udp6_import(a1, (uint32_t)a2, &address);
            if (ret < 0) return ret;
            if (number == __NR_bind) return udp6_bind(s, &address);
            if (udp6_mapped(&address.sin6_addr) && s->v6only) return -LINUX_ENETUNREACH;
            if (!s->bound && (ret = bind_port(s, 0, 0)) < 0) return ret;
            s->remote6 = address.sin6_addr; s->remote_scope6 = address.sin6_scope_id;
            s->remote = udp6_mapped(&address.sin6_addr) ? udp6_unmap(&address.sin6_addr) : 0;
            s->remote_port = swap16(address.sin6_port); s->connected = true;
            return 0;
        }
        struct sockaddr_in address;
        int ret = import_address(a1, (uint32_t)a2, &address);
        if (ret < 0) return ret;
        uint32_t ip = swap32(address.sin_addr.s_addr);
        uint16_t port = swap16(address.sin_port);
        if (number == __NR_bind) return bind_port(s, ip, port);
        if (!s->bound && (ret = bind_port(s, 0, 0)) < 0) return ret;
        s->remote = ip; s->remote_port = port; s->connected = true;
        return 0;
    }
    if (number == __NR_getsockname || number == __NR_getpeername) {
        if (number == __NR_getpeername && !s->connected) return -LINUX_ENOTCONN;
        if (s->ipv6) {
            struct sockaddr_in6 address = {.sin6_family = AF_INET6,
                .sin6_addr = number == __NR_getsockname ? s->local6 : s->remote6,
                .sin6_port = swap16(number == __NR_getsockname ? s->port : s->remote_port),
                .sin6_scope_id = number == __NR_getsockname ? s->scope6 : s->remote_scope6};
            if (number == __NR_getsockname && udp6_any(&address.sin6_addr) && s->connected) {
                if (udp6_mapped(&s->remote6)) {
                    struct reliefos_net_config config; net_get_config(&config);
                    address.sin6_addr = udp6_map(s->remote >> 24 == 127 ? 0x7f000001 : config.local_ip);
                } else {
                    struct net_ipv6_path path;
                    if (!net_ipv6_route(&s->remote6, s->remote_scope6, &s->local6, &path)) address.sin6_addr = path.source;
                }
            }
            return udp6_export(a1, a2, &address);
        }
        struct reliefos_net_config config;
        net_get_config(&config);
        uint32_t local = s->local;
        if (!local && s->connected) local = s->remote >> 24 == 127 ? 0x7f000001 : config.local_ip;
        return export_address(a1, a2, number == __NR_getsockname ? local : s->remote,
                               number == __NR_getsockname ? s->port : s->remote_port);
    }
    if (number == __NR_setsockopt || number == __NR_getsockopt) {
        if (s->ipv6 && a1 == SOL_IPV6)
            return udp6_option(s, number == __NR_setsockopt, (uint32_t)a2, a3, a4);
        if (a1 != SOL_SOCKET) return -LINUX_ENOPROTOOPT;
        bool set = number == __NR_setsockopt;
        uint32_t length;
        if (set) length = (uint32_t)a4;
        else {
            int prep=socket_user_length(task,a4,&length);if(prep)return prep;
        }
        if ((int32_t)length < 0) return -LINUX_EINVAL;
        if (a2 == SO_BINDTODEVICE) {
            if (set) {
                struct task *task = sched_current_task();
                if (!(task->cap_effective & (1ULL << CAP_NET_RAW))) return -LINUX_EPERM;
                char name[16] = {0};
                uint32_t size = length < sizeof(name) - 1 ? length : sizeof(name) - 1;
                int prep=socket_user_prepare(task,a3,size,false);if(prep)return prep;
                if (size && !user_range_ok(a3, size)) return -LINUX_EFAULT;
                if (size) __builtin_memcpy(name, (void *)(uintptr_t)a3, size);
                int index = !name[0] ? 0 : !__builtin_strcmp(name, "eth0") ? 2 : !__builtin_strcmp(name, "lo") ? 1 : -1;
                if (index < 0) return -LINUX_ENODEV;
                s->ifindex = index;
                return 0;
            }
            const char *name = s->ifindex == 2 ? "eth0" : s->ifindex == 1 ? "lo" : "";
            uint32_t size = s->ifindex == 2 ? 5 : s->ifindex == 1 ? 3 : 0;
            if (size && length < 16) return -LINUX_EINVAL;
            int prep=socket_user_prepare(task,a3,size,true);if(prep)return prep;
            if (size && !user_range_writable(a3, size)) return -LINUX_EFAULT;
            if (size) __builtin_memcpy((void *)(uintptr_t)a3, name, size);
            *(uint32_t *)(uintptr_t)a4 = size;
            return 0;
        }
        int value = 0;
        if (a2 == SO_RCVTIMEO || a2 == SO_RCVTIMEO_NEW) {
            int64_t tv[2];
            uint64_t *timeout = &s->receive_timeout;
            if (set) {
                if (length < sizeof(tv)) return -LINUX_EINVAL;
                int prep=socket_user_prepare(task,a3,sizeof(tv),false);if(prep)return prep;
                if (!user_range_ok(a3, sizeof(tv))) return -LINUX_EFAULT;
                __builtin_memcpy(tv, (void *)(uintptr_t)a3, sizeof(tv));
                if (tv[1] < 0 || tv[1] >= 1000000) return -LINUX_EDOM;
                if (tv[0] < 0) {
                    *timeout = 0;
                    s->receive_timeout_immediate = true;
                } else {
                    uint64_t seconds = (uint64_t)tv[0];
                    *timeout = seconds > (UINT64_MAX - RELIEFNT_TICK_HZ) / RELIEFNT_TICK_HZ
                        ? UINT64_MAX : seconds * RELIEFNT_TICK_HZ +
                          ((uint64_t)tv[1] * RELIEFNT_TICK_HZ + 999999) / 1000000;
                    s->receive_timeout_immediate = false;
                }
                return 0;
            }
            tv[0] = *timeout / RELIEFNT_TICK_HZ;
            tv[1] = (*timeout % RELIEFNT_TICK_HZ) * 1000000 / RELIEFNT_TICK_HZ;
            if (length > sizeof(tv)) length = sizeof(tv);
            int prep=socket_user_prepare(task,a3,length,true);if(prep)return prep;
            if (length && !user_range_writable(a3, length)) return -LINUX_EFAULT;
            __builtin_memcpy((void *)(uintptr_t)a3, tv, length);
            *(uint32_t *)(uintptr_t)a4 = length;
            return 0;
        }
        if (set) {
            if (length < 4) return -LINUX_EINVAL;
            int prep=socket_user_prepare(task,a3,4,false);if(prep)return prep;
            if (!user_range_ok(a3, 4)) return -LINUX_EFAULT;
            value = *(int *)(uintptr_t)a3;
            if (a2 == SO_REUSEADDR) s->reuse = value != 0;
            else if (a2 == UDP_SO_BROADCAST) s->broadcast = value != 0;
            else return -LINUX_ENOPROTOOPT;
            return 0;
        }
        if (a2 == SO_TYPE) value = SOCK_DGRAM;
        else if (a2 == SO_DOMAIN) value = s->ipv6 ? AF_INET6 : AF_INET;
        else if (a2 == SO_PROTOCOL) value = 17;
        else if (a2 == SO_ERROR) value = s->error;
        else if (a2 == SO_REUSEADDR) value = s->reuse;
        else if (a2 == UDP_SO_BROADCAST) value = s->broadcast;
        else return -LINUX_ENOPROTOOPT;
        if (length > 4) length = 4;
        int prep=socket_user_prepare(task,a3,length,true);if(prep)return prep;
        if (length && !user_range_writable(a3, length)) return -LINUX_EFAULT;
        __builtin_memcpy((void *)(uintptr_t)a3, &value, length);
        *(uint32_t *)(uintptr_t)a4 = length;
        if (a2 == SO_ERROR) s->error = 0;
        return 0;
    }
    return -LINUX_EOPNOTSUPP;
}

/** @brief Match a validated ICMPv6 quote to its originating UDP endpoint.
 * @param source Original local address. @param destination Original peer address.
 * @param source_port Original local port. @param destination_port Original peer port.
 * @param error Positive errno. @param mtu Reported path MTU or zero. */
void net_udp6_error(const struct in6_addr *source,const struct in6_addr *destination,
                    uint16_t source_port,uint16_t destination_port,int error,uint32_t mtu)
{
    (void)mtu;
    for (struct udp_socket *s = sockets; s; s = s->registry_next) {
        if (!s->ipv6 || !s->bound || s->port != source_port ||
            (!udp6_any(&s->local6) && __builtin_memcmp(&s->local6, source, sizeof(*source))) ||
            (s->connected && (s->remote_port != destination_port ||
                              __builtin_memcmp(&s->remote6, destination, sizeof(*destination))))) continue;
        s->error = error;
    }
    kernel_poll_readiness_notify();
}

/** @brief Render IPv6 UDP endpoints and their actual queued byte counts.
 * @param stream Caller-owned proc output slice. */
void net_udp6_proc_emit(struct text_stream *stream)
{
    text_string(stream,"  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    uint32_t index = 0;
    for (struct udp_socket *u = sockets; u; u = u->registry_next) {
        if (!u->ipv6 || !u->bound) continue;
        uint64_t queued = 0; for (struct udp_packet *p = u->head; p; p = p->next) queued += p->length;
        text_unsigned(stream,index++); text_string(stream,": ");
        for (unsigned i = 0; i < 16; i += 4) { uint32_t word; __builtin_memcpy(&word,u->local6.s6_addr+i,4); text_hex(stream,word,8); }
        text_string(stream,":"); text_hex(stream,u->port,4); text_string(stream," ");
        for (unsigned i = 0; i < 16; i += 4) { uint32_t word; __builtin_memcpy(&word,u->remote6.s6_addr+i,4); text_hex(stream,word,8); }
        text_string(stream,":"); text_hex(stream,u->remote_port,4); text_string(stream,u->connected ? " 01 " : " 07 ");
        text_string(stream,"00000000:"); text_hex(stream,queued,8); text_string(stream," 00:00000000 00000000 ");
        text_unsigned(stream,u->uid); text_string(stream," 0 "); text_unsigned(stream,u->ino); text_string(stream,"\n");
    }
}

/** @brief Render registered UDP endpoints and their actual queued payloads.
 * @param s Output, execution-owned; stable numeric IDs never expose pointers.
 */
void net_udp_proc_emit(struct text_stream *s)
{
    text_string(s, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   "
                   "uid  timeout inode\n");
    uint32_t index = 0;
    for (struct udp_socket *u = sockets; u; u = u->registry_next) {
        if (!u->bound || u->ipv6)
            continue;
        uint64_t queued = 0;
        for (struct udp_packet *p = u->head; p; p = p->next)
            queued += p->length;
        text_unsigned(s, index++);
        text_string(s, ": ");
        text_hex(s, __builtin_bswap32(u->local), 8);
        text_string(s, ":");
        text_hex(s, u->port, 4);
        text_string(s, " ");
        text_hex(s, __builtin_bswap32(u->remote), 8);
        text_string(s, ":");
        text_hex(s, u->remote_port, 4);
        text_string(s, u->connected ? " 01 " : " 07 ");
        text_string(s, "00000000:");
        text_hex(s, queued, 8);
        text_string(s, " 00:00000000 00000000 ");
        text_unsigned(s, u->uid);
        text_string(s, " 0 ");
        text_unsigned(s, u->ino);
        text_string(s, "\n");
    }
}
/** @brief Emit loopback datagram packet/byte counters in net/dev format.
 * @param s Required output; execution ownership, no polling or allocation.
 */
void net_loopback_proc_emit(struct text_stream *s)
{
    text_string(s, "    lo: ");
    text_unsigned(s, loopback_bytes);
    text_string(s, " ");
    text_unsigned(s, loopback_packets);
    text_string(s, " 0 0 0 0 0 0 ");
    text_unsigned(s, loopback_bytes);
    text_string(s, " ");
    text_unsigned(s, loopback_packets);
    text_string(s, " 0 0 0 0 0 0\n");
}
