/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief No-swap AF_UNIX sendto preserves full-range admission order. */
#define TASK8_NOSWAP_APERTURE_TEST 1
#define main inherited_unix_socket_batch_main
#include "socket_batch_unix_test.c"
#undef main

int main(void)
{
    current.kind=TASK_KIND_USER;current.pid=current.tgid=10;current.state=TASK_READY;
    for(unsigned i=0;i<2;++i) {
        peers[i]=(struct unix_socket){.used=1,.refs=1,.handle=i+1,.type=SOCK_STREAM,
            .state=UNIX_SOCKET_CONNECTED,.peer_handle=2-i,.receive_timeout=UINT64_MAX,.send_timeout=UINT64_MAX};
        endpoints[i]=(struct task_file){.used=1,.references=1,.aux=i+1,
            .flags=TASK_FILE_FLAG_SOCKET|TASK_FILE_FLAG_SOCKET_UNIX|RELIEFOS_O_RDWR};
    }
    current.syscall_file=&endpoints[0];
    uint64_t edge=RELIEFNT_USER_TOP-16384;
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,edge,16385,0,0,0)==-LINUX_EFAULT);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,edge,16385,MSG_OOB,0,0)==-LINUX_EFAULT);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,UINT64_MAX-2,8,MSG_OOB,0,0)==-LINUX_EFAULT);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,(uintptr_t)&current,1,MSG_OOB,0,0)==-LINUX_EOPNOTSUPP);
    assert(!peers[1].rx_written && !peers[1].packet_count);
    peers[0].type=peers[1].type=SOCK_DGRAM;
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,edge,65537,0,0,0)==-LINUX_EFAULT);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,UINT64_MAX-2,8,0,0,0)==-LINUX_EFAULT);
    /* The inherited no-swap front checks the user range before message flags. */
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,edge,65537,MSG_OOB,0,0)==-LINUX_EFAULT);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,UINT64_MAX-2,8,MSG_OOB,0,0)==-LINUX_EFAULT);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,(uintptr_t)&current,65537,0,0,0)==-LINUX_EMSGSIZE);
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,3,(uintptr_t)&current,1,MSG_OOB,0,0)==-LINUX_EOPNOTSUPP);
    current.syscall_file=NULL;
    assert(syscall_socket_dispatch(LINUX_SYS_SENDTO,99,edge,65537,MSG_OOB,0,0)==-LINUX_EBADF);
    assert(!peers[1].packet_count && !allocations);
    puts("PASS no-swap UNIX sendto full aperture and legacy flag order");return 0;
}
