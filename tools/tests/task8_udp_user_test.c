/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: udp user.
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
static bool contended;
struct task_file *task_file_for_io(struct task *task, int fd)
{ return task->syscall_file && task->syscall_fd == fd ? task->syscall_file : task_file_for_fd(task, fd); }
int swap_prepare_user_range(struct task *task, uint64_t address, uint64_t length, bool output)
{ assert(task == &current && length <= 32768); (void)output;
  return contended && address == blocked ? SWAP_STORAGE_CONTENDED : 0; }
void storage_drain_task_io(uint32_t pid) { assert(pid == current.pid); }
void vfs_walk_cancel(struct vfs_walk **walk) { assert(!*walk); }
void vfs_path_put(struct vfs_path *path) { assert(!path->mount); }
void storage_inode_allow_write(struct storage_inode_ref *inode) { (void)inode; assert(0); }
int storage_inode_put(struct storage_inode_ref *inode) { (void)inode; assert(0); return 0; }
void task_process_vm_scope_release(void *state) { (void)state; assert(0); }
static int64_t scoped(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                      uint64_t a3, uint64_t a4)
{
    struct trap_frame f = {.rax = number, .rdi = a0, .rsi = a1, .rdx = a2, .r10 = a3, .r8 = a4};
    task_lookup_scope_begin(&current, &f);
    return syscall_udp(number, a0, a1, a2, a3, a4, 0);
}
int main(void)
{
    current = (struct task){.pid = 92, .kind = TASK_KIND_USER,
                            .cap_effective = 1ULL << CAP_NET_BIND_SERVICE};
    int sender = open_socket(), receiver = open_socket(); assert(sender >= 0 && receiver >= 0);
    struct sockaddr_in dest = {.sin_family = AF_INET, .sin_port = swap16(55000),
                               .sin_addr = {.s_addr = swap32(0x7f000001)}};
    blocked = (uintptr_t)&dest; contended = true;
    assert(scoped(__NR_bind, receiver, (uintptr_t)&dest, sizeof(dest), 0, 0) == KERNEL_SYSCALL_SWAP_CONTENDED);
    contended = false;
    assert(!scoped(__NR_bind, receiver, (uintptr_t)&dest, sizeof(dest), 0, 0));
    task_lookup_scope_cancel(&current);
    assert(!call(__NR_connect, sender, (uintptr_t)&dest, sizeof(dest), 0, 0));
    char out[8] = {0};
    assert(task_udp_send(&files[sender], "hello", 5, 0, 0, 0) == 5);
    struct sockaddr_in peer = {0}; uint32_t length = sizeof(peer);
    struct trap_frame f = {.rax=LINUX_SYS_RECVFROM, .rdi=(uint64_t)receiver};
    task_lookup_scope_begin(&current,&f);
    blocked = (uintptr_t)&peer; contended = true;
    assert(task_udp_recv(&files[receiver], out, sizeof(out), MSG_PEEK | MSG_TRUNC,
                         (uintptr_t)&peer, (uintptr_t)&length) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(task_udp_available(&files[receiver]) == 5);
    length = 1; blocked = 0; contended = false; task_lookup_scope_begin(&current,&f);
    assert(task_udp_recv(&files[receiver], out, sizeof(out), MSG_PEEK | MSG_TRUNC,
                         (uintptr_t)&peer, (uintptr_t)&length) == 5);
    assert(length == sizeof(peer) && peer.sin_family == AF_INET && task_udp_available(&files[receiver]) == 5);
    task_lookup_scope_cancel(&current);
    assert(task_udp_recv(&files[receiver], out, sizeof(out), 0, 0, 0) == 5 && !memcmp(out,"hello",5));
    task_udp_release(&files[sender]); task_udp_release(&files[receiver]);
    assert(!allocations);
    puts("PASS UDP: swapped sockaddr wait, immutable length, queued PEEK/TRUNC packet");
    return 0;
}
