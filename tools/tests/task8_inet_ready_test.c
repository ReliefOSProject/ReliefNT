/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Actual UDP queue and syscall reception with modeled user-page faults.
 * Tests unused output capacity, prerequisite waits and queue conservation.
 */
#define TASK8_INET_READY_TEST 1
#include "task8_inet_message_user_test.c"

int main(int argc,char **argv)
{
    assert(argc==2);
    current=(struct task){.pid=92,.kind=TASK_KIND_USER,.cap_effective=1ULL<<CAP_NET_BIND_SERVICE};
    int sender=open_socket(),receiver=open_socket();assert(sender>=0 && receiver>=0);
    struct sockaddr_in dest={.sin_family=AF_INET,.sin_port=swap16(55000),
                             .sin_addr={.s_addr=swap32(0x7f000001)}};
    assert(!call(__NR_bind,receiver,(uintptr_t)&dest,sizeof(dest),0,0));
    assert(!call(__NR_connect,sender,(uintptr_t)&dest,sizeof(dest),0,0));
    char out[32]={0};
    struct iovec vector={.iov_base=out,.iov_len=sizeof(out)};
    struct msghdr msg={.msg_iov=&vector,.msg_iovlen=1};
    bool zero=!strcmp(argv[1],"msg-zero") || !strcmp(argv[1],"from-zero");
    bool message=!strcmp(argv[1],"msg-empty") || !strcmp(argv[1],"msg-short") || !strcmp(argv[1],"msg-zero");
    bool empty=!strcmp(argv[1],"msg-empty") || !strcmp(argv[1],"from-empty");
    if(zero)assert(!task_udp_send(&files[sender],"",0,0,0,0));
    else if(!empty)assert(task_udp_send(&files[sender],"hello",5,0,0,0)==5);
    blocked=(uintptr_t)out+((empty||zero) ? 0 : 5);contended=true;
    int64_t result=message ? scoped_socket(__NR_recvmsg,receiver,(uintptr_t)&msg,0,0,0,0) :
        scoped_socket(__NR_recvfrom,receiver,(uintptr_t)out,sizeof(out),0,0,0);
    if(empty)assert(result==-LINUX_EAGAIN);
    else if(zero)assert(!result && !(task_udp_poll(&files[receiver],POLLIN)&POLLIN));
    else assert(result==5 && !memcmp(out,"hello",5) && task_udp_available(&files[receiver])==0);
    task_lookup_scope_cancel(&current);
    if(zero) {
        assert(!task_udp_send(&files[sender],"",0,0,0,0));
        result=message ? scoped_socket(__NR_recvmsg,receiver,(uintptr_t)&msg,MSG_PEEK|MSG_TRUNC,0,0,0) :
            scoped_socket(__NR_recvfrom,receiver,(uintptr_t)out,sizeof(out),MSG_PEEK|MSG_TRUNC,0,0);
        assert(!result && (task_udp_poll(&files[receiver],POLLIN)&POLLIN));
        task_lookup_scope_cancel(&current);
        assert(!task_udp_recv(&files[receiver],out,sizeof(out),0,0,0));
        assert(!(task_udp_poll(&files[receiver],POLLIN)&POLLIN));
    } else if(!empty) {
        assert(task_udp_send(&files[sender],"hello",5,0,0,0)==5);
        blocked=(uintptr_t)out;contended=true;
        result=message ? scoped_socket(__NR_recvmsg,receiver,(uintptr_t)&msg,MSG_PEEK|MSG_TRUNC,0,0,0) :
            scoped_socket(__NR_recvfrom,receiver,(uintptr_t)out,3,MSG_PEEK|MSG_TRUNC,0,0);
        assert(result==KERNEL_SYSCALL_SWAP_CONTENDED && task_udp_available(&files[receiver])==5);
        blocked=0;contended=false;unmapped_output=(uintptr_t)out+5;
        result=message ? scoped_socket(__NR_recvmsg,receiver,(uintptr_t)&msg,MSG_PEEK|MSG_TRUNC,0,0,0) :
            scoped_socket(__NR_recvfrom,receiver,(uintptr_t)out,3,MSG_PEEK|MSG_TRUNC,0,0);
        assert(result==5 && task_udp_available(&files[receiver])==5);
        task_lookup_scope_cancel(&current);unmapped_output=0;
        assert(task_udp_recv(&files[receiver],out,sizeof(out),0,0,0)==5);
    }
    task_udp_release(&files[sender]);task_udp_release(&files[receiver]);assert(!allocations);
    puts("PASS UDP readiness and actual output length");return 0;
}
