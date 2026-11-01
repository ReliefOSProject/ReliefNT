/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Probe actual PACKET queue readiness and zero-length datagrams.
 * Real socket/packet code is linked; swap and network hardware are modeled.
 */
#define TASK8_PACKET_READY_TEST 1
#include "task8_packet_user_test.c"
#include "../../kernel/syscall/ipc/socket.c"

int task_udp_recv_staged(struct task_file *file,void *data,uint32_t length,uint32_t flags,struct sockaddr_in *source)
{ (void)file;(void)data;(void)length;(void)flags;(void)source;abort(); }
int task_udp_recv(struct task_file *file,void *data,uint32_t length,uint32_t flags,uint64_t address,uint64_t address_length)
{ (void)file;(void)data;(void)length;(void)flags;(void)address;(void)address_length;abort(); }

int main(void)
{
    current=(struct task){.pid=93,.kind=TASK_KIND_USER,.cap_effective=1ULL<<CAP_NET_RAW};
    assert(syscall_packet(__NR_socket,AF_PACKET,SOCK_DGRAM|SOCK_NONBLOCK,8,0,0,0)==3);
    struct sockaddr_ll dest={.sll_family=AF_PACKET,.sll_ifindex=2,.sll_protocol=8,.sll_halen=6,
                             .sll_addr={255,255,255,255,255,255}};
    assert(!syscall_packet(__NR_bind,3,(uintptr_t)&dest,sizeof(dest),0,0,0));
    char out[32]={0};uint32_t actual=99;blocked=(uintptr_t)out;contended=true;
    current.socket_receive_file=&description;
    assert(inet_receive_probe(&description,sizeof(out),0,&actual)==-LINUX_EAGAIN);
    assert(syscall_socket_dispatch(__NR_recvfrom,3,(uintptr_t)out,sizeof(out),0,0,0)==-LINUX_EAGAIN);
    assert(!task_packet_send(&description,"",0,0,(uintptr_t)&dest,sizeof(dest)));
    net_packet_input(sent,sent_length);assert(task_packet_poll(&description,POLLIN)&POLLIN);
    assert(!inet_receive_probe(&description,sizeof(out),0,&actual) && !actual);
    assert(task_packet_poll(&description,POLLIN)&POLLIN);
    assert(!syscall_socket_dispatch(__NR_recvfrom,3,(uintptr_t)out,sizeof(out),MSG_PEEK|MSG_TRUNC,0,0));
    assert(!task_packet_recv(&description,out,0,MSG_PEEK|MSG_TRUNC,0,0));
    assert(task_packet_poll(&description,POLLIN)&POLLIN);
    assert(!task_packet_recv(&description,out,0,0,0,0));
    assert(!(task_packet_poll(&description,POLLIN)&POLLIN));
    assert(task_packet_send(&description,"hello",5,0,(uintptr_t)&dest,sizeof(dest))==5);
    net_packet_input(sent,sent_length);blocked=(uintptr_t)out+5;
    assert(inet_receive_probe(&description,sizeof(out),0,&actual)==5 && actual==5);
    assert(!socket_user_prepare(&current,(uintptr_t)out,actual,true));
    blocked=(uintptr_t)out+3;
    assert(syscall_socket_dispatch(__NR_recvfrom,3,(uintptr_t)out,3,MSG_PEEK|MSG_TRUNC,0,0)==5);
    assert(task_packet_available(&description)==5);
    blocked=(uintptr_t)out;
    assert(syscall_socket_dispatch(__NR_recvfrom,3,(uintptr_t)out,sizeof(out),0,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(task_packet_available(&description)==5);contended=false;
    assert(syscall_socket_dispatch(__NR_recvfrom,3,(uintptr_t)out,sizeof(out),0,0,0)==5 && !memcmp(out,"hello",5));
    current.socket_receive_file=NULL;
    task_packet_release(&description);
    puts("PASS real PACKET peek distinguishes empty queue, zero frame and short packet");return 0;
}
