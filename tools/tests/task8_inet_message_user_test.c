/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: inet message user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_udp_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "udp_socket_test.c"
#pragma clang diagnostic pop
#undef main
#include <reliefnt/swap.h>
#include <reliefnt/syscall_internal.h>

static uint64_t blocked;
static uint64_t unmapped_output;
static bool contended;
static int tcp_error;
static unsigned tcp_wait_removed;
int net_socket_error(int32_t handle, bool clear)
{ assert(handle==44); int result=tcp_error;if(clear)tcp_error=0;return result; }
void net_socket_cancel_wait(int32_t handle,struct task *task)
{ assert(handle==44 && task==&current);++tcp_wait_removed; }
struct task_file *task_file_for_io(struct task *task, int fd)
{ return task->syscall_file && task->syscall_fd == fd ? task->syscall_file : task_file_for_fd(task, fd); }
int swap_prepare_user_range(struct task *task, uint64_t address, uint64_t length, bool output)
{ assert(task == &current && length <= 32768);
  if(output && unmapped_output && address<=unmapped_output && unmapped_output-address<length)return -LINUX_EFAULT;
  return contended && blocked && address<=blocked && blocked-address<length ? SWAP_STORAGE_CONTENDED : 0; }
void storage_drain_task_io(uint32_t pid) { assert(pid == current.pid); }
void vfs_walk_cancel(struct vfs_walk **walk) { assert(!*walk); }
void vfs_path_put(struct vfs_path *path) { assert(!path->mount); }
void storage_inode_allow_write(struct storage_inode_ref *inode) { (void)inode; assert(0); }
int storage_inode_put(struct storage_inode_ref *inode) { (void)inode; assert(0); return 0; }
void task_process_vm_scope_release(void *state) { (void)state; assert(0); }

static int64_t scoped_socket(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5)
{
    struct trap_frame f = {.rax=number,.rdi=a0,.rsi=a1,.rdx=a2,.r10=a3,.r8=a4,.r9=a5};
    task_lookup_scope_begin(&current,&f);
    return syscall_socket_dispatch(number,a0,a1,a2,a3,a4,a5);
}

#ifdef TASK8_INET_READY_TEST
int inherited_task8_inet_message_main(void)
#else
int main(void)
#endif
{
    current=(struct task){.pid=92,.kind=TASK_KIND_USER,.cap_effective=1ULL<<CAP_NET_BIND_SERVICE};
    int sender=open_socket(), receiver=open_socket(); assert(sender>=0 && receiver>=0);
    struct sockaddr_in dest={.sin_family=AF_INET,.sin_port=swap16(55000),
                             .sin_addr={.s_addr=swap32(0x7f000001)}};
    assert(!call(__NR_bind,receiver,(uintptr_t)&dest,sizeof(dest),0,0));
    assert(!call(__NR_connect,sender,(uintptr_t)&dest,sizeof(dest),0,0));
    assert(task_udp_send(&files[sender],"hello",5,0,0,0)==5);
    char out[8]={0}; struct sockaddr_in peer={0}; uint32_t peer_len=sizeof(peer);
    blocked=(uintptr_t)out;contended=true;
    assert(scoped_socket(__NR_recvfrom,receiver,(uintptr_t)out,sizeof(out),0,
                         (uintptr_t)&peer,(uintptr_t)&peer_len)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(task_udp_available(&files[receiver])==5);
    blocked=(uintptr_t)&peer_len;
    assert(scoped_socket(__NR_recvfrom,receiver,(uintptr_t)out,sizeof(out),0,
                         (uintptr_t)&peer,(uintptr_t)&peer_len)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(task_udp_available(&files[receiver])==5);
    blocked=(uintptr_t)&peer;
    assert(scoped_socket(__NR_recvfrom,receiver,(uintptr_t)out,sizeof(out),0,
                         (uintptr_t)&peer,(uintptr_t)&peer_len)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(task_udp_available(&files[receiver])==5);
    contended=false;task_lookup_scope_cancel(&current);
    struct iovec vectors[2]={{.iov_base=out,.iov_len=2},{.iov_base=out+2,.iov_len=1}};
    struct msghdr msg={.msg_iov=vectors,.msg_iovlen=2,.msg_name=&peer,.msg_namelen=sizeof(peer)};
    blocked=(uintptr_t)&peer;contended=true;
    assert(scoped_socket(__NR_recvmsg,receiver,(uintptr_t)&msg,MSG_PEEK|MSG_TRUNC,0,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(task_udp_available(&files[receiver])==5 && !out[0]);
    msg.msg_namelen=1;contended=false;
    assert(scoped_socket(__NR_recvmsg,receiver,(uintptr_t)&msg,MSG_PEEK|MSG_TRUNC,0,0,0)==5);
    assert(msg.msg_namelen==sizeof(peer) && msg.msg_flags==MSG_TRUNC && !memcmp(out,"hel",3));
    assert(task_udp_available(&files[receiver])==5);
    task_lookup_scope_cancel(&current);
    assert(task_udp_recv(&files[receiver],out,sizeof(out),0,0,0)==5);
    char first[3]="ab",second[3]="cd";
    struct iovec sendvec[2]={{.iov_base=first,.iov_len=2},{.iov_base=second,.iov_len=2}};
    struct msghdr sendmsg={.msg_iov=sendvec,.msg_iovlen=2};
    blocked=(uintptr_t)second;contended=true;
    assert(scoped_socket(__NR_sendmsg,sender,(uintptr_t)&sendmsg,0,0,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(task_udp_available(&files[receiver])==0);
    first[0]='X';contended=false;
    assert(scoped_socket(__NR_sendmsg,sender,(uintptr_t)&sendmsg,0,0,0,0)==4);
    task_lookup_scope_cancel(&current);
    assert(task_udp_recv(&files[receiver],out,sizeof(out),0,0,0)==4 && !memcmp(out,"abcd",4));
    char direct[4]="abc";blocked=(uintptr_t)&dest;contended=true;
    assert(scoped_socket(__NR_sendto,sender,(uintptr_t)direct,3,0,(uintptr_t)&dest,sizeof(dest))==KERNEL_SYSCALL_SWAP_CONTENDED);
    direct[0]='X';contended=false;
    assert(scoped_socket(__NR_sendto,sender,(uintptr_t)direct,3,0,(uintptr_t)&dest,sizeof(dest))==3);
    task_lookup_scope_cancel(&current);
    assert(task_udp_recv(&files[receiver],out,sizeof(out),0,0,0)==3 && !memcmp(out,"abc",3));
    assert(scoped_socket(__NR_sendto,sender,0,0,0,0,0)==0);
    task_lookup_scope_cancel(&current);
    assert(task_udp_recv(&files[receiver],out,sizeof(out),0,0,0)==0);
    files[79]=(struct task_file){.used=1,.flags=TASK_FILE_FLAG_SOCKET|TASK_FILE_FLAG_SOCKET_INET,.aux=44};
    int error=0;uint32_t error_len=sizeof(error);tcp_error=LINUX_ECONNRESET;
    blocked=(uintptr_t)&error;contended=true;
    assert(scoped_socket(__NR_getsockopt,79,SOL_SOCKET,SO_ERROR,(uintptr_t)&error,
                         (uintptr_t)&error_len,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(tcp_error==LINUX_ECONNRESET && !error);
    contended=false;
    assert(!scoped_socket(__NR_getsockopt,79,SOL_SOCKET,SO_ERROR,(uintptr_t)&error,
                          (uintptr_t)&error_len,0));
    assert(error==LINUX_ECONNRESET && !tcp_error);
    task_lookup_scope_cancel(&current);
    task_udp_release(&files[sender]);task_udp_release(&files[receiver]);
    assert(!allocations);
    puts("PASS INET recvfrom prerequisite, recvmsg immutable header, PEEK/TRUNC and cancellation");
    return 0;
}
