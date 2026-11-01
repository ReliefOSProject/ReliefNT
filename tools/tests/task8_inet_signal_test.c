/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: inet signal.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define TASK8_REAL_LOOKUP_SCOPE 1
#define TASK8_REAL_INET_SIGNAL 1
#define main inherited_signal_main
#include "signal_address_space_test.c"
#undef main
#include <reliefnt/net_udp.h>
#include <linux/capability.h>

static struct task_file descriptors[8];
struct socket_send_stage *sched_socket_stage(const struct task *task)
{ assert(task==&target);return NULL; }
bool task_sysv_sem_ready(const struct task *task) { assert(task==&target);return false; }
void syscall_record_lock_cancel(struct task *task) { assert(task==&target); }
struct task_file *task_file_for_fd(struct task *task,int fd)
{ assert(task==&target);return fd>=0 && fd<8 && descriptors[fd].used?&descriptors[fd]:NULL; }
struct task_file *task_file_for_io(struct task *task,int fd)
{ return task_file_for_fd(task,fd); }
int task_allocate_fd(struct task *task,int minimum,struct task_file **out)
{ assert(task==&target);for(int i=minimum;i<8;++i)if(!descriptors[i].used)
  {descriptors[i].used=1;*out=&descriptors[i];return i;}return -LINUX_EMFILE; }
int net_get_config(struct reliefos_net_config *config)
{ *config=(struct reliefos_net_config){.local_ip=0x0a00020f};return 0; }
void net_poll_packets(void) {}
int net_ipv4_send_udp(uint32_t s,uint32_t d,uint16_t sp,uint16_t dp,const void *b,uint32_t n)
{ (void)s;(void)d;(void)sp;(void)dp;(void)b;(void)n;return -LINUX_ENETDOWN; }
void sched_sleep_current_until(uint64_t tick) { (void)tick;assert(0); }

static int64_t scoped_recv(int fd,uint64_t header)
{
    struct trap_frame f={.rax=__NR_recvmsg,.rdi=(uint64_t)fd,.rsi=header};
    task_lookup_scope_begin(&target,&f);
    return syscall_socket_dispatch(__NR_recvmsg,fd,header,0,0,0,0);
}

int main(void)
{
    unsigned char *user=mmap((void *)STACK_ADDRESS,sizeof(target_pages),PROT_READ|PROT_WRITE,
                             MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(user==(void *)STACK_ADDRESS);
    target=(struct task){.kind=TASK_KIND_USER,.pid=1,.state=TASK_READY,
                         .cap_effective=1ULL<<CAP_NET_BIND_SERVICE};
    target.limits.sigpending.rlim_cur=8;
    target.signal_actions[2]=(struct kernel_signal_action){.handler=0x410000,
        .restorer=0x420000,.flags=LINUX_SA_SIGINFO};
    int a=(int)syscall_udp(__NR_socket,AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0,0,0,0);
    int b=(int)syscall_udp(__NR_socket,AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0,0,0,0);
    assert(a>=0 && b>=0);
    struct sockaddr_in *address=(void *)(user+64);
    *address=(struct sockaddr_in){.sin_family=AF_INET,.sin_port=__builtin_bswap16(55000),
                                  .sin_addr={.s_addr=__builtin_bswap32(0x7f000001)}};
    assert(!syscall_udp(__NR_bind,b,(uintptr_t)address,sizeof(*address),0,0,0));
    assert(!syscall_udp(__NR_connect,a,(uintptr_t)address,sizeof(*address),0,0,0));
    struct iovec *vector=(void *)(user+128);struct msghdr *msg=(void *)(user+192);
    struct sockaddr_in *peer=(void *)(user+400);char *bytes=(void *)(user+512);
    *vector=(struct iovec){.iov_base=bytes,.iov_len=8};
    *msg=(struct msghdr){.msg_name=peer,.msg_namelen=sizeof(*peer),.msg_iov=vector,.msg_iovlen=1};
    assert(task_udp_send(&descriptors[a],"hello",5,0,0,0)==5);
    signal_inet_blocked=(uintptr_t)peer;
    assert(scoped_recv(b,(uintptr_t)msg)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(task_udp_available(&descriptors[b])==5);
    target.restart_syscall=__NR_recvmsg+1;
    assert(!kernel_signal_queue_task(&target,2));
    struct trap_frame frame={.rsp=STACK_ADDRESS+4896,.rip=0x430000,.cs=0x23};
    signal_inet_blocked=0;
    assert(kernel_signal_deliver_pending_outer(&target,&frame)==1);
    assert(!target.lookup_scope.armed && task_udp_available(&descriptors[b])==5);
    assert(scoped_recv(b,(uintptr_t)msg)==5 && !memcmp(bytes,"hello",5));
    task_lookup_scope_cancel(&target);
    assert(task_udp_send(&descriptors[a],"fatal",5,0,0,0)==5);
    signal_inet_blocked=(uintptr_t)peer;
    assert(scoped_recv(b,(uintptr_t)msg)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!kernel_signal_enqueue(&target,false,11,NULL));
    assert(!kernel_signal_deliver_pending_outer(&target,&frame));
    assert(target.state==TASK_EXITED && exits==1 && task_udp_available(&descriptors[b])==5);
    task_lookup_scope_cancel(&target);signal_inet_blocked=0;
    assert(task_udp_recv(&descriptors[b],bytes,8,0,0,0)==5 && !memcmp(bytes,"fatal",5));
    task_udp_release(&descriptors[a]);task_udp_release(&descriptors[b]);
    munmap(user,sizeof(target_pages));
    puts("PASS INET+signal: caught cancellation and fatal leave unaccepted datagrams queued");
    return 0;
}
