/* Raw IPv4 and Ethernet sockets. Serialized by the kernel execution lock. */
#include <reliefnt/net_packet.h>
#include <reliefnt/net.h>
#include <reliefnt/net_ipv6.h>
#include <reliefnt/net_udp.h>
#include <reliefnt/proc_stats.h>
#include <reliefnt/e1000.h>
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
#include <linux/if_packet.h>
#include <linux/capability.h>
#include <linux/poll.h>
#include <linux/errno.h>

#define PACKET_LIMIT 64u
#define PACKET_QUEUE_LIMIT 32u
#define PACKET_SOL_RAW 255
#define PACKET_ICMP6_FILTER 1
#ifndef IPV6_2292HOPLIMIT
#define IPV6_2292HOPLIMIT 8
#endif
struct packet_message {
    struct packet_message *next;
    uint32_t length;
    struct sockaddr_ll link;
    struct sockaddr_in ip;
    struct sockaddr_in6 ip6;
    struct in6_addr destination6;
    uint32_t ifindex;
    uint8_t hops;
    uint8_t data[];
};
struct packet_socket {
    int domain, type, protocol, ifindex;
    uint32_t local_ip, remote_ip;
    uint32_t uid;
    uint64_t ino;
    int error;
    struct in6_addr local6, remote6;
    uint32_t scope6, remote_scope6, multicast_if;
    uint16_t ping_id;
    int checksum_offset, unicast_hops, multicast_hops, mtu_discover;
    bool bound6, connected6, recv_pktinfo, recv_hoplimit, recv_old_hoplimit, dont_fragment, multicast_loop;
    bool timeout_immediate;
    uint32_t icmp6_filter[8];
    struct { struct in6_addr address; uint32_t ifindex; bool used; } groups[16];
    bool hdrincl, broadcast;
    uint64_t timeout;
    unsigned queued;
    struct packet_message *head, *tail;
};
static struct packet_socket *sockets[PACKET_LIMIT];
static uint64_t packet_ino;

/** @brief Test for an unspecified network-order IPv6 address.
 * @param address Address to inspect. @return True when all octets are zero. */
static bool packet6_any(const struct in6_addr *address)
{
    static const struct in6_addr zero = {0};
    return !__builtin_memcmp(address, &zero, sizeof(zero));
}

/** @brief Resolve the raw socket owned by a pinned description.
 * @param file Description or NULL.
 * @return Socket pointer or NULL for another descriptor kind.
 */
static struct packet_socket *packet_of(const struct task_file *file)
{
    return file && file->kind == TASK_FILE_KIND_PACKET ? (void *)(uintptr_t)file->aux : NULL;
}

/** @brief Queue a bounded copy of each matching ingress packet.
 * @param input Ethernet frame under the execution lock.
 * @param length Complete frame length.
 */
void net_packet_input(const void *input, uint32_t length)
{
    if (length < 14 || length > 1514) return;
    const uint8_t *frame = input;
    uint16_t protocol;
    __builtin_memcpy(&protocol, frame + 12, 2);
    for (unsigned i = 0; i < PACKET_LIMIT; ++i) {
        struct packet_socket *s = sockets[i];
        if (!s || s->queued >= PACKET_QUEUE_LIMIT) continue;
        uint32_t offset = s->domain == AF_PACKET && s->type == SOCK_RAW ? 0 : 14;
        uint32_t size = length - offset;
        uint32_t source = 0, destination = 0;
        if (s->domain == AF_PACKET) {
            if (!s->protocol || (s->protocol != (int)__builtin_bswap16(3) && s->protocol != protocol)) continue;
            if (s->ifindex && s->ifindex != 2) continue;
        } else {
            if (s->domain != AF_INET) continue;
            if (protocol != __builtin_bswap16(0x0800) || length < 34 || frame[14] >> 4 != 4 ||
                frame[23] != s->protocol) continue;
            uint32_t ihl = (frame[14] & 15u) * 4;
            uint32_t total = ((uint32_t)frame[16] << 8) | frame[17];
            if (ihl < 20 || total < ihl || total > length - 14) continue;
            __builtin_memcpy(&source, frame + 26, 4);
            __builtin_memcpy(&destination, frame + 30, 4);
            if ((s->local_ip && s->local_ip != destination) || (s->remote_ip && s->remote_ip != source)) continue;
            size = total;
        }
        struct packet_message *m = kernel_malloc(sizeof(*m) + size);
        if (!m) continue;
        *m = (struct packet_message){.length = size,
            .link = {.sll_family = AF_PACKET, .sll_protocol = protocol, .sll_ifindex = 2,
                .sll_hatype = 1, .sll_halen = 6,
                .sll_pkttype = frame[0] == 255 ? PACKET_BROADCAST : frame[0] & 1 ? PACKET_MULTICAST : PACKET_HOST},
            .ip = {.sin_family = AF_INET, .sin_addr.s_addr = source}};
        __builtin_memcpy(m->link.sll_addr, frame + 6, 6);
        __builtin_memcpy(m->data, frame + offset, size);
        if (s->tail) s->tail->next = m;
        else s->head = m;
        s->tail = m;
        ++s->queued;
        kernel_poll_readiness_notify();
    }
}

/** @brief Queue a validated IPv6 upper-layer message for matching raw sockets.
 * @param source Peer address. @param destination Local address. @param protocol Next header.
 * @param data Borrowed upper-layer bytes. @param length Byte count. @param ifindex Interface.
 * @param hops Received hop limit. */
void net_raw6_input(const struct in6_addr *source, const struct in6_addr *destination,
                    uint8_t protocol, const void *data, uint32_t length,
                    uint32_t ifindex, uint8_t hops)
{
    if (length > 65535) return;
    for (unsigned i = 0; i < PACKET_LIMIT; ++i) {
        struct packet_socket *s = sockets[i];
        if (!s || s->domain != AF_INET6 || s->protocol != protocol || s->queued >= PACKET_QUEUE_LIMIT ||
            (s->ifindex && (uint32_t)s->ifindex != ifindex) || (s->scope6 && s->scope6 != ifindex) ||
            (!packet6_any(&s->local6) && __builtin_memcmp(&s->local6, destination, sizeof(*destination))) ||
            (s->connected6 && __builtin_memcmp(&s->remote6, source, sizeof(*source)))) continue;
        if (s->type == SOCK_DGRAM && (length < 8 || ((const uint8_t *)data)[0] != 129 ||
            (((uint16_t)((const uint8_t *)data)[4] << 8) | ((const uint8_t *)data)[5]) != s->ping_id)) continue;
        if (protocol == IPPROTO_ICMPV6 && length &&
            (s->icmp6_filter[((const uint8_t *)data)[0] >> 5] & (1u << (((const uint8_t *)data)[0] & 31)))) continue;
        if (destination->s6_addr[0] == 0xff) {
            static const struct in6_addr all_nodes = {.s6_addr = {0xff,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,1}};
            bool joined = !__builtin_memcmp(destination, &all_nodes, sizeof(all_nodes));
            for (unsigned g = 0; g < 16; ++g)
                if (s->groups[g].used && s->groups[g].ifindex == ifindex &&
                    !__builtin_memcmp(&s->groups[g].address, destination, sizeof(*destination))) joined = true;
            if (!joined) continue;
        }
        struct packet_message *m = kernel_malloc(sizeof(*m) + length);
        if (!m) continue;
        *m = (struct packet_message){.length = length, .destination6 = *destination,
            .ifindex = ifindex, .hops = hops, .ip6 = {.sin6_family = AF_INET6, .sin6_addr = *source,
                .sin6_scope_id = source->s6_addr[0] == 0xfe && (source->s6_addr[1] & 0xc0) == 0x80 ? ifindex : 0}};
        if (length) __builtin_memcpy(m->data, data, length);
        if (s->tail) s->tail->next = m; else s->head = m;
        s->tail = m; ++s->queued; kernel_poll_readiness_notify();
    }
}

/** @brief Import the full native IPv6 address before socket side effects.
 * @param pointer User sockaddr. @param length Supplied size. @param out Kernel copy.
 * @return Zero or negative errno. */
static int packet6_import(uint64_t pointer, uint32_t length, struct sockaddr_in6 *out)
{
    if ((int32_t)length < 0 || length < sizeof(*out)) return -LINUX_EINVAL;
    int ret = socket_user_prepare(sched_current_task(), pointer, sizeof(*out), false);
    if (ret) return ret;
    if (!user_range_ok(pointer, sizeof(*out))) return -LINUX_EFAULT;
    __builtin_memcpy(out, (void *)(uintptr_t)pointer, sizeof(*out));
    return out->sin6_family == AF_INET6 ? 0 : -LINUX_EAFNOSUPPORT;
}

/** @brief Send a raw IPv6 upper-layer message through the selected native route.
 * @param s Socket. @param data Borrowed bytes. @param length Byte count.
 * @param address Optional user sockaddr. @param address_length Supplied sockaddr length.
 * @return Accepted bytes or negative errno. */
static int packet6_send(struct packet_socket *s, const void *data, uint32_t length,
                        uint64_t address, uint32_t address_length)
{
    struct sockaddr_in6 to = {.sin6_family = AF_INET6, .sin6_addr = s->remote6,
                               .sin6_scope_id = s->remote_scope6};
    if (address) { int ret = packet6_import(address, address_length, &to); if (ret) return ret; }
    else if (!s->connected6) return -LINUX_EDESTADDRREQ;
    if (length > 65535) return -LINUX_EMSGSIZE;
    uint32_t scope = to.sin6_scope_id ? to.sin6_scope_id : (uint32_t)s->ifindex;
    if (!scope && to.sin6_addr.s6_addr[0] == 0xff) scope = s->multicast_if;
    struct net_ipv6_path path;
    int ret = net_ipv6_route(&to.sin6_addr, scope, &s->local6, &path);
    if (ret < 0) return ret;
    uint8_t *copy = kernel_malloc(length ? length : 1);
    if (!copy) return -LINUX_ENOMEM;
    if (length) __builtin_memcpy(copy, data, length);
    if (s->type == SOCK_DGRAM) {
        if (length < 8 || copy[0] != 128 || copy[1] != 0) { kernel_free(copy); return -LINUX_EINVAL; }
        copy[4] = (uint8_t)(s->ping_id >> 8); copy[5] = (uint8_t)s->ping_id;
    }
    int offset = s->type == SOCK_DGRAM ? 2 : s->checksum_offset;
    if (offset >= 0) {
        if ((uint32_t)offset + 2 > length) { kernel_free(copy); return -LINUX_EINVAL; }
        copy[offset] = copy[offset + 1] = 0;
        uint16_t checksum = net_ipv6_checksum(&path.source, &path.destination, (uint8_t)s->protocol, copy, length);
        copy[offset] = (uint8_t)(checksum >> 8); copy[offset + 1] = (uint8_t)checksum;
    }
    int hops = to.sin6_addr.s6_addr[0] == 0xff ? s->multicast_hops : s->unicast_hops;
    ret = net_ipv6_output(&path, (uint8_t)s->protocol, copy, length, hops, to.sin6_flowinfo,
                          s->dont_fragment || s->mtu_discover == IPV6_PMTUDISC_DO, s->multicast_loop);
    kernel_free(copy);
    return ret < 0 ? ret : (int)length;
}

/** @brief Send a complete packet with native address validation.
 * @param file Pinned socket description.
 * @param data Validated packet payload.
 * @param length Payload bytes.
 * @param flags Linux message flags.
 * @param address Optional user sockaddr.
 * @param address_length Size of sockaddr.
 * @return Bytes accepted or negative errno.
 */
int task_packet_send(struct task_file *file, const void *data, uint32_t length,
                     uint32_t flags, uint64_t address, uint32_t address_length)
{
    struct packet_socket *s = packet_of(file);
    if (!s) return -LINUX_EBADF;
    if (s->error) { int error=s->error; s->error=0; return -error; }
    if (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) return -LINUX_EOPNOTSUPP;
    if (s->domain == AF_INET6) return packet6_send(s, data, length, address, address_length);
    if (!net_interface_ready()) return -LINUX_ENETDOWN;
    if (s->domain == AF_INET) {
        uint32_t destination = s->remote_ip;
        if (address) {
            if ((int32_t)address_length < 0 || address_length < sizeof(struct sockaddr_in)) return -LINUX_EINVAL;
            int prep=socket_user_prepare(sched_current_task(),address,sizeof(struct sockaddr_in),false);
            if(prep)return prep;
            if (!user_range_ok(address, sizeof(struct sockaddr_in))) return -LINUX_EFAULT;
            struct sockaddr_in to;
            __builtin_memcpy(&to, (void *)(uintptr_t)address, sizeof(to));
            if (to.sin_family != AF_INET) return -LINUX_EAFNOSUPPORT;
            destination = to.sin_addr.s_addr;
        }
        if (!destination) return -LINUX_EDESTADDRREQ;
        if (destination == UINT32_MAX && !s->broadcast) return -LINUX_EACCES;
        return net_ipv4_send_raw(__builtin_bswap32(destination), __builtin_bswap32(s->local_ip),
            (uint8_t)s->protocol, s->hdrincl, data, length);
    }
    struct sockaddr_ll to = {.sll_family = AF_PACKET, .sll_protocol = (uint16_t)s->protocol, .sll_ifindex = s->ifindex};
    if (address) {
        if ((int32_t)address_length < 0 || address_length < sizeof(to)) return -LINUX_EINVAL;
        int prep=socket_user_prepare(sched_current_task(),address,sizeof(to),false);
        if(prep)return prep;
        if (!user_range_ok(address, sizeof(to))) return -LINUX_EFAULT;
        __builtin_memcpy(&to, (void *)(uintptr_t)address, sizeof(to));
        if (to.sll_family != AF_PACKET) return -LINUX_EINVAL;
    }
    if (to.sll_ifindex != 2) return -LINUX_ENXIO;
    uint8_t frame[1514];
    uint32_t total = length;
    if (s->type == SOCK_DGRAM) {
        if (length > 1500) return -LINUX_EMSGSIZE;
        if (to.sll_halen < 6 || to.sll_halen > 8) return -LINUX_EINVAL;
        __builtin_memcpy(frame, to.sll_addr, 6);
        __builtin_memcpy(frame + 6, e1000_mac(), 6);
        __builtin_memcpy(frame + 12, &to.sll_protocol, 2);
        __builtin_memcpy(frame + 14, data, length);
        total += 14;
    } else {
        if (length < 14) return -LINUX_EINVAL;
        if (length > sizeof(frame)) return -LINUX_EMSGSIZE;
        __builtin_memcpy(frame, data, length);
    }
    int ret = e1000_send(frame, total);
    return ret < 0 ? ret : (int)length;
}

/** @brief Receive a queued packet or wait interruptibly for ingress.
 * @param file Pinned socket description.
 * @param data Writable payload destination.
 * @param length Payload capacity.
 * @param flags Linux receive flags.
 * @param address Optional user source sockaddr destination.
 * @param address_length User socklen_t pointer.
 * @return Message bytes, negative errno or KERNEL_SYSCALL_BLOCKED.
 */
static int packet_recv(struct task_file *file, void *data, uint32_t length,
                       uint32_t flags, uint64_t address, uint64_t address_length,
                       void *staged_source,uint32_t *staged_length,struct net_udp_rx_info *info)
{
    struct packet_socket *s = packet_of(file);
    if (!s) return -LINUX_EBADF;
    if (s->error) { int error=s->error; s->error=0; return -error; }
    if (flags & ~(MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC | MSG_WAITALL)) return -LINUX_EOPNOTSUPP;
    net_poll_packets();
    if (!s->head) {
        if ((file->flags & RELIEFOS_O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -LINUX_EAGAIN;
        struct task *task = sched_current_task();
        uint64_t now = time_ticks();
        if (!task->socket_io_deadline) {
            task->socket_io_timed = s->timeout_immediate || s->timeout != 0;
            task->socket_io_deadline = s->timeout_immediate ? now :
                !s->timeout || UINT64_MAX - now < s->timeout ? UINT64_MAX : now + s->timeout;
        }
        if (now >= task->socket_io_deadline) return -LINUX_EAGAIN;
        sched_sleep_current_until(now + 1);
        return KERNEL_SYSCALL_BLOCKED;
    }
    struct packet_message *m = s->head;
    if(staged_source && staged_length) {
        const void *source=s->domain==AF_PACKET ? (const void *)&m->link :
                           s->domain==AF_INET6 ? (const void *)&m->ip6 : (const void *)&m->ip;
        *staged_length=s->domain==AF_PACKET ? sizeof(m->link) :
                       s->domain==AF_INET6 ? sizeof(m->ip6) : sizeof(m->ip);
        __builtin_memcpy(staged_source,source,*staged_length);
    }
    if (address) {
        uint32_t capacity;
        int prep=socket_user_length(sched_current_task(),address_length,&capacity);
        if(prep)return prep;
        if ((int32_t)capacity < 0) return -LINUX_EINVAL;
        const void *source = s->domain == AF_PACKET ? (void *)&m->link :
                             s->domain == AF_INET6 ? (void *)&m->ip6 : (void *)&m->ip;
        uint32_t size = s->domain == AF_PACKET ? sizeof(m->link) :
                        s->domain == AF_INET6 ? sizeof(m->ip6) : sizeof(m->ip);
        uint32_t copied = capacity < size ? capacity : size;
        prep=socket_user_prepare(sched_current_task(),address,copied,true);if(prep)return prep;
        if (copied && !user_range_writable(address, copied)) return -LINUX_EFAULT;
        if (copied) __builtin_memcpy((void *)(uintptr_t)address, source, copied);
        *(uint32_t *)(uintptr_t)address_length = size;
    }
    if (info) *info = (struct net_udp_rx_info){.destination=m->destination6,.ifindex=m->ifindex,
        .hops=m->hops,.pktinfo=s->domain==AF_INET6 && s->recv_pktinfo,
        .hoplimit=s->domain==AF_INET6 && s->recv_hoplimit,
        .old_hoplimit=s->domain==AF_INET6 && s->recv_old_hoplimit};
    uint32_t copied = length < m->length ? length : m->length;
    if (copied) __builtin_memcpy(data, m->data, copied);
    int result = flags & MSG_TRUNC ? (int)m->length : (int)copied;
    if (!(flags & MSG_PEEK)) {
        s->head = m->next;
        if (!s->head) s->tail = NULL;
        --s->queued;
        kernel_free(m);
    }
    return result;
}

int task_packet_recv(struct task_file *file,void *data,uint32_t length,
                     uint32_t flags,uint64_t address,uint64_t address_length)
{ return packet_recv(file,data,length,flags,address,address_length,NULL,NULL,NULL); }

/** @brief Determine the native source sockaddr size for a packet endpoint.
 * @param file Pinned packet OFD. @return Native size, or zero for invalid OFD.
 */
uint32_t task_packet_address_size(const struct task_file *file)
{
    const struct packet_socket *s=packet_of(file);
    return !s ? 0 : s->domain==AF_PACKET ? sizeof(struct sockaddr_ll) :
           s->domain==AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
}

/** @brief Receive into owned buffers and copy IPv6 ingress metadata before dequeue.
 * @param file Pinned OFD. @param data Kernel destination. @param length Capacity.
 * @param flags Receive flags. @param source Optional native source output.
 * @param source_length Optional size output. @param info Optional ingress metadata.
 * @return Bytes, errno or the blocked sentinel. */
int task_packet_recv_ex(struct task_file *file,void *data,uint32_t length,uint32_t flags,
                        void *source,uint32_t *source_length,struct net_udp_rx_info *info)
{ return packet_recv(file,data,length,flags,0,0,source,source_length,info); }

#ifdef CONFIG_SWAP
/** @brief Receive packet bytes and source into caller-owned kernel storage.
 * @param file Pinned packet OFD. @param data Kernel destination. @param length
 * Destination capacity. @param flags Native receive flags. @param source Optional
 * kernel sockaddr. @param source_length Receives its size when source is given.
 * @return Byte count, blocking sentinel, or errno. PEEK retains the queue entry.
 */
int task_packet_recv_staged(struct task_file *file,void *data,uint32_t length,
                            uint32_t flags,void *source,uint32_t *source_length)
{ return packet_recv(file,data,length,flags,0,0,source,source_length,NULL); }
#endif

/** @brief Query packet readiness under the execution lock.
 * @param file Pinned socket.
 * @param events Requested readiness bits.
 * @return Readiness bits or POLLNVAL.
 */
short task_packet_poll(struct task_file *file, short events)
{
    struct packet_socket *s = packet_of(file);
    if (!s) return POLLNVAL;
    net_poll_packets();
    return (events & POLLOUT) | (s->head ? events & POLLIN : 0) | (s->error ? POLLERR : 0);
}

/** @brief Query the next datagram length.
 * @param file Pinned socket.
 * @return Length, zero, or EBADF.
 */
int task_packet_available(struct task_file *file)
{
    struct packet_socket *s = packet_of(file);
    return !s ? -LINUX_EBADF : s->head ? (int)s->head->length : 0;
}

/** @brief Free a final socket description and its bounded receive queue.
 * @param file Owned description under the execution lock.
 */
void task_packet_release(struct task_file *file)
{
    struct packet_socket *s = packet_of(file);
    if (!s) return;
    for (unsigned i = 0; i < PACKET_LIMIT; ++i) if (sockets[i] == s) sockets[i] = NULL;
    for (unsigned i = 0; i < 16; ++i)
        if (s->groups[i].used)
            (void)net_ipv6_multicast_change(&s->groups[i].address, s->groups[i].ifindex, -1);
    while (s->head) { struct packet_message *m = s->head; s->head = m->next; kernel_free(m); }
    kernel_free(s);
    file->aux = 0;
}

/** @brief Read or update a supported IPv6 raw-socket option.
 * @param s Raw socket. @param set True for a setter. @param level Native option level.
 * @param option Native option.
 * @param pointer User value. @param length_or_pointer Length or user socklen pointer.
 * @return Zero or negative errno. */
static int packet6_option(struct packet_socket *s, bool set, uint32_t level, uint32_t option,
                          uint64_t pointer, uint64_t length_or_pointer)
{
    struct task *task = sched_current_task();
    uint32_t length = (uint32_t)length_or_pointer;
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
        unsigned slot = 16, empty = 16;
        for (unsigned i = 0; i < 16; ++i) {
            if (!s->groups[i].used && empty == 16) empty = i;
            if (s->groups[i].used && s->groups[i].ifindex == ifindex &&
                !__builtin_memcmp(&s->groups[i].address, &group.ipv6mr_multiaddr, sizeof(group.ipv6mr_multiaddr))) slot = i;
        }
        bool join = option == IPV6_JOIN_GROUP;
        if (join && slot != 16) return -LINUX_EADDRINUSE;
        if (!join && slot == 16) return -LINUX_EADDRNOTAVAIL;
        if (join && empty == 16) return -LINUX_ENOBUFS;
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
        case IPV6_CHECKSUM:
            if ((s->protocol == IPPROTO_ICMPV6 && level == SOL_IPV6) || (value > 0 && (value & 1))) return -LINUX_EINVAL;
            s->checksum_offset = value < 0 ? -1 : value; break;
        case IPV6_UNICAST_HOPS: if (value < -1 || value > 255) return -LINUX_EINVAL; s->unicast_hops = value; break;
        case IPV6_MULTICAST_HOPS: if (value < -1 || value > 255) return -LINUX_EINVAL; s->multicast_hops = value; break;
        case IPV6_MULTICAST_IF: if (value < 0 || value > 2) return -LINUX_ENODEV; s->multicast_if = (uint32_t)value; break;
        case IPV6_MULTICAST_LOOP: if (value != 0 && value != 1) return -LINUX_EINVAL; s->multicast_loop = value != 0; break;
        case IPV6_RECVPKTINFO: s->recv_pktinfo = value != 0; break;
        case IPV6_RECVHOPLIMIT: s->recv_hoplimit = value != 0; break;
        case IPV6_2292HOPLIMIT: s->recv_old_hoplimit = value != 0; break;
        case IPV6_DONTFRAG: s->dont_fragment = value != 0; break;
        case IPV6_MTU_DISCOVER:
            if (value != IPV6_PMTUDISC_DONT && value != IPV6_PMTUDISC_WANT && value != IPV6_PMTUDISC_DO) return -LINUX_EINVAL;
            s->mtu_discover = value; break;
        default: return -LINUX_ENOPROTOOPT;
        }
        return 0;
    }
    switch (option) {
    case IPV6_CHECKSUM: value = s->checksum_offset; break;
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
        if (!s->connected6) return -LINUX_ENOTCONN;
        struct net_ipv6_path path;
        int ret = net_ipv6_route(&s->remote6, s->remote_scope6, &s->local6, &path);
        if (ret < 0) return ret;
        value = (int)path.mtu; break;
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

/** @brief Handle raw socket creation, binding and options with Linux-sized arguments.
 * @param number Native syscall number.
 * @param a0 First native argument.
 * @param a1 Second native argument.
 * @param a2 Third native argument.
 * @param a3 Fourth native argument.
 * @param a4 Fifth native argument.
 * @param a5 Sixth native argument.
 * @return Native result or negative errno; unsupported options are not accepted.
 */
int64_t syscall_packet(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a5;
    struct task *task = sched_current_task();
    if (!task) return -LINUX_ESRCH;
    if (number == __NR_socket) {
        int type = (int)a1 & 15;
        if ((uint32_t)a1 & ~(uint32_t)(15 | SOCK_NONBLOCK | SOCK_CLOEXEC)) return -LINUX_EINVAL;
        if (type != SOCK_RAW && !((int)a0 == AF_PACKET && type == SOCK_DGRAM) &&
            !((int)a0 == AF_INET6 && type == SOCK_DGRAM && (int)a2 == IPPROTO_ICMPV6)) return -LINUX_ESOCKTNOSUPPORT;
        if (((int)a0 == AF_INET || (int)a0 == AF_INET6) && ((int)a2 <= 0 || (int)a2 > 255)) return -LINUX_EPROTONOSUPPORT;
        if (!(task->cap_effective & (1ULL << CAP_NET_RAW))) return -LINUX_EPERM;
        unsigned index = 0;
        while (index < PACKET_LIMIT && sockets[index]) ++index;
        if (index == PACKET_LIMIT) return -LINUX_ENFILE;
        struct packet_socket *s = kernel_malloc(sizeof(*s));
        if (!s) return -LINUX_ENOMEM;
        struct task_file *file;
        int fd = task_allocate_fd(task, 0, &file);
        if (fd < 0) { kernel_free(s); return fd; }
        *s = (struct packet_socket){.domain = (int)a0, .type = type, .protocol = (int)a2,
            .uid = task->euid, .ino = (4ULL << 60) | ++packet_ino,
            .hdrincl = (int)a0 == AF_INET && (int)a2 == 255,
            .checksum_offset = ((int)a0 == AF_INET6 && (int)a2 == IPPROTO_ICMPV6) ? 2 : -1,
            .unicast_hops = -1, .multicast_hops = 1, .mtu_discover = IPV6_PMTUDISC_WANT, .multicast_loop = true};
        if (s->domain == AF_INET6 && s->type == SOCK_DGRAM) {
            uint16_t id = (uint16_t)packet_ino;
            if (!id) id = 1;
            for (unsigned tries = 0; tries < 65535; ++tries) {
                bool used = false;
                for (unsigned i = 0; i < PACKET_LIMIT; ++i)
                    if (sockets[i] && sockets[i]->domain == AF_INET6 && sockets[i]->type == SOCK_DGRAM && sockets[i]->ping_id == id) used = true;
                if (!used) break;
                if (++id == 0) id = 1;
            }
            s->ping_id = id;
        }
        sockets[index] = s;
        file->flags = TASK_FILE_FLAG_SOCKET | TASK_FILE_FLAG_SOCKET_INET | RELIEFOS_O_RDWR | (a1 & SOCK_NONBLOCK ? RELIEFOS_O_NONBLOCK : 0);
        file->fd_flags = a1 & SOCK_CLOEXEC ? RELIEFOS_FD_CLOEXEC : 0;
        file->kind = TASK_FILE_KIND_PACKET;
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
    struct packet_socket *s = packet_of(file);
    if (!s) return -LINUX_EBADF;
    if (number == __NR_bind || number == __NR_connect) {
        if (s->domain == AF_PACKET) {
            if (number == __NR_connect) return -LINUX_EOPNOTSUPP;
            if ((int32_t)a2 < 0 || (uint32_t)a2 < sizeof(struct sockaddr_ll)) return -LINUX_EINVAL;
            int prep=socket_user_prepare(task,a1,sizeof(struct sockaddr_ll),false);if(prep)return prep;
            if (!user_range_ok(a1, sizeof(struct sockaddr_ll))) return -LINUX_EFAULT;
            struct sockaddr_ll address;
            __builtin_memcpy(&address, (void *)(uintptr_t)a1, sizeof(address));
            if (address.sll_family != AF_PACKET) return -LINUX_EINVAL;
            if (address.sll_ifindex && address.sll_ifindex != 2) return -LINUX_ENODEV;
            s->ifindex = address.sll_ifindex;
            s->protocol = address.sll_protocol;
        } else if (s->domain == AF_INET6) {
            struct sockaddr_in6 address;
            int ret = packet6_import(a1, (uint32_t)a2, &address);
            if (ret < 0) return ret;
            if (address.sin6_scope_id > 2) return -LINUX_ENODEV;
            if (number == __NR_connect) {
                s->remote6 = address.sin6_addr; s->remote_scope6 = address.sin6_scope_id; s->connected6 = true;
            } else {
                if (s->type == SOCK_DGRAM && address.sin6_port) {
                    uint16_t id = __builtin_bswap16(address.sin6_port);
                    for (unsigned i = 0; i < PACKET_LIMIT; ++i)
                        if (sockets[i] && sockets[i] != s && sockets[i]->domain == AF_INET6 &&
                            sockets[i]->type == SOCK_DGRAM && sockets[i]->ping_id == id) return -LINUX_EADDRINUSE;
                    s->ping_id = id;
                }
                if (!packet6_any(&address.sin6_addr) && address.sin6_addr.s6_addr[0] != 0xff) {
                    struct net_ipv6_address entries[32]; uint32_t count = net_ipv6_addresses(entries, 32); bool found = false;
                    for (uint32_t i = 0; i < count && i < 32; ++i)
                        if (!__builtin_memcmp(&entries[i].address, &address.sin6_addr, sizeof(address.sin6_addr)) &&
                            !(entries[i].flags & (NET_IPV6_ADDR_TENTATIVE | NET_IPV6_ADDR_DADFAILED)) &&
                            (!address.sin6_scope_id || entries[i].ifindex == address.sin6_scope_id)) found = true;
                    if (!found) return -LINUX_EADDRNOTAVAIL;
                }
                s->local6 = address.sin6_addr; s->scope6 = address.sin6_scope_id; s->bound6 = true;
            }
        } else {
            if ((int32_t)a2 < 0 || (uint32_t)a2 < sizeof(struct sockaddr_in)) return -LINUX_EINVAL;
            int prep=socket_user_prepare(task,a1,sizeof(struct sockaddr_in),false);if(prep)return prep;
            if (!user_range_ok(a1, sizeof(struct sockaddr_in))) return -LINUX_EFAULT;
            struct sockaddr_in address;
            __builtin_memcpy(&address, (void *)(uintptr_t)a1, sizeof(address));
            if (address.sin_family != AF_INET) return -LINUX_EAFNOSUPPORT;
            if (number == __NR_connect) s->remote_ip = address.sin_addr.s_addr;
            else {
                struct reliefos_net_config config;
                net_get_config(&config);
                if (address.sin_addr.s_addr && address.sin_addr.s_addr != __builtin_bswap32(config.local_ip))
                    return -LINUX_EADDRNOTAVAIL;
                s->local_ip = address.sin_addr.s_addr;
            }
        }
        return 0;
    }
    if (s->domain == AF_INET6 && (number == __NR_getsockname || number == __NR_getpeername)) {
        if (number == __NR_getpeername && !s->connected6) return -LINUX_ENOTCONN;
        uint32_t length; int ret = socket_user_length(task, a2, &length); if (ret) return ret;
        if ((int32_t)length < 0) return -LINUX_EINVAL;
        struct sockaddr_in6 address = {.sin6_family = AF_INET6,
            .sin6_addr = number == __NR_getpeername ? s->remote6 : s->local6,
            .sin6_scope_id = number == __NR_getpeername ? s->remote_scope6 : s->scope6};
        if (number == __NR_getsockname && s->type == SOCK_DGRAM) address.sin6_port = __builtin_bswap16(s->ping_id);
        if (number == __NR_getsockname && s->connected6 && packet6_any(&address.sin6_addr)) {
            struct net_ipv6_path path;
            if (!net_ipv6_route(&s->remote6, s->remote_scope6, &s->local6, &path)) address.sin6_addr = path.source;
        }
        if (length > sizeof(address)) length = sizeof(address);
        ret = socket_user_prepare(task, a1, length, true); if (ret) return ret;
        if (length && !user_range_writable(a1, length)) return -LINUX_EFAULT;
        if (length) __builtin_memcpy((void *)(uintptr_t)a1, &address, length);
        *(uint32_t *)(uintptr_t)a2 = sizeof(address);
        return 0;
    }
    if (s->domain == AF_INET6 && (number == __NR_setsockopt || number == __NR_getsockopt)) {
        bool set = number == __NR_setsockopt;
        if (a1 == IPPROTO_ICMPV6) {
            if (s->protocol != IPPROTO_ICMPV6) return -LINUX_EOPNOTSUPP;
            if (a2 != PACKET_ICMP6_FILTER) return -LINUX_ENOPROTOOPT;
            uint32_t length = (uint32_t)a4;
            if (!set) { int ret = socket_user_length(task, a4, &length); if (ret) return ret; }
            if ((int32_t)length < 0) return -LINUX_EINVAL;
            if (length > sizeof(s->icmp6_filter)) length = sizeof(s->icmp6_filter);
            int ret = socket_user_prepare(task, a3, length, !set); if (ret) return ret;
            if (length && !(set ? user_range_ok(a3,length) : user_range_writable(a3,length))) return -LINUX_EFAULT;
            if (length) {
                if (set) __builtin_memcpy(s->icmp6_filter, (void *)(uintptr_t)a3, length);
                else __builtin_memcpy((void *)(uintptr_t)a3, s->icmp6_filter, length);
            }
            if (!set) *(uint32_t *)(uintptr_t)a4 = length;
            return 0;
        }
        if (a1 == SOL_IPV6 || (a1 == PACKET_SOL_RAW && a2 == IPV6_CHECKSUM))
            return packet6_option(s, set, (uint32_t)a1, (uint32_t)a2, a3, a4);
        if (a1 == PACKET_SOL_RAW) return -LINUX_ENOPROTOOPT;
    }
    if (number == __NR_getsockopt && a1 == SOL_SOCKET) {
        if (a2 == SO_RCVTIMEO || a2 == SO_RCVTIMEO_NEW) {
            int64_t timeout[2] = {(int64_t)(s->timeout / RELIEFNT_TICK_HZ),
                (int64_t)((s->timeout % RELIEFNT_TICK_HZ) * 1000000u / RELIEFNT_TICK_HZ)};
            uint32_t length; int ret = socket_user_length(task, a4, &length); if (ret) return ret;
            if ((int32_t)length < 0) return -LINUX_EINVAL;
            if (length > sizeof(timeout)) length = sizeof(timeout);
            ret = socket_user_prepare(task, a3, length, true); if (ret) return ret;
            if (length && !user_range_writable(a3, length)) return -LINUX_EFAULT;
            if (length) __builtin_memcpy((void *)(uintptr_t)a3, timeout, length);
            *(uint32_t *)(uintptr_t)a4 = length;
            return 0;
        }
        int value;
        if (a2 == SO_TYPE) value = s->type;
        else if (a2 == SO_DOMAIN) value = s->domain;
        else if (a2 == SO_PROTOCOL) value = s->protocol;
        else if (a2 == SO_ERROR) value = s->error;
        else return -LINUX_ENOPROTOOPT;
        uint32_t length; int ret = socket_user_length(task, a4, &length); if (ret) return ret;
        if ((int32_t)length < 0) return -LINUX_EINVAL;
        if (length > sizeof(value)) length = sizeof(value);
        ret = socket_user_prepare(task, a3, length, true); if (ret) return ret;
        if (length && !user_range_writable(a3, length)) return -LINUX_EFAULT;
        if (length) __builtin_memcpy((void *)(uintptr_t)a3, &value, length);
        *(uint32_t *)(uintptr_t)a4 = length;
        if (a2 == SO_ERROR) s->error = 0;
        return 0;
    }
    if (number == __NR_setsockopt) {
        if ((int)a1 == SOL_SOCKET && ((int)a2 == SO_RCVTIMEO || (int)a2 == SO_RCVTIMEO_NEW)) {
            int64_t timeout[2];
            if ((uint32_t)a4 < sizeof(timeout)) return -LINUX_EINVAL;
            int prep=socket_user_prepare(task,a3,sizeof(timeout),false);if(prep)return prep;
            if (!user_range_ok(a3, sizeof(timeout))) return -LINUX_EFAULT;
            __builtin_memcpy(timeout, (void *)(uintptr_t)a3, sizeof(timeout));
            if (timeout[1] < 0 || timeout[1] >= 1000000) return -LINUX_EDOM;
            if (timeout[0] < 0) { s->timeout = 0; s->timeout_immediate = true; }
            else {
                s->timeout_immediate = false;
                if ((uint64_t)timeout[0] > (UINT64_MAX - RELIEFNT_TICK_HZ) / RELIEFNT_TICK_HZ) s->timeout = UINT64_MAX;
                else s->timeout = (uint64_t)timeout[0] * RELIEFNT_TICK_HZ +
                                  ((uint64_t)timeout[1] * RELIEFNT_TICK_HZ + 999999) / 1000000;
            }
            return 0;
        }
        if (!((int)a1 == SOL_SOCKET && (int)a2 == 6) &&
            !((int)a1 == 0 && (int)a2 == 3 && s->domain == AF_INET)) return -LINUX_ENOPROTOOPT;
        if ((uint32_t)a4 < 4) return -LINUX_EINVAL;
        int prep=socket_user_prepare(task,a3,4,false);if(prep)return prep;
        if (!user_range_ok(a3, 4)) return -LINUX_EFAULT;
        bool value = *(int *)(uintptr_t)a3 != 0;
        if ((int)a1 == SOL_SOCKET) s->broadcast = value;
        else s->hdrincl = value;
        return 0;
    }
    return -LINUX_EOPNOTSUPP;
}

/** @brief Report ICMPv6 errors to the socket owning the quoted protocol tuple.
 * @param packet Quoted original IPv6 packet. @param length Quote size.
 * @param error Positive Linux errno. @param mtu Reported path MTU or zero. */
void net_ipv6_socket_error(const uint8_t *packet, uint32_t length, int error, uint32_t mtu)
{
    if (!packet || length < 40 || packet[0] >> 4 != 6) return;
    struct in6_addr source, destination;
    __builtin_memcpy(&source, packet + 8, 16); __builtin_memcpy(&destination, packet + 24, 16);
    uint8_t protocol = packet[6];
    if ((protocol == 6 || protocol == 17) && length >= 48) {
        uint16_t source_port = ((uint16_t)packet[40] << 8) | packet[41];
        uint16_t destination_port = ((uint16_t)packet[42] << 8) | packet[43];
        if (protocol == 6) net_tcp6_error(&source, &destination, source_port, destination_port, error, mtu);
        else net_udp6_error(&source, &destination, source_port, destination_port, error, mtu);
    }
    for (unsigned i = 0; i < PACKET_LIMIT; ++i) {
        struct packet_socket *s = sockets[i];
        if (!s || s->domain != AF_INET6 || s->protocol != protocol ||
            (!packet6_any(&s->local6) && __builtin_memcmp(&s->local6, &source, sizeof(source))) ||
            (s->connected6 && __builtin_memcmp(&s->remote6, &destination, sizeof(destination)))) continue;
        s->error = error;
    }
    kernel_poll_readiness_notify();
}

/** @brief Render raw IPv6 endpoints with their actual receive queue sizes.
 * @param stream Caller-owned proc output. */
static void net_raw6_proc_emit(struct text_stream *stream)
{
    text_string(stream,"  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    unsigned index = 0;
    for (unsigned i = 0; i < PACKET_LIMIT; ++i) {
        struct packet_socket *s = sockets[i];
        if (!s || s->domain != AF_INET6) continue;
        uint64_t queued = 0; for (struct packet_message *m = s->head; m; m = m->next) queued += m->length;
        text_unsigned(stream,index++); text_string(stream,": ");
        for (unsigned j = 0; j < 16; j += 4) { uint32_t word; __builtin_memcpy(&word,s->local6.s6_addr+j,4); text_hex(stream,word,8); }
        text_string(stream,":"); text_hex(stream,(uint32_t)s->protocol,4); text_string(stream," ");
        for (unsigned j = 0; j < 16; j += 4) { uint32_t word; __builtin_memcpy(&word,s->remote6.s6_addr+j,4); text_hex(stream,word,8); }
        text_string(stream,":0000 07 00000000:"); text_hex(stream,queued,8);
        text_string(stream," 00:00000000 00000000 "); text_unsigned(stream,s->uid);
        text_string(stream," 0 "); text_unsigned(stream,s->ino); text_string(stream,"\n");
    }
}

/** @brief Emit the selected standard proc IPv6 transport view.
 * @param stream Caller-owned output slice. @param leaf Requested basename. */
void net_ipv6_transport_proc_emit(struct text_stream *stream, const char *leaf)
{
    if (!__builtin_strcmp(leaf,"tcp6")) net_tcp6_proc_emit(stream);
    else if (!__builtin_strcmp(leaf,"udp6")) net_udp6_proc_emit(stream);
    else if (!__builtin_strcmp(leaf,"raw6")) net_raw6_proc_emit(stream);
}
