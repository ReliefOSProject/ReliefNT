/* Exercise real INET vector dispatch and UDP queues with swap contention. */
#define TASK8_INET_READY_TEST 1
#define IPV6_TRANSPORT_FIXTURE 1
#include "task8_inet_message_user_test.c"

static const struct in6_addr loop6={.s6_addr={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}};
void kernel_poll_readiness_notify(void) {}
uint32_t net_ipv6_addresses(struct net_ipv6_address *out,uint32_t capacity)
{ if(capacity)out[0]=(struct net_ipv6_address){.address=loop6,.ifindex=1};return 1; }
int net_ipv6_route(const struct in6_addr *destination,uint32_t scope,
                   const struct in6_addr *bound,struct net_ipv6_path *path)
{
    if(memcmp(destination,&loop6,16) || (scope && scope!=1))return -LINUX_ENETUNREACH;
    *path=(struct net_ipv6_path){.source=udp6_any(bound) ? loop6 : *bound,
        .destination=*destination,.ifindex=1,.mtu=65536};return 0;
}
uint16_t net_ipv6_checksum(const struct in6_addr *source,const struct in6_addr *destination,
                           uint8_t protocol,const void *data,uint32_t length)
{
    uint32_t sum=0;const uint8_t *parts[]={source->s6_addr,destination->s6_addr,data};
    uint32_t lengths[]={16,16,length};
    for(unsigned p=0;p<3;++p)for(uint32_t i=0;i<lengths[p];i+=2)
        sum+=(uint32_t)parts[p][i]*256+(i+1<lengths[p] ? parts[p][i+1] : 0);
    sum+=(length>>16)+(length&0xffff)+protocol;
    while(sum>>16)sum=(sum&0xffff)+(sum>>16);
    return (uint16_t)~sum;
}
int net_ipv6_output(const struct net_ipv6_path *path,uint8_t protocol,const void *data,
                    uint32_t length,int hops,uint32_t flowinfo,bool df,bool multicast_loop)
{
    (void)flowinfo;(void)df;(void)multicast_loop;const uint8_t *p=data;
    assert(protocol==17 && length>=8 && !net_ipv6_checksum(&path->source,&path->destination,protocol,data,length));
    assert(net_udp6_input(&path->source,&path->destination,(p[0]<<8)|p[1],(p[2]<<8)|p[3],
                          p+8,length-8,path->ifindex,hops<0 ? 64 : (uint8_t)hops));
    return (int)length;
}
int net_ipv6_multicast_change(const struct in6_addr *address,uint32_t ifindex,int delta)
{ (void)address;(void)ifindex;(void)delta;return 0; }

int file_can_read(const struct task_file *file) { return file != NULL; }
int file_can_write(const struct task_file *file) { return file != NULL; }

/* The scalar fallback is only reached by the old, incorrect vector path. */
int64_t syscall_io_write(uint64_t fd, uint64_t address, uint64_t length)
{
    int prep=swap_prepare_user_range(&current,address,length,false);
    if(prep==SWAP_STORAGE_CONTENDED)return KERNEL_SYSCALL_SWAP_CONTENDED;
    if(prep<0)return prep;
    return task_udp_send(&files[fd],(const void *)(uintptr_t)address,length,0,0,0);
}
int64_t syscall_io_read(uint64_t fd, uint64_t address, uint64_t length)
{
    int prep=swap_prepare_user_range(&current,address,length,true);
    if(prep==SWAP_STORAGE_CONTENDED)return KERNEL_SYSCALL_SWAP_CONTENDED;
    if(prep<0)return prep;
    return task_udp_recv(&files[fd],(void *)(uintptr_t)address,length,0,0,0);
}
int64_t syscall_vector_io(struct task *,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static int64_t vector_call(uint64_t number,int fd,struct iovec *iov,uint64_t count)
{
    struct trap_frame frame={.rax=number,.rdi=(uint64_t)fd,.rsi=(uintptr_t)iov,.rdx=count};
    task_lookup_scope_begin(&current,&frame);
    return syscall_vector_io(&current,number,fd,(uintptr_t)iov,count,0,0,0);
}
int main(void)
{
    current=(struct task){.pid=92,.kind=TASK_KIND_USER,.cap_effective=1ULL<<CAP_NET_BIND_SERVICE};
    int sender=open_socket(),receiver=open_socket();
    struct sockaddr_in dest={.sin_family=AF_INET,.sin_port=swap16(55000),
                             .sin_addr={.s_addr=swap32(0x7f000001)}};
    assert(sender>=0 && receiver>=0);
    assert(!call(__NR_bind,receiver,(uintptr_t)&dest,sizeof(dest),0,0));
    assert(!call(__NR_connect,sender,(uintptr_t)&dest,sizeof(dest),0,0));
    char first[4]="abc",second[4]="def",out[8]={0};
    struct iovec send[2]={{first,3},{second,3}};
    blocked=(uintptr_t)second;contended=true;
    assert(vector_call(LINUX_SYS_WRITEV,sender,send,2)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(task_udp_available(&files[receiver])==0);
    first[0]='X';contended=false;
    assert(vector_call(LINUX_SYS_WRITEV,sender,send,2)==6);
    task_lookup_scope_cancel(&current);
    assert(task_udp_recv(&files[receiver],out,sizeof(out),0,0,0)==6 && !memcmp(out,"abcdef",6));
    assert(task_udp_send(&files[sender],"ghijkl",6,0,0,0)==6);
    char receive_a[3]={0},receive_b[3]={0};
    struct iovec receive[2]={{receive_a,3},{receive_b,3}};
    blocked=(uintptr_t)receive_b;contended=true;
    assert(vector_call(LINUX_SYS_READV,receiver,receive,2)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(task_udp_available(&files[receiver])==6 && !receive_a[0]);
    contended=false;unmapped_output=(uintptr_t)receive_b;
    assert(vector_call(LINUX_SYS_READV,receiver,receive,2)==-LINUX_EFAULT);
    assert(task_udp_available(&files[receiver])==6 && !receive_a[0]);
    unmapped_output=0;
    assert(vector_call(LINUX_SYS_READV,receiver,receive,2)==6);
    task_lookup_scope_cancel(&current);
    assert(!memcmp(receive_a,"ghi",3) && !memcmp(receive_b,"jkl",3));
    assert(task_udp_available(&files[receiver])==0);
    task_udp_release(&files[sender]);task_udp_release(&files[receiver]);
    files[sender].used=files[receiver].used=0;
    sender=(int)call(__NR_socket,AF_INET6,SOCK_DGRAM|SOCK_NONBLOCK,0,0,0);
    receiver=(int)call(__NR_socket,AF_INET6,SOCK_DGRAM|SOCK_NONBLOCK,0,0,0);
    struct sockaddr_in6 dest6={.sin6_family=AF_INET6,.sin6_port=swap16(55001),.sin6_addr=loop6};
    assert(sender>=0 && receiver>=0);
    assert(!call(__NR_bind,receiver,(uintptr_t)&dest6,sizeof(dest6),0,0));
    assert(!call(__NR_connect,sender,(uintptr_t)&dest6,sizeof(dest6),0,0));
    unsigned char large[8192],returned[8192];
    for(unsigned i=0;i<sizeof(large);++i)large[i]=(unsigned char)(i*19u+3u);
    struct iovec large_send[2]={{large,3072},{large+3072,5120}};
    struct iovec large_receive[2]={{returned,4100},{returned+4100,4092}};
    assert(vector_call(LINUX_SYS_WRITEV,sender,large_send,2)==sizeof(large));
    task_lookup_scope_cancel(&current);
    assert(vector_call(LINUX_SYS_READV,receiver,large_receive,2)==sizeof(returned));
    task_lookup_scope_cancel(&current);
    assert(!memcmp(large,returned,sizeof(large)));
    task_udp_release(&files[sender]);task_udp_release(&files[receiver]);
    assert(!allocations);
    puts("PASS INET datagram readv/writev atomicity and retry-safe user vectors");
    return 0;
}
