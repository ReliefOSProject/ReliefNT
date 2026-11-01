/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: socket send.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_socket_batch_main
#include "socket_batch_unix_test.c"
#undef main
#include <reliefnt/swap.h>
#include "task8_socket_output_stubs.h"
static uint8_t packet[65536],received_packet[65536];
static uint64_t blocked;
static bool contended;
static unsigned first_prepares,total_prepares;
struct socket_send_stage;
static struct socket_send_stage *owned_stage;
struct socket_send_stage *sched_socket_stage(const struct task *task)
{ assert(task==&current);return owned_stage; }
bool sched_socket_stage_set(struct task *task,struct socket_send_stage *stage)
{ assert(task==&current && !owned_stage);owned_stage=stage;return true; }
struct socket_send_stage *sched_socket_stage_take(struct task *task)
{ assert(task==&current);struct socket_send_stage *s=owned_stage;owned_stage=NULL;return s; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    assert(task==&current && length<=32768);
    ++total_prepares;
    if(!writing && address==(uintptr_t)packet)++first_prepares;
    if(!writing && address==blocked && contended)return SWAP_STORAGE_CONTENDED;
#ifdef TASK8_CLEANUP_TEST
    if(writing && address==blocked && contended)return SWAP_STORAGE_CONTENDED;
#endif
    return 0;
}
#ifndef TASK8_SOCKET_NO_MAIN
int main(void)
{
    current.kind=TASK_KIND_USER;current.pid=current.tgid=10;current.uid=current.gid=100;current.state=TASK_READY;
    for(unsigned i=0;i<2;++i){peers[i]=(struct unix_socket){.used=1,.refs=1,.handle=i+1,.type=SOCK_DGRAM,
        .state=UNIX_SOCKET_CONNECTED,.peer_handle=2-i,.receive_timeout=UINT64_MAX,.send_timeout=UINT64_MAX};
        endpoints[i]=(struct task_file){.used=1,.references=1,.aux=i+1,
            .flags=TASK_FILE_FLAG_SOCKET|TASK_FILE_FLAG_SOCKET_UNIX|RELIEFOS_O_RDWR};}
    passed=(struct task_file){.used=1,.references=1};passed_replacement=(struct task_file){.used=1,.references=1};
    /* sendto must reject the whole declared numerical range before either
     * protocol truncation, a page-in attempt, or packet publication. */
    current.syscall_file=&endpoints[0];
    uint64_t edge=RELIEFNT_USER_TOP-16384u;
    struct trap_frame edge_frame={.rax=LINUX_SYS_SENDTO,.rdi=3,.rsi=edge,.rdx=16385};
    peers[0].type=peers[1].type=SOCK_STREAM;
    blocked=edge;contended=true;task_lookup_scope_begin(&current,&edge_frame);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,edge,16385,0,0,0)==-LINUX_EFAULT);
    assert(!peers[1].rx_written && !owned_stage);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,edge,16385,MSG_OOB,0,0)==-LINUX_EFAULT);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,UINT64_MAX-2,8,MSG_OOB,0,0)==-LINUX_EFAULT);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,(uintptr_t)packet,1,MSG_OOB,0,0)==-LINUX_EOPNOTSUPP);
    assert(!total_prepares && !peers[1].rx_written && !peers[1].packet_count && !owned_stage);
    task_lookup_scope_cancel(&current);
    peers[0].type=peers[1].type=SOCK_DGRAM;
    edge_frame.rdx=65537;task_lookup_scope_begin(&current,&edge_frame);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,edge,65537,0,0,0)==-LINUX_EFAULT);
    assert(!peers[1].packet_count && !owned_stage);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,UINT64_MAX-2,8,0,0,0)==-LINUX_EFAULT);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,edge,65537,MSG_OOB,0,0)==-LINUX_EFAULT);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,UINT64_MAX-2,8,MSG_OOB,0,0)==-LINUX_EFAULT);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,(uintptr_t)packet,65537,0,0,0)==-LINUX_EMSGSIZE);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,(uintptr_t)packet,1,MSG_OOB,0,0)==-LINUX_EOPNOTSUPP);
    assert(!total_prepares && !peers[1].packet_count && !owned_stage);
    current.syscall_file=NULL;
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,99,edge,65537,MSG_OOB,0,0)==-LINUX_EBADF);
    current.syscall_file=&endpoints[0];
    task_lookup_scope_cancel(&current);contended=false;
    for(size_t i=0;i<sizeof(packet);++i)packet[i]=(uint8_t)(i*13u);
    struct iovec vector={packet,sizeof(packet)};
    unsigned char control[CMSG_SPACE(sizeof(int))] __attribute__((aligned(8)))={0};
    struct cmsghdr *cmsg=(void *)control;*cmsg=(struct cmsghdr){CMSG_LEN(sizeof(int)),SOL_SOCKET,SCM_RIGHTS};
    int fd=5;memcpy(CMSG_DATA(cmsg),&fd,sizeof(fd));
    struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
    struct trap_frame frame={.rax=LINUX_SYS_SENDMSG,.rdi=3,.rsi=(uintptr_t)&message};
    struct socket_message_result output={0};current.syscall_file=&endpoints[0];
    task_lookup_scope_begin(&current,&frame);blocked=(uintptr_t)packet+32768;contended=true;
    assert(task_socket_message(&current,&endpoints[0],(uintptr_t)&message,0,false,false,&output)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!peers[1].packet_count && passed.references==2 && !passed.scm_references && first_prepares==1);
    memset(packet,0xa5,32768);contended=false;peers[1].packet_count=10;
    task_lookup_scope_begin(&current,&frame);
    assert(task_socket_message(&current,&endpoints[0],(uintptr_t)&message,0,false,false,&output)==KERNEL_SYSCALL_BLOCKED);
    assert(passed.references==2 && !passed.scm_references && first_prepares==1);
    swap_replace_passed=true;peers[1].packet_count=0;current.state=TASK_READY;
    task_lookup_scope_begin(&current,&frame);
    assert(task_socket_message(&current,&endpoints[0],(uintptr_t)&message,0,false,false,&output)==65536);
    assert(peers[1].packet_count==1 && peers[1].rights && peers[1].rights->files[0]==&passed);
    assert(passed_replacement.references==1 && !owned_stage);
    current.syscall_file=&endpoints[1];struct unix_receive_result metadata;struct unix_rights *rights=NULL;
    assert(unix_read_data(&endpoints[1],received_packet,sizeof(received_packet),MSG_PEEK|MSG_TRUNC,&rights,&metadata)==65536);
    for(size_t i=0;i<sizeof(packet);++i)assert(received_packet[i]==(uint8_t)(i*13u));
    assert(peers[1].packet_count==1 && passed.references==2);
    assert(unix_read_data(&endpoints[1],received_packet,sizeof(received_packet),0,&rights,&metadata)==65536);
    assert(rights && rights->files[0]==&passed);unix_rights_free(rights);
    task_lookup_scope_cancel(&current);assert(!allocations && passed.references==1);
    swap_replace_passed=false;memset(packet,3,sizeof(packet));current.syscall_file=&endpoints[0];
    task_lookup_scope_begin(&current,&frame);peers[1].packet_count=10;
    assert(task_socket_message(&current,&endpoints[0],(uintptr_t)&message,0,false,false,&output)==KERNEL_SYSCALL_BLOCKED);
    assert(passed.references==2);
    task_socket_stage_cancel(&current);
    task_lookup_scope_cancel(&current);assert(passed.references==1 && !allocations);
    peers[1].packet_count=0;current.state=TASK_READY;current.syscall_file=&endpoints[0];
    for(size_t i=0;i<sizeof(packet);++i)packet[i]=(uint8_t)(i*17u);
    struct trap_frame sendto_frame={.rax=LINUX_SYS_SENDTO,.rdi=3,.rsi=(uintptr_t)packet,.rdx=sizeof(packet)};
    task_lookup_scope_begin(&current,&sendto_frame);blocked=(uintptr_t)packet+32768;contended=true;
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,(uintptr_t)packet,sizeof(packet),0,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!peers[1].packet_count);
    memset(packet,0x77,32768);contended=false;task_lookup_scope_begin(&current,&sendto_frame);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,(uintptr_t)packet,sizeof(packet),0,0,0)==65536);
    current.syscall_file=&endpoints[1];rights=NULL;
    assert(unix_read_data(&endpoints[1],received_packet,sizeof(received_packet),0,&rights,&metadata)==65536);
    for(size_t i=0;i<sizeof(packet);++i)assert(received_packet[i]==(uint8_t)(i*17u));
    task_lookup_scope_cancel(&current);assert(!allocations);
    puts("PASS UNIX datagram sendmsg/sendto: staged 64KiB, original rights, peek and cancel");
    return 0;
}
#endif
