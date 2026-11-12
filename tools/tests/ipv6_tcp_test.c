/* Exercise TCP's real socket, input and output state machines over a deferred link. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../net/internal.h"

struct net_socket *net_sockets;
static struct reliefos_net_config fixture_config;
static bool fixture_interface_up;
uint32_t net_sequence;
uint16_t net_ipv4_id;
static struct task current;
static uint64_t now = 1000;
static const struct in6_addr loop6 = {.s6_addr = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}};
struct queued { struct net_ipv6_path path; uint32_t length; uint8_t data[1500]; };
static struct queued queue[32];
static unsigned head, tail;

void *kernel_malloc(size_t n) { return calloc(1, n); }
void kernel_free(void *p) { free(p); }
struct task *sched_current_task(void) { return &current; }
uint64_t time_uptime_ms(void) { return now; }
uint64_t time_ticks(void) { return now; }
void sched_sleep_current_until(uint64_t tick) { (void)tick; }
void kernel_wait_queue_init(struct kernel_wait_queue *q) { memset(q, 0, sizeof(*q)); }
int kernel_wait_queue_add(struct kernel_wait_queue *q, struct task *t) { (void)q; (void)t; return 0; }
void kernel_wait_queue_remove(struct kernel_wait_queue *q, struct task *t) { (void)q; (void)t; }
uint32_t kernel_wait_queue_wake_all(struct kernel_wait_queue *q) { (void)q; return 0; }
void kernel_poll_readiness_notify(void) {}
void console_log(enum console_log_level level, const char *format, ...) { (void)level; (void)format; }
int net_get_config(struct reliefos_net_config *out) { *out = net_config; return 0; }
struct reliefos_net_config *net_interface_legacy_config(void) { return &fixture_config; }
bool *net_interface_legacy_up(void) { return &fixture_interface_up; }
bool net_interface_exists(uint32_t index) { return index==1; }
bool net_interface_is_ready(uint32_t index) { return index==1; }
uint32_t net_interface_for_ipv4(uint32_t ip) { return (ip>>24)==127?1:0; }
uint32_t netdev_next(uint32_t after) { (void)after;return 0; }
int netdev_send(uint32_t index,const void *data,uint32_t length)
{ (void)index;(void)data;(void)length;return -LINUX_ENETDOWN; }
int net_ipv4_route_lookup(uint32_t destination,uint32_t source,uint32_t scope,struct net_ipv4_path *out)
{
    if((destination>>24)!=127 || (scope && scope!=1))return -LINUX_ENETUNREACH;
    *out=(struct net_ipv4_path){.source=source?source:0x7f000001u,.destination=destination,
                                .next_hop=destination,.ifindex=1};
    return 0;
}
int e1000_is_ready(void) { return 0; }
int e1000_send(const void *data, uint32_t length) { (void)data; (void)length; return -LINUX_ENETDOWN; }
int net_resolve_mac(uint32_t ip, uint32_t timeout, uint8_t *out) { (void)ip; (void)timeout; (void)out; return -1; }
int net_resolve_mac_on(uint32_t index,uint32_t ip,uint32_t timeout,uint8_t *out)
{ (void)index;return net_resolve_mac(ip,timeout,out); }
uint32_t net_route_arp_ip(uint32_t ip) { return ip; }
uint32_t net_ensure_ipv4_config(uint32_t timeout, int dns) { (void)timeout; (void)dns; return RELIEFOS_NET_STATUS_OK; }
void net_poll_once(struct net_arp_wait *a, struct net_ping_wait *p, struct net_udp_wait *u, struct net_tcp_wait *t)
{ (void)a; (void)p; (void)u; (void)t; }
void net_write_eth(uint8_t *frame, const uint8_t *mac, uint16_t type)
{ (void)frame; (void)mac; (void)type; }
void net_write_eth_on(uint32_t index,uint8_t *frame,const uint8_t *mac,uint16_t type)
{ (void)index;net_write_eth(frame,mac,type); }
uint32_t net_ipv6_addresses(struct net_ipv6_address *out,uint32_t cap)
{ if (cap) out[0] = (struct net_ipv6_address){.address=loop6,.ifindex=1}; return 1; }
int net_ipv6_route(const struct in6_addr *destination,uint32_t scope,
                   const struct in6_addr *bound,struct net_ipv6_path *path)
{
    static const struct in6_addr zero = {0};
    if (memcmp(destination,&loop6,16) || (scope && scope != 1)) return -LINUX_ENETUNREACH;
    *path = (struct net_ipv6_path){.source=memcmp(bound,&zero,16) ? *bound : loop6,
                                  .destination=*destination,.ifindex=1,.mtu=1280};
    return 0;
}
uint16_t net_ipv6_checksum(const struct in6_addr *source,const struct in6_addr *destination,
                           uint8_t protocol,const void *data,uint32_t length)
{
    uint8_t trailer[8] = {0}; net_put_u32(trailer,length); trailer[7] = protocol;
    uint32_t sum = net_checksum_partial(0,source->s6_addr,16);
    sum = net_checksum_partial(sum,destination->s6_addr,16);
    sum = net_checksum_partial(sum,trailer,8);
    return net_checksum_finish(net_checksum_partial(sum,data,length));
}
int net_ipv6_output(const struct net_ipv6_path *path,uint8_t protocol,const void *data,
                    uint32_t length,int hops,uint32_t flowinfo,bool df,bool multicast_loop)
{
    (void)hops; (void)flowinfo; (void)df; (void)multicast_loop;
    assert(protocol == 6 && length <= sizeof(queue[0].data));
    assert(!net_ipv6_checksum(&path->source,&path->destination,protocol,data,length));
    assert(tail - head < 32);
    struct queued *q = &queue[tail++ % 32]; q->path = *path; q->length = length;
    memcpy(q->data,data,length);
    return (int)length;
}
void net_poll_packets(void)
{
    net_tcp_service_sockets();
    while (head < tail) {
        struct queued q = queue[head++ % 32];
        net_tcp6_input(&q.path.source,&q.path.destination,q.data,q.length,q.path.ifindex);
        assert(head <= tail && tail - head < 32);
    }
}

static int open6(void)
{
    struct reliefos_net_socket_open request = {.domain=RELIEFOS_NET_AF_INET,.type=RELIEFOS_NET_SOCK_STREAM};
    assert(!net_socket_open(&request,1,0) && request.status == RELIEFOS_NET_STATUS_OK);
    assert(!net_socket_set_ipv6(request.socket,true));
    net_socket_pin_fd(request.socket);
    return request.socket;
}

int main(void)
{
    current.cap_effective = 1ULL << CAP_NET_BIND_SERVICE;
    int listener = open6(), client = open6();
    struct sockaddr_in6 addr = {.sin6_family=AF_INET6,.sin6_port=__builtin_bswap16(55010),.sin6_addr=loop6};
    assert(!net_socket_bind6_fd(listener,&addr));
    assert(!net_socket_listen_fd(listener,4));
    assert(net_socket_connect6_fd(client,&addr,true) == -LINUX_EINPROGRESS);
    net_poll_packets();
    /* A peer may send and half-close before its listener calls accept. */
    struct reliefos_net_socket_io io = {.socket=client,.buffer="hello",.length=5};
    assert(!net_socket_send(&io,0) && io.transferred == 5);
    net_poll_packets();
    assert(!net_socket_shutdown_fd(client,SHUT_WR));
    net_poll_packets();
    assert(net_socket_poll_fd(listener,0,POLLIN) & POLLIN);
    int child = net_socket_accept_peek(listener);
    assert(child > 0 && !net_socket_accept_commit(listener,child));
    struct sockaddr_in6 peer;
    assert(!net_socket_address6(child,true,&peer) && peer.sin6_family == AF_INET6 && !memcmp(&peer.sin6_addr,&loop6,16));
    char bytes[8] = {0}; io = (struct reliefos_net_socket_io){.socket=child,.buffer=bytes,.length=sizeof(bytes)};
    assert(!net_socket_recv(&io,0) && io.transferred == 5 && !memcmp(bytes,"hello",5));
    assert(net_socket_poll_fd(child,0,POLLIN) & POLLIN);
    int dual = open6(), mapped_client = open6();
    struct sockaddr_in6 any = {.sin6_family=AF_INET6,.sin6_port=__builtin_bswap16(55011)};
    assert(!net_socket_bind6_fd(dual,&any) && !net_socket_listen_fd(dual,4));
    struct sockaddr_in6 mapped = {.sin6_family=AF_INET6,.sin6_port=any.sin6_port,
        .sin6_addr={.s6_addr={0,0,0,0,0,0,0,0,0,0,255,255,127,0,0,1}}};
    assert(net_socket_connect6_fd(mapped_client,&mapped,true) == -LINUX_EINPROGRESS);
    net_poll_packets();
    int mapped_child = net_socket_accept_peek(dual);
    assert(mapped_child > 0 && !net_socket_accept_commit(dual,mapped_child));
    assert(!net_socket_address6(mapped_child,true,&peer));
    assert(peer.sin6_addr.s6_addr[10] == 255 && peer.sin6_addr.s6_addr[11] == 255 && peer.sin6_addr.s6_addr[12] == 127);
    io = (struct reliefos_net_socket_io){.socket=mapped_client,.buffer="mapped",.length=6};
    assert(!net_socket_send(&io,0) && io.transferred == 6);
    net_poll_packets();
    io = (struct reliefos_net_socket_io){.socket=mapped_child,.buffer=bytes,.length=sizeof(bytes)};
    assert(!net_socket_recv(&io,0) && io.transferred == 6 && !memcmp(bytes,"mapped",6));
    int only = open6();
    int only_value = 1;
    assert(!net_socket_ipv6_option(only,true,IPV6_V6ONLY,&only_value));
    any.sin6_port = __builtin_bswap16(55012);
    assert(!net_socket_bind6_fd(only,&any) && !net_socket_listen_fd(only,4));
    struct reliefos_net_socket_open v4request = {.domain=RELIEFOS_NET_AF_INET,.type=RELIEFOS_NET_SOCK_STREAM};
    assert(!net_socket_open(&v4request,1,0) && v4request.status == RELIEFOS_NET_STATUS_OK);
    net_socket_pin_fd(v4request.socket);
    assert(net_socket_connect_fd(v4request.socket,0x7f000001,55012,true) == -LINUX_EINPROGRESS);
    net_poll_packets();
    assert(net_socket_error(v4request.socket,true) == LINUX_ECONNREFUSED);
    while (net_sockets) { struct net_socket *s=net_sockets; net_sockets=s->registry_next; free(s->rx); free(s->tx); free(s); }
    puts("PASS IPv6 and IPv4-mapped TCP deferred loopback handshake, accept, payload and FIN");
}

bool net_interface_is_up(uint32_t index) { return net_interface_is_ready(index); }
