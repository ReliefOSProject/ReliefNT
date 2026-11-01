/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Real TCP receive probe distinguishes empty, EOF, data, and SO_ERROR.
 * The network and probe code are production-linked; NIC and swap are modeled.
 */
#include <string.h>
#define main inherited_task8_tcp_state_main
#include "tcp_state_test.c"
#undef main
#include <reliefnt/swap.h>

static uintptr_t blocked_byte;
static struct task_file tcp_file;
struct task_file *task_file_for_io(struct task *task,int fd)
{ assert(task==&current_task && fd==3);return &tcp_file; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    assert(task==&current_task && writing);
    return blocked_byte && address<=blocked_byte && blocked_byte-address<length ? SWAP_STORAGE_CONTENDED : 0;
}
int task_udp_recv_staged(struct task_file *file,void *data,uint32_t length,uint32_t flags,struct sockaddr_in *source)
{ (void)file;(void)data;(void)length;(void)flags;(void)source;abort(); }
int task_packet_recv_staged(struct task_file *file,void *data,uint32_t length,uint32_t flags,void *source,uint32_t *source_length)
{ (void)file;(void)data;(void)length;(void)flags;(void)source;(void)source_length;abort(); }

int main(void)
{
    current_task.kind=TASK_KIND_USER;current_task.pid=7;
    struct net_socket *socket=net_socket_alloc(7,1000);assert(socket);
    socket->state=RELIEFOS_NET_TCP_ESTABLISHED;
    struct task_file *file=&tcp_file;
    *file=(struct task_file){.flags=TASK_FILE_FLAG_SOCKET|TASK_FILE_FLAG_SOCKET_INET|RELIEFOS_O_NONBLOCK,
                             .aux=socket->handle};
    uint32_t count=99;char out[16]={0};blocked_byte=(uintptr_t)out;
    assert(inet_receive_probe(file,sizeof(out),0,&count)==-LINUX_EAGAIN);
    socket->rx_head=0;socket->rx_len=3;memcpy(socket->rx,"abc",3);blocked_byte=(uintptr_t)out+3;
    assert(inet_receive_probe(file,sizeof(out),0,&count)==3 && count==3 && socket->rx_len==3);
    assert(!socket_user_prepare(&current_task,(uintptr_t)out,count,true));
    assert(task_inet_recv(file,out,count,0)==3 && !memcmp(out,"abc",3) && !socket->rx_len);
    socket->fin_received=true;blocked_byte=(uintptr_t)out;
    assert(!inet_receive_probe(file,sizeof(out),0,&count) && !count);
    socket->fin_received=false;socket->rx_len=1;socket->rx[0]='x';socket->rx_head=0;
    socket->error=LINUX_ECONNRESET;
    assert(inet_receive_probe(file,sizeof(out),MSG_TRUNC,&count)==-LINUX_EOPNOTSUPP);
    assert(socket->error==LINUX_ECONNRESET && socket->rx_len==1);
    assert(inet_receive_probe(file,sizeof(out),0,&count)==-LINUX_ECONNRESET);
    assert(!socket->error && socket->rx_len==1);
    socket->rx_len=0;current_task.socket_receive_file=file;blocked_byte=(uintptr_t)out;
    assert(syscall_socket_dispatch(__NR_recvfrom,3,(uintptr_t)out,sizeof(out),0,0,0)==-LINUX_EAGAIN);
    socket->rx_len=3;socket->rx_head=0;memcpy(socket->rx,"xyz",3);
    blocked_byte=(uintptr_t)out+3;
    assert(syscall_socket_dispatch(__NR_recvfrom,3,(uintptr_t)out,sizeof(out),MSG_PEEK,0,0)==3);
    assert(!memcmp(out,"xyz",3) && socket->rx_len==3);
    blocked_byte=(uintptr_t)out;
    assert(syscall_socket_dispatch(__NR_recvfrom,3,(uintptr_t)out,sizeof(out),0,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(socket->rx_len==3);blocked_byte=0;
    assert(syscall_socket_dispatch(__NR_recvfrom,3,(uintptr_t)out,sizeof(out),0,0,0)==3);
    assert(!socket->rx_len && !memcmp(out,"xyz",3));
    socket->fin_received=true;blocked_byte=(uintptr_t)out;
    assert(!syscall_socket_dispatch(__NR_recvfrom,3,(uintptr_t)out,sizeof(out),0,0,0));
    current_task.socket_receive_file=NULL;
    net_socket_clear(socket);assert(!buffers);
    puts("PASS real TCP peek readiness, EOF, short prefix and error consumption");return 0;
}
