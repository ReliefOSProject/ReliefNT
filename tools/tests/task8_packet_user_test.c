/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: packet user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_packet_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "net_packet_test.c"
#pragma clang diagnostic pop
#undef main
#include <reliefnt/swap.h>
#include <reliefnt/syscall_internal.h>

static uint64_t blocked;
static bool contended;
struct task_file *task_file_for_io(struct task *task, int fd)
{ return task->syscall_file && task->syscall_fd == fd ? task->syscall_file : task_file_for_fd(task, fd); }
int swap_prepare_user_range(struct task *task, uint64_t address, uint64_t length, bool output)
{ assert(task == &current && length <= 32768); (void)output;
  return contended && blocked && address<=blocked && blocked-address<length ? SWAP_STORAGE_CONTENDED : 0; }
void storage_drain_task_io(uint32_t pid) { assert(pid == current.pid); }
void vfs_walk_cancel(struct vfs_walk **walk) { assert(!*walk); }
void vfs_path_put(struct vfs_path *path) { assert(!path->mount); }
void storage_inode_allow_write(struct storage_inode_ref *inode) { (void)inode; assert(0); }
int storage_inode_put(struct storage_inode_ref *inode) { (void)inode; assert(0); return 0; }
void task_process_vm_scope_release(void *state) { (void)state; assert(0); }
static int64_t scoped(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2)
{
    struct trap_frame f = {.rax=number,.rdi=a0,.rsi=a1,.rdx=a2};
    task_lookup_scope_begin(&current,&f);
    return syscall_packet(number,a0,a1,a2,0,0,0);
}
#ifdef TASK8_PACKET_READY_TEST
int inherited_task8_packet_user_main(void)
#else
int main(void)
#endif
{
    current = (struct task){.pid=93,.kind=TASK_KIND_USER,.cap_effective=1ULL<<CAP_NET_RAW};
    assert(syscall_packet(__NR_socket,AF_PACKET,SOCK_DGRAM|SOCK_NONBLOCK,8,0,0,0)==3);
    struct sockaddr_ll dest={.sll_family=AF_PACKET,.sll_ifindex=2,.sll_protocol=8,.sll_halen=6,
                             .sll_addr={255,255,255,255,255,255}};
    blocked=(uintptr_t)&dest;contended=true;
    assert(scoped(__NR_bind,3,(uintptr_t)&dest,sizeof(dest))==KERNEL_SYSCALL_SWAP_CONTENDED);
    contended=false;assert(!scoped(__NR_bind,3,(uintptr_t)&dest,sizeof(dest)));
    task_lookup_scope_cancel(&current);
    const char data[]="packet";assert(task_packet_send(&description,data,sizeof(data),0,
        (uintptr_t)&dest,sizeof(dest))==(int)sizeof(data));net_packet_input(sent,sent_length);
    char out[16]={0};struct sockaddr_ll source={0};uint32_t length=sizeof(source);
    struct trap_frame f={.rax=LINUX_SYS_RECVFROM,.rdi=3};task_lookup_scope_begin(&current,&f);
    blocked=(uintptr_t)&source;contended=true;
    assert(task_packet_recv(&description,out,sizeof(out),MSG_PEEK|MSG_TRUNC,
        (uintptr_t)&source,(uintptr_t)&length)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(task_packet_available(&description)==sizeof(data));
    length=1;blocked=0;contended=false;task_lookup_scope_begin(&current,&f);
    assert(task_packet_recv(&description,out,sizeof(out),MSG_PEEK|MSG_TRUNC,
        (uintptr_t)&source,(uintptr_t)&length)==(int)sizeof(data));
    assert(length==sizeof(source) && source.sll_family==AF_PACKET && task_packet_available(&description)==sizeof(data));
    task_lookup_scope_cancel(&current);
    assert(task_packet_recv(&description,out,sizeof(out),0,0,0)==(int)sizeof(data) && !memcmp(out,data,sizeof(data)));
    task_packet_release(&description);
    puts("PASS packet: swapped sockaddr wait, immutable length, queued PEEK/TRUNC packet");
    return 0;
}
