#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../net/socket/udp.c"

static struct task current;
static struct task_file files[80];
static unsigned allocations;
static unsigned sleeps;
static uint64_t ticks = 100;
struct task *sched_current_task(void) { return &current; }
void *kernel_malloc(size_t bytes) { ++allocations; return calloc(1, bytes); }
void kernel_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
uint64_t time_ticks(void) { return ticks; }
void sched_sleep_current_until(uint64_t tick) { assert(tick == ticks + 1); ++sleeps; }
bool user_range_ok(uint64_t address, uint64_t size) { (void)size; return address > 4096; }
bool user_range_writable(uint64_t address, uint64_t size) { return user_range_ok(address, size); }
int task_allocate_fd(struct task *task, int minimum, struct task_file **out)
{
    (void)task;
    for (unsigned i = minimum; i < 80; ++i) if (!files[i].used) {
        files[i].used = 1; *out = &files[i]; return (int)i;
    }
    return -LINUX_EMFILE;
}
struct task_file *task_file_for_fd(struct task *task, int fd)
{ (void)task; return fd >= 0 && fd < 80 && files[fd].used ? &files[fd] : NULL; }
int net_get_config(struct reliefos_net_config *config)
{ *config = (struct reliefos_net_config){.local_ip = 0x0a00020f}; return 0; }
void net_poll_packets(void) {}
#ifndef IPV6_TRANSPORT_FIXTURE
/* This legacy fixture exercises IPv4 only; IPv6 dependencies are supplied by
 * the dedicated IPv6 transport fixture with an actual deferred link model. */
__attribute__((weak)) void kernel_poll_readiness_notify(void) {}
__attribute__((weak)) uint32_t net_ipv6_addresses(struct net_ipv6_address *out,uint32_t capacity)
{ (void)out; (void)capacity; return 0; }
__attribute__((weak)) int net_ipv6_route(const struct in6_addr *destination,uint32_t scope,
                                         const struct in6_addr *bound,struct net_ipv6_path *path)
{ (void)destination; (void)scope; (void)bound; (void)path; return -LINUX_ENETUNREACH; }
__attribute__((weak)) uint16_t net_ipv6_checksum(const struct in6_addr *source,const struct in6_addr *destination,
                                                  uint8_t protocol,const void *data,uint32_t length)
{ (void)source; (void)destination; (void)protocol; (void)data; (void)length; return 0; }
__attribute__((weak)) int net_ipv6_output(const struct net_ipv6_path *path,uint8_t protocol,const void *data,
                                           uint32_t length,int hops,uint32_t flowinfo,bool df,bool multicast_loop)
{ (void)path; (void)protocol; (void)data; (void)length; (void)hops; (void)flowinfo; (void)df; (void)multicast_loop; return -LINUX_ENETUNREACH; }
__attribute__((weak)) int net_ipv6_multicast_change(const struct in6_addr *address,uint32_t ifindex,int delta)
{ (void)address; (void)ifindex; (void)delta; return -LINUX_ENETUNREACH; }
#endif
int net_ipv4_send_udp(uint32_t source, uint32_t destination, uint16_t source_port,
                       uint16_t destination_port, const void *data, uint32_t length)
{ (void)source; (void)destination; (void)source_port; (void)destination_port; (void)data; (void)length; return -LINUX_ENETDOWN; }

static long call(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{ return syscall_udp(nr, a, b, c, d, e, 0); }
static int open_socket(void)
{ return (int)call(__NR_socket, AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, 0, 0); }

int main(void)
{
    current.cap_effective = 1ULL << CAP_NET_BIND_SERVICE;
    current.euid = 1234;
    int a = open_socket(), b = open_socket();
    assert(a >= 0 && b >= 0 && a != b);
    assert(files[a].fd_flags == 1 && (files[a].flags & RELIEFOS_O_NONBLOCK));
    struct sockaddr_in dest = {.sin_family = AF_INET, .sin_port = swap16(55000),
                               .sin_addr = {.s_addr = swap32(0x7f000001)}};
    assert(call(__NR_bind, b, (uintptr_t)&dest, sizeof(dest), 0, 0) == 0);
    assert(call(__NR_bind, a, (uintptr_t)&dest, sizeof(dest), 0, 0) == -LINUX_EADDRINUSE);
    assert(call(__NR_connect, a, (uintptr_t)&dest, sizeof(dest), 0, 0) == 0);
    char text[16] = {0};
    assert(task_udp_recv(&files[b], text, sizeof(text), 0, 0, 0) == -LINUX_EAGAIN);
    assert(task_udp_send(&files[a], "hello", 5, 0, 0, 0) == 5);
    char proc[2048] = {0}, identity[64];
    struct text_stream stream = {.buffer = proc, .capacity = sizeof(proc) - 1};
    net_udp_proc_emit(&stream);
    snprintf(identity, sizeof(identity), " 1234 0 %llu\n", (unsigned long long)files[b].socket_ino);
    assert(strstr(proc, "0100007f:d6d8") && strstr(proc, "00000000:00000005"));
    assert(files[b].socket_ino && strstr(proc, identity));
    memset(proc, 0, sizeof(proc));
    stream = (struct text_stream){.buffer = proc, .capacity = sizeof(proc) - 1};
    net_loopback_proc_emit(&stream);
    assert(!strcmp(proc, "    lo: 33 1 0 0 0 0 0 0 33 1 0 0 0 0 0 0\n"));
    assert(task_udp_poll(&files[b], POLLIN | POLLOUT) == (POLLIN | POLLOUT));
    assert(task_udp_recv(&files[b], text, 2, MSG_PEEK | MSG_TRUNC, 0, 0) == 5);
    assert(!memcmp(text, "he", 2));
    struct sockaddr_in source;
    uint32_t size = sizeof(source);
    assert(task_udp_recv(&files[b], text, sizeof(text), 0, (uintptr_t)&source, (uintptr_t)&size) == 5);
    assert(source.sin_family == AF_INET && source.sin_addr.s_addr == swap32(0x7f000001));
    assert(task_udp_recv(&files[b], text, sizeof(text), 0, 0, 0) == -LINUX_EAGAIN);
    assert(task_udp_send(&files[a], "", 0, 0, 0, 0) == 0);
    assert(task_udp_poll(&files[b], POLLIN) == POLLIN);
    assert(task_udp_recv(&files[b], text, 0, 0, 0, 0) == 0);
    assert(task_udp_poll(&files[b], POLLIN) == 0);
    int64_t timeout[2] = {0, 1000};
    assert(call(__NR_setsockopt, b, SOL_SOCKET, SO_RCVTIMEO, (uintptr_t)timeout, sizeof(timeout)) == 0);
    size = sizeof(timeout);
    assert(call(__NR_getsockopt, b, SOL_SOCKET, SO_RCVTIMEO, (uintptr_t)timeout, (uintptr_t)&size) == 0);
    assert(timeout[0] == 0 && timeout[1] == 1000000 / RELIEFNT_TICK_HZ);
    files[b].flags &= ~RELIEFOS_O_NONBLOCK;
    assert(task_udp_recv(&files[b], text, sizeof(text), 0, 0, 0) == KERNEL_SYSCALL_BLOCKED);
    ++ticks;
    assert(task_udp_recv(&files[b], text, sizeof(text), 0, 0, 0) == -LINUX_EAGAIN);
    current.socket_io_deadline = 0;
    current.socket_io_timed = false;
    int64_t negative_timeout[2] = {-1, 500000};
    assert(call(__NR_setsockopt, b, SOL_SOCKET, SO_RCVTIMEO,
                (uintptr_t)negative_timeout, sizeof(negative_timeout)) == 0);
    size = sizeof(negative_timeout);
    assert(call(__NR_getsockopt, b, SOL_SOCKET, SO_RCVTIMEO,
                (uintptr_t)negative_timeout, (uintptr_t)&size) == 0);
    assert(negative_timeout[0] == 0 && negative_timeout[1] == 0);
    unsigned sleeps_before = sleeps;
    assert(task_udp_recv(&files[b], text, sizeof(text), 0, 0, 0) == -LINUX_EAGAIN);
    assert(current.socket_io_timed && sleeps == sleeps_before);
    files[b].flags |= RELIEFOS_O_NONBLOCK;
    assert(call(__NR_setsockopt, b, SOL_SOCKET, SO_SNDTIMEO, (uintptr_t)timeout, sizeof(timeout)) == -LINUX_ENOPROTOOPT);
    assert(call(__NR_getsockopt, b, SOL_SOCKET, SO_SNDTIMEO, (uintptr_t)timeout, (uintptr_t)&size) == -LINUX_ENOPROTOOPT);
    int value = -1;
    size = 4;
    assert(call(__NR_getsockopt, a, SOL_SOCKET, SO_TYPE, (uintptr_t)&value, (uintptr_t)&size) == 0);
    assert(value == SOCK_DGRAM && size == 4);
    assert(call(__NR_getsockopt, a, SOL_SOCKET, 99999, (uintptr_t)&value, (uintptr_t)&size) == -LINUX_ENOPROTOOPT);
    assert(call(__NR_bind, a, 1, 16, 0, 0) == -LINUX_EFAULT);
    assert(task_udp_send(&files[a], text, 1473, 0, 0, 0) == -LINUX_EMSGSIZE);
    for (unsigned i = 0; i < 100; ++i) assert(task_udp_send(&files[a], "x", 1, 0, 0, 0) == 1);
    assert(udp_from_file(&files[b])->queued == UDP_QUEUE_LENGTH);
    int extra[70];
    for (unsigned i = 0; i < 70; ++i) {
        extra[i] = open_socket();
        assert(extra[i] >= 0);
    }
    for (unsigned i = 0; i < 70; ++i) task_udp_release(&files[extra[i]]);
    task_udp_release(&files[a]); task_udp_release(&files[b]);
    assert(!sockets);
    assert(!allocations);
    puts("PASS UDP bind, connect, datagram boundaries, source address, truncation/peek, zero length, options and cleanup");
}
