#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../net/socket/packet.c"
#include "../../net/interface/socket.c"
static struct task current;
static struct task_file description;
static uint8_t sent[1514];
static uint32_t sent_length;
struct task *sched_current_task(void) { return &current; }
void *kernel_malloc(size_t n) { return malloc(n); }
void kernel_free(void *p) { free(p); }
int task_allocate_fd(struct task *t, int minimum, struct task_file **f)
{ (void)t; (void)minimum; *f = &description; return 3; }
struct task_file *task_file_for_fd(struct task *t, int fd)
{ (void)t; return fd == 3 ? &description : NULL; }
bool user_range_ok(uint64_t p, uint64_t n) { return p >= 4096 && n < 65536; }
bool user_range_writable(uint64_t p, uint64_t n) { return user_range_ok(p,n); }
uint64_t time_ticks(void) { return 1; }
void sched_sleep_current_until(uint64_t deadline) { (void)deadline; }
void net_poll_packets(void) {}
void kernel_poll_readiness_notify(void) {}
uint32_t net_ipv6_addresses(struct net_ipv6_address *out, uint32_t capacity)
{ (void)out; (void)capacity; return 0; }
int net_ipv6_multicast_change(const struct in6_addr *address, uint32_t index, int delta)
{ (void)address; (void)index; (void)delta; return 0; }
int net_ipv6_route(const struct in6_addr *destination, uint32_t scope,
                   const struct in6_addr *source, struct net_ipv6_path *path)
{ (void)destination; (void)scope; (void)source; (void)path; return -LINUX_ENETUNREACH; }
int net_ipv6_output(const struct net_ipv6_path *path, uint8_t protocol,
                    const void *data, uint32_t length, int hops, uint32_t flow,
                    bool dont_fragment, bool multicast_loop)
{ (void)path; (void)protocol; (void)data; (void)length; (void)hops; (void)flow;
  (void)dont_fragment; (void)multicast_loop; return -LINUX_ENETUNREACH; }
uint16_t net_ipv6_checksum(const struct in6_addr *source, const struct in6_addr *destination,
                          uint8_t protocol, const void *data, uint32_t length)
{ (void)source; (void)destination; (void)protocol; (void)data; (void)length; return 0; }
bool net_interface_ready(void) { return true; }
const uint8_t *e1000_mac(void) { static const uint8_t mac[6] = {2,3,4,5,6,7}; return mac; }
int e1000_send(const void *p, uint32_t n) { assert(n <= sizeof(sent)); memcpy(sent,p,n); sent_length=n; return 0; }
int net_get_config(struct reliefos_net_config *c) { *c=(struct reliefos_net_config){0}; return 0; }
int net_ipv4_send_raw(uint32_t d, uint32_t s, uint8_t p, bool h, const void *b, uint32_t n)
{ (void)d; (void)s; (void)p; (void)h; (void)b; return n; }
bool net_interface_exists(uint32_t index) { return index >= 1 && index <= 3; }
bool net_interface_is_ready(uint32_t index) { return index >= 2 && index <= 3; }
uint32_t net_interface_index(const char *name)
{ return !strcmp(name,"lo") ? 1 : !strcmp(name,"eth0") ? 2 : !strcmp(name,"eth1") ? 3 : 0; }
bool net_interface_name(uint32_t index, char out[16])
{ if (!net_interface_exists(index)) return false; strcpy(out,index == 1 ? "lo" : index == 2 ? "eth0" : "eth1"); return true; }
uint32_t net_interface_for_ipv4(uint32_t ip) { return (ip >> 24) == 127 ? 1 : 0; }
uint32_t netdev_next(uint32_t after) { return after < 2 ? 2 : after == 2 ? 3 : 0; }
const uint8_t *netdev_mac(uint32_t index)
{ static const uint8_t macs[2][6] = {{2,3,4,5,6,7},{2,3,4,5,6,8}}; return macs[index == 3]; }
static uint32_t sent_interface;
int netdev_send(uint32_t index, const void *data, uint32_t length)
{ sent_interface = index; return e1000_send(data, length); }
int net_ipv4_send_raw_on(uint32_t index, uint32_t d, uint32_t s, uint8_t p,
                          bool h, const void *data, uint32_t length)
{ (void)index; return net_ipv4_send_raw(d,s,p,h,data,length); }
int main(void)
{
    assert(syscall_packet(__NR_socket, AF_PACKET, SOCK_DGRAM, 8,0,0,0) == -LINUX_EPERM);
    current.cap_effective = 1ULL << CAP_NET_RAW;
    assert(syscall_packet(__NR_socket, AF_PACKET, SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 8,0,0,0) == 3);
    assert(description.flags & TASK_FILE_FLAG_SOCKET);
    assert(description.flags & TASK_FILE_FLAG_SOCKET_INET);
    assert(description.fd_flags == RELIEFOS_FD_CLOEXEC);
    struct sockaddr_ll dest = {.sll_family=AF_PACKET,.sll_ifindex=2,.sll_protocol=8,.sll_halen=6,
        .sll_addr={255,255,255,255,255,255}};
    dest.sll_ifindex = 3;
    assert(syscall_packet(__NR_bind,3,(uintptr_t)&dest,sizeof(dest),0,0,0) == 0);
    const char second[] = "second link";
    assert(task_packet_send(&description,second,sizeof(second),0,(uintptr_t)&dest,sizeof(dest)) == sizeof(second));
    assert(sent_interface == 3 && !memcmp(sent+6,netdev_mac(3),6));
    net_packet_input_on(2,sent,sent_length);
    assert(task_packet_available(&description) == 0);
    net_packet_input_on(3,sent,sent_length);
    struct sockaddr_ll ingress; uint32_t ingress_length=sizeof(ingress); char second_out[sizeof(second)];
    assert(task_packet_recv(&description,second_out,sizeof(second_out),0,(uintptr_t)&ingress,(uintptr_t)&ingress_length) == sizeof(second));
    assert(ingress.sll_ifindex == 3 && !memcmp(second_out,second,sizeof(second)));
    dest.sll_ifindex = 2;
    assert(syscall_packet(__NR_bind,3,(uintptr_t)&dest,-1,0,0,0) == -LINUX_EINVAL);
    assert(syscall_packet(__NR_bind,3,(uintptr_t)&dest,sizeof(dest),0,0,0) == 0);
    const char data[]="packet regression";
    assert(task_packet_send(&description,data,sizeof(data),0,(uintptr_t)&dest,sizeof(dest)) == sizeof(data));
    assert(sent_length == 14+sizeof(data) && !memcmp(sent+14,data,sizeof(data)));
    assert(!memcmp(sent+6,e1000_mac(),6));
    net_packet_input(sent,sent_length);
    assert(task_packet_available(&description) == sizeof(data));
    char out[sizeof(data)]; struct sockaddr_ll source; uint32_t len=sizeof(source);
    assert(task_packet_recv(&description,out,2,MSG_PEEK|MSG_TRUNC,(uintptr_t)&source,(uintptr_t)&len) == sizeof(data));
    assert(len == sizeof(source) && source.sll_pkttype == PACKET_BROADCAST && source.sll_ifindex == 2);
    assert(task_packet_available(&description) == sizeof(data));
    assert(task_packet_recv(&description,out,sizeof(out),0,0,0) == sizeof(data));
    assert(!memcmp(out,data,sizeof(data)));
    assert(task_packet_recv(&description,out,sizeof(out),0,0,0) == -LINUX_EAGAIN);
    task_packet_release(&description);
    for (unsigned i=0;i<PACKET_LIMIT;i++) assert(!sockets[i]);
    puts("PASS real packet socket CAP_NET_RAW, descriptor flags, wire frame, receive/peek/trunc and release");
}
