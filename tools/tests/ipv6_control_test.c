/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if __has_include("../../net/socket/netlink.c")
#include "../../net/socket/netlink.c"
#include "../../net/interface/state.c"
#include "../../net/interface/routes.c"
#include "../../net/interface/ioctl.c"
#include "../../net/interface/config.c"
#include "../../net/proc.c"

static struct task current;
static struct task_file files[4];
static unsigned allocations, updates, link_changes;
static bool additional_address;
static bool nic_ready=true;
static bool nic_present=true;
struct net_socket *net_sockets;
struct task *sched_current_task(void) { return &current; }
void *kernel_malloc(size_t bytes) { ++allocations; return calloc(1, bytes); }
void kernel_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
uint64_t time_ticks(void) { return 100; }
uint64_t time_uptime_ms(void) { return 1000; }
void sched_sleep_current_until(uint64_t tick) { (void)tick; }
void kernel_poll_readiness_notify(void) {}
bool user_range_ok(uint64_t address, uint64_t size) { (void)size; return address > 4096; }
bool user_range_writable(uint64_t address, uint64_t size) { return user_range_ok(address, size); }
int task_allocate_fd(struct task *task, int minimum, struct task_file **out)
{
    (void)task;
    for (unsigned i = (unsigned)minimum; i < 4; ++i) if (!files[i].used) {
        files[i].used = 1; *out = &files[i]; return (int)i;
    }
    return -LINUX_EMFILE;
}
struct task_file *task_file_for_fd(struct task *task, int fd)
{ (void)task; return fd >= 0 && fd < 4 && files[fd].used ? &files[fd] : NULL; }
int e1000_is_ready(void) { return nic_ready; }
const uint8_t *e1000_mac(void) { static const uint8_t mac[6] = {2,3,4,5,6,7}; return mac; }
void e1000_get_info(struct e1000_info *info) { *info = (struct e1000_info){.present=nic_present,.active=nic_ready,.mac={2,3,4,5,6,7}}; }
uint32_t netdev_next(uint32_t after) { return nic_present && after<2 ? 2 : 0; }
int netdev_is_ready(uint32_t index) { return index==2 && nic_present && nic_ready; }
const uint8_t *netdev_mac(uint32_t index) { (void)index; return e1000_mac(); }
void netdev_get_info(uint32_t index,struct e1000_info *info)
{ if (index==2) e1000_get_info(info); else memset(info,0,sizeof(*info)); }
void netdev_counters(uint32_t index,struct net_proc_counters *out)
{ (void)index; memset(out,0,sizeof(*out)); }
void net_memcpy(void *dst,const void *src,uint32_t size) { memcpy(dst,src,size); }
void net_memzero(void *dst,uint32_t size) { memset(dst,0,size); }
uint32_t net_get_u32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
void net_put_u32(uint8_t *p,uint32_t value) { p[0]=value>>24;p[1]=value>>16;p[2]=value>>8;p[3]=value; }
void net_arp_cache_clear(void) {}
void net_arp_cache_clear_on(uint32_t index) { (void)index; }
void net_ipv6_link_changed(bool up) { (void)up; ++link_changes; }
int net_ipv6_link_changed_on(uint32_t index,bool up) { (void)index; (void)up; ++link_changes; return 0; }
void net_device_counters(struct net_proc_counters *out) { memset(out,0,sizeof(*out)); }
void net_loopback_proc_emit(struct text_stream *stream) { (void)stream; }
void net_arp_proc_emit(struct text_stream *stream) { (void)stream; }
void net_udp_proc_emit(struct text_stream *stream) { (void)stream; }
void unix_proc_emit(struct text_stream *stream) { (void)stream; }
void net_ipv6_transport_proc_emit(struct text_stream *stream,const char *leaf)
{ text_string(stream,leaf); text_string(stream,"\n"); }
uint32_t net_ipv6_addresses(struct net_ipv6_address *out, uint32_t capacity)
{
    struct net_ipv6_address a = {.address.s6_addr={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
        .ifindex=1,.prefix_length=128,.flags=NET_IPV6_ADDR_PERMANENT,.scope=RT_SCOPE_HOST,
        .preferred_until=UINT64_MAX,.valid_until=UINT64_MAX};
    if (capacity) *out = a;
    if (additional_address && capacity>1) {
        out[1]=(struct net_ipv6_address){.address.s6_addr={0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,2},
            .ifindex=2,.prefix_length=64,.flags=NET_IPV6_ADDR_TENTATIVE,.scope=RT_SCOPE_LINK,
            .preferred_until=UINT64_MAX,.valid_until=UINT64_MAX};
    }
    return 1+additional_address;
}
uint32_t net_ipv6_routes(struct net_ipv6_route *out, uint32_t capacity)
{
    struct net_ipv6_route r = {.destination.s6_addr={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
        .prefix_length=128,.ifindex=1,.metric=256,.valid_until=UINT64_MAX};
    if (capacity) *out = r;
    return 1;
}
int net_ipv6_address_update(const struct net_ipv6_address *address, bool remove)
{ (void)remove; assert(address->ifindex == 2 && address->prefix_length == 64); ++updates; return 0; }
int net_ipv6_route_update(const struct net_ipv6_route *route, bool remove)
{ (void)route; (void)remove; ++updates; return 0; }

static long call(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{ return syscall_netlink(nr,a,b,c,d,e,0); }
static int send_req(int fd, const void *message, unsigned length)
{ return task_netlink_send(&files[fd],message,length,0,0,0); }
static int receive(int fd, void *buffer, unsigned capacity)
{ return task_netlink_recv(&files[fd],buffer,capacity,MSG_DONTWAIT,0,0); }

static int receive_dump(int fd, void *buffer, unsigned capacity)
{
    int count=receive(fd,buffer,capacity);
    assert(count>0 && ((struct nlmsghdr *)buffer)->nlmsg_type!=NLMSG_DONE);
    unsigned char end[sizeof(struct nlmsghdr)+sizeof(int32_t)];
    assert(receive(fd,end,sizeof(end))==sizeof(end));
    assert(((struct nlmsghdr *)(void *)end)->nlmsg_type==NLMSG_DONE);
    return count;
}

/* A route reader may stop scanning a data datagram after finding a zero-metric
 * default route. Linux delivers the dump terminator in a later datagram, so a
 * subsequent recv must still observe completion (as fastfetch expects). */
static void check_route_completion(int fd)
{
    struct { struct nlmsghdr h; struct rtmsg m; struct rtattr table; uint32_t id; } request = {
        .h={.nlmsg_len=sizeof(request),.nlmsg_type=RTM_GETROUTE,
            .nlmsg_flags=NLM_F_REQUEST|NLM_F_DUMP,.nlmsg_seq=91},
        .m={.rtm_family=AF_INET},.table={.rta_len=8,.rta_type=RTA_TABLE},.id=RT_TABLE_MAIN};
    uint32_t gateway=net_config.gateway_ip;
    net_config.gateway_ip=0x0a000202u;
    assert(send_req(fd,&request,sizeof(request))==sizeof(request));
    unsigned char output[8192];
    int count=receive(fd,output,sizeof(output));
    assert(count>0);
    bool found=false;
    for (unsigned pos=0;pos+sizeof(struct nlmsghdr)<=(unsigned)count;) {
        struct nlmsghdr *h=(void *)(output+pos);
        assert(h->nlmsg_len>=sizeof(*h) && h->nlmsg_len<=(unsigned)count-pos);
        if (h->nlmsg_type==RTM_NEWROUTE && !((struct rtmsg *)(h+1))->rtm_dst_len) {
            found=true;
            break;
        }
        pos+=NLMSG_ALIGN(h->nlmsg_len);
    }
    assert(found);
    assert(task_netlink_poll(&files[fd],POLLIN)&POLLIN);
    assert(task_netlink_recv(&files[fd],output,1,MSG_DONTWAIT|MSG_PEEK|MSG_TRUNC,0,0)==20);
    count=receive(fd,output,sizeof(output));
    assert(count==(int)(sizeof(struct nlmsghdr)+sizeof(int32_t)));
    struct nlmsghdr *done=(void *)output;
    assert(done->nlmsg_type==NLMSG_DONE && done->nlmsg_seq==91 &&
           done->nlmsg_pid==nl_socket(&files[fd])->port && (done->nlmsg_flags&NLM_F_MULTI));
    assert(*(int32_t *)(done+1)==0);
    assert(receive(fd,output,sizeof(output))==-LINUX_EAGAIN);
    assert(!(task_netlink_poll(&files[fd],POLLIN)&POLLIN));
    net_config.gateway_ip=gateway;
}

static void check_dump_boundaries(int fd)
{
    struct { struct nlmsghdr h; struct ifaddrmsg m; } request = {
        .h={.nlmsg_len=sizeof(request),.nlmsg_type=RTM_GETADDR,
            .nlmsg_flags=NLM_F_REQUEST|NLM_F_DUMP,.nlmsg_seq=92},
        .m={.ifa_family=AF_INET,.ifa_index=77}};
    unsigned char output[8192];
    /* An empty dump still completes; an invalid request only returns an error. */
    assert(send_req(fd,&request,sizeof(request))==sizeof(request));
    assert(receive(fd,output,sizeof(output))==20);
    assert(((struct nlmsghdr *)output)->nlmsg_type==NLMSG_DONE);
    assert(receive(fd,output,sizeof(output))==-LINUX_EAGAIN);
    request.m.ifa_family=255;
    assert(send_req(fd,&request,sizeof(request))==sizeof(request));
    assert(receive(fd,output,sizeof(output))>0);
    assert(((struct nlmsghdr *)output)->nlmsg_type==NLMSG_ERROR);
    assert(receive(fd,output,sizeof(output))==-LINUX_EAGAIN);
    request.m.ifa_family=AF_INET; request.m.ifa_index=0;
    unsigned accepted=0;
    int rc;
    while ((rc=send_req(fd,&request,sizeof(request)))==(int)sizeof(request))
        assert(++accepted<=NL_QUEUE_BUDGET/NL_MESSAGE_MAX);
    assert(rc==-LINUX_ENOBUFS && accepted);
    for (unsigned i=0;i<accepted;++i) assert(receive_dump(fd,output,sizeof(output))>0);
    assert(receive(fd,output,sizeof(output))==-LINUX_EAGAIN);
    assert(nl_socket(&files[fd])->queued_bytes==0);
}

int main(void)
{
    assert(!net_interface_register(2));
    net_config.local_ip=0x0a00020f; net_config.subnet_mask=0xffffff00;
    int fd = (int)call(__NR_socket,AF_NETLINK,SOCK_RAW|SOCK_NONBLOCK,NETLINK_ROUTE,0,0);
    assert(fd >= 0 && files[fd].socket_ino);
    check_route_completion(fd);
    check_dump_boundaries(fd);
    struct { struct nlmsghdr h; struct ifinfomsg m; } link = {
        .h={.nlmsg_len=sizeof(link),.nlmsg_type=RTM_GETLINK,.nlmsg_flags=NLM_F_REQUEST|NLM_F_DUMP,.nlmsg_seq=42},
        .m={.ifi_family=AF_UNSPEC}};
    assert(send_req(fd,&link,sizeof(link)) == sizeof(link));
    unsigned char output[8192]; int count, found_lo=0, found_eth=0, done=0;
    while ((count=receive(fd,output,sizeof(output))) > 0) {
        for (unsigned pos=0; pos+sizeof(struct nlmsghdr)<=(unsigned)count;) {
            struct nlmsghdr *h=(void *)(output+pos);
            assert(h->nlmsg_len>=sizeof(*h) && h->nlmsg_len<=(unsigned)count-pos && h->nlmsg_seq==42);
            if (h->nlmsg_type == RTM_NEWLINK) {
                struct ifinfomsg *m=(void *)(h+1);
                found_lo |= m->ifi_index==1; found_eth |= m->ifi_index==2;
            }
            done |= h->nlmsg_type==NLMSG_DONE;
            pos += NLMSG_ALIGN(h->nlmsg_len);
        }
    }
    assert(count == -LINUX_EAGAIN && found_lo && found_eth && done);
    /* A present Ethernet device remains enumerable while PHY negotiates. */
    nic_ready=false;
    assert(send_req(fd,&link,sizeof(link))==sizeof(link));
    count=receive_dump(fd,output,sizeof(output));
    assert(count>0);
    found_eth=0;
    for (unsigned pos=0;pos+sizeof(struct nlmsghdr)<=(unsigned)count;) {
        struct nlmsghdr *h=(void *)(output+pos);
        if (h->nlmsg_type==RTM_NEWLINK && ((struct ifinfomsg *)(h+1))->ifi_index==2) {
            struct ifinfomsg *m=(void *)(h+1);
            assert((m->ifi_flags&IFF_UP) && !(m->ifi_flags&(IFF_RUNNING|0x10000u)));
            found_eth=1;
        }
        pos+=NLMSG_ALIGN(h->nlmsg_len);
    }
    assert(found_eth);
    nic_ready=true;
    /* musl and BusyBox issue rtgenmsg dumps, only one payload byte plus padding. */
    struct { struct nlmsghdr h; uint8_t family, pad[3]; } generic = {
        .h={.nlmsg_len=sizeof(generic),.nlmsg_type=RTM_GETLINK,
            .nlmsg_flags=NLM_F_REQUEST|NLM_F_DUMP,.nlmsg_seq=71},.family=AF_UNSPEC};
    assert(send_req(fd,&generic,sizeof(generic)) == sizeof(generic));
    count=receive_dump(fd,output,sizeof(output));
    assert(count>0 && ((struct nlmsghdr *)output)->nlmsg_type==RTM_NEWLINK);
    generic.family=AF_INET6;
    assert(send_req(fd,&generic,sizeof(generic)) == sizeof(generic));
    count=receive_dump(fd,output,sizeof(output));
    assert(count>0 && ((struct nlmsghdr *)output)->nlmsg_type==RTM_NEWLINK);
    generic.family=AF_INET;
    assert(send_req(fd,&generic,sizeof(generic)) == sizeof(generic));
    count=receive_dump(fd,output,sizeof(output));
    assert(count>0 && ((struct nlmsghdr *)output)->nlmsg_type==RTM_NEWLINK);
    generic.family=AF_UNSPEC;
    generic.h.nlmsg_type=RTM_GETADDR; generic.h.nlmsg_seq=72;
    assert(send_req(fd,&generic,sizeof(generic)) == sizeof(generic));
    count=receive_dump(fd,output,sizeof(output));
    assert(count>0 && ((struct nlmsghdr *)output)->nlmsg_type==RTM_NEWADDR);
    generic.h.nlmsg_type=RTM_GETROUTE; generic.h.nlmsg_seq=73; generic.family=AF_INET6;
    assert(send_req(fd,&generic,sizeof(generic)) == sizeof(generic));
    count=receive_dump(fd,output,sizeof(output));
    assert(count>0 && ((struct nlmsghdr *)output)->nlmsg_type==RTM_NEWROUTE);
    struct { struct nlmsghdr h; struct ifaddrmsg m; } addr = {
        .h={.nlmsg_len=sizeof(addr),.nlmsg_type=RTM_GETADDR,.nlmsg_flags=NLM_F_REQUEST|NLM_F_DUMP,.nlmsg_seq=43},
        .m={.ifa_family=AF_INET6}};
    assert(send_req(fd,&addr,sizeof(addr)) == sizeof(addr));
    count=receive_dump(fd,output,sizeof(output));
    assert(count>0 && ((struct nlmsghdr *)output)->nlmsg_type==RTM_NEWADDR);
    while (receive(fd,output,sizeof(output))>0) {}
    struct { struct nlmsghdr h; struct ifaddrmsg m; struct rtattr a; unsigned char ip[16]; } change = {
        .h={.nlmsg_len=sizeof(change),.nlmsg_type=RTM_NEWADDR,.nlmsg_flags=NLM_F_REQUEST|NLM_F_ACK,.nlmsg_seq=44},
        .m={.ifa_family=AF_INET6,.ifa_prefixlen=64,.ifa_index=2},
        .a={.rta_len=20,.rta_type=IFA_LOCAL},.ip={0x20,0x01,0x0d,0xb8}};
    assert(send_req(fd,&change,sizeof(change)) == sizeof(change));
    count=receive(fd,output,sizeof(output));
    assert(count>= (int)(sizeof(struct nlmsghdr)+sizeof(struct nlmsgerr)) &&
           ((struct nlmsghdr *)output)->nlmsg_type==NLMSG_ERROR &&
           ((struct nlmsgerr *)(output+sizeof(struct nlmsghdr)))->error==-LINUX_EPERM && updates==0);
    current.cap_effective=1ULL<<CAP_NET_ADMIN;
    change.h.nlmsg_flags |= NLM_F_CREATE|NLM_F_EXCL;
    assert(send_req(fd,&change,sizeof(change)) == sizeof(change));
    count=receive(fd,output,sizeof(output));
    assert(count>0 && ((struct nlmsgerr *)(output+sizeof(struct nlmsghdr)))->error==0 && updates==1);
    change.a.rta_len=2;
    assert(send_req(fd,&change,sizeof(change)) == sizeof(change));
    count=receive(fd,output,sizeof(output));
    assert(count>0 && ((struct nlmsgerr *)(output+sizeof(struct nlmsghdr)))->error==-LINUX_EINVAL && updates==1);
    struct { struct nlmsghdr h; struct rtmsg m; struct rtattr dst; uint8_t ip[16];
             struct rtattr oif; uint32_t index; } route={
        .h={.nlmsg_len=sizeof(route),.nlmsg_type=RTM_NEWROUTE,
            .nlmsg_flags=NLM_F_REQUEST|NLM_F_ACK|NLM_F_CREATE|NLM_F_EXCL,.nlmsg_seq=45},
        .m={.rtm_family=AF_INET6,.rtm_dst_len=64,.rtm_table=RT_TABLE_MAIN,.rtm_type=RTN_UNICAST},
        .dst={.rta_len=20,.rta_type=RTA_DST},.ip={0x20,1,0x0d,0xb8},
        .oif={.rta_len=8,.rta_type=RTA_OIF},.index=2};
    assert(send_req(fd,&route,sizeof(route))==sizeof(route));
    count=receive(fd,output,sizeof(output));
    assert(count>0 && ((struct nlmsgerr *)(output+sizeof(struct nlmsghdr)))->error==0 && updates==2);
    int listener=(int)call(__NR_socket,AF_NETLINK,SOCK_RAW|SOCK_NONBLOCK,NETLINK_ROUTE,0,0);
    assert(listener>=0 && files[listener].socket_ino!=files[fd].socket_ino);
    struct sockaddr_nl bind_address={.nl_family=AF_NETLINK,.nl_groups=RTMGRP_IPV6_IFADDR};
    assert(call(__NR_bind,listener,(uintptr_t)&bind_address,sizeof(bind_address),0,0)==0);
    additional_address=true; netlink_ipv6_changed();
    struct sockaddr_nl source={0}; uint32_t source_size=sizeof(source);
    count=task_netlink_recv(&files[listener],output,sizeof(output),0,(uintptr_t)&source,(uintptr_t)&source_size);
    assert(count>0 && ((struct nlmsghdr *)output)->nlmsg_type==RTM_NEWADDR &&
           ((struct nlmsghdr *)output)->nlmsg_seq==0 && source.nl_pid==0 &&
           source.nl_groups==RTMGRP_IPV6_IFADDR && source_size==sizeof(source));
    additional_address=false; netlink_ipv6_changed();
    count=receive(listener,output,sizeof(output));
    assert(count>0 && ((struct nlmsghdr *)output)->nlmsg_type==RTM_DELADDR);
    for (unsigned i=0;i<10;++i) { additional_address=!additional_address; netlink_ipv6_changed(); }
    assert(task_netlink_poll(&files[listener],POLLIN)&POLLERR);
    assert(receive(listener,output,sizeof(output))==-LINUX_ENOBUFS);
    while (receive(listener,output,sizeof(output))>0) {}
    task_netlink_release(&files[listener]);
    struct in6_ifreq in6={.ifr6_addr.s6_addr={0x20,1,0x0d,0xb8},.ifr6_prefixlen=64,.ifr6_ifindex=2};
    current.cap_effective=0;
    assert(net_ipv6_interface_ioctl(SIOCSIFADDR,(uintptr_t)&in6)==-LINUX_EPERM && updates==2);
    current.cap_effective=1ULL<<CAP_NET_ADMIN;
    assert(net_ipv6_interface_ioctl(SIOCSIFADDR,(uintptr_t)&in6)==0 && updates==3);
    struct in6_rtmsg in6route={.rtmsg_dst.s6_addr={0x20,1,0x0d,0xb8},.rtmsg_dst_len=64,
                               .rtmsg_flags=RTF_UP,.rtmsg_ifindex=2};
    assert(net_ipv6_interface_ioctl(SIOCADDRT,(uintptr_t)&in6route)==0 && updates==4);
    struct ifreq ifr={.ifr_name="lo"};
    assert(net_interface_ioctl(SIOCGIFINDEX,(uintptr_t)&ifr)==0 && ifr.data.value==1);
    memset(&ifr,0,sizeof(ifr)); ifr.data.value=1;
    assert(net_interface_ioctl(SIOCGIFNAME,(uintptr_t)&ifr)==0 && !strcmp(ifr.ifr_name,"lo"));
    nic_ready=nic_present=false;
    net_interface_remove(2);
    strcpy(ifr.ifr_name,"lo");
    assert(net_interface_ioctl(SIOCGIFINDEX,(uintptr_t)&ifr)==0 && ifr.data.value==1);
    memset(&ifr,0,sizeof(ifr)); ifr.data.value=1;
    assert(net_interface_ioctl(SIOCGIFNAME,(uintptr_t)&ifr)==0 && !strcmp(ifr.ifr_name,"lo"));
    strcpy(ifr.ifr_name,"eth0");
    assert(net_interface_ioctl(SIOCGIFINDEX,(uintptr_t)&ifr)==-LINUX_ENODEV);
    nic_ready=nic_present=true;
    assert(!net_interface_register(2));
    memset(&ifr,0,sizeof(ifr)); ifr.data.value=2;
    assert(net_interface_ioctl(SIOCGIFNAME,(uintptr_t)&ifr)==0 && !strcmp(ifr.ifr_name,"eth0"));
    strcpy(ifr.ifr_name,"eth0");
    ifr.data.flags=0;
    assert(net_interface_ioctl(SIOCSIFFLAGS,(uintptr_t)&ifr)==0 && link_changes==1);
    assert(net_interface_ioctl(SIOCSIFFLAGS,(uintptr_t)&ifr)==0 && link_changes==1);
    char text[2048]={0}; struct text_stream stream={.buffer=text,.capacity=sizeof(text)-1};
    assert(net_proc_emit(&stream,"if_inet6")==0);
    assert(strstr(text,"00000000000000000000000000000001 01 80 10 80 lo\n"));
    memset(text,0,sizeof(text)); stream=(struct text_stream){.buffer=text,.capacity=sizeof(text)-1};
    assert(net_proc_emit(&stream,"ipv6_route")==0 && strstr(text," 80 00000000000000000000000000000000 00 "));
    memset(text,0,sizeof(text)); stream=(struct text_stream){.buffer=text,.capacity=sizeof(text)-1};
    assert(net_proc_emit(&stream,"tcp6")==0 && !strcmp(text,"tcp6\n"));
    task_netlink_release(&files[fd]);
    net_interface_remove(2);
    assert(!allocations);
    puts("PASS route-netlink dumps/updates, musl requests, multicast/overflow, IPv6 ioctl and proc");
    return 0;
}
#else
int main(void) { fputs("IPv6 route-netlink control is missing\n", stderr); return 1; }
#endif
