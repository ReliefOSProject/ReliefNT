/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: vector special user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/uio.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/swap.h>

static struct task current;
static struct task_file file;
static uint64_t vector_addr, blocked;
static bool contended;
static unsigned allocations, calls;
static uint64_t seen[4];
void *kernel_malloc(size_t n) { void *p = malloc(n); if (p) ++allocations; return p; }
void kernel_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
struct task_file *task_file_for_io(struct task *t, int fd)
{ assert(t == &current && fd == 3); return &file; }
int file_can_read(const struct task_file *f) { return f == &file; }
int file_can_write(const struct task_file *f) { return f == &file; }
bool user_range_ok(uint64_t address, uint64_t length)
{ return address >= 4096 && length <= UINT64_MAX - address && !(contended && address == blocked); }
bool user_range_writable(uint64_t address, uint64_t length) { return user_range_ok(address, length); }
int swap_prepare_user_range(struct task *task, uint64_t address, uint64_t length, bool output)
{
    assert(task == &current && length <= 32768); (void)output;
    return contended && address == blocked ? SWAP_STORAGE_CONTENDED : 0;
}
int64_t syscall_io_write(uint64_t fd, uint64_t address, uint64_t length)
{
    assert(fd == 3 && calls < 4); seen[calls++] = address;
    return contended && address == blocked ? KERNEL_SYSCALL_SWAP_CONTENDED : (int64_t)length;
}
int64_t syscall_io_read(uint64_t fd, uint64_t address, uint64_t length)
{ return syscall_io_write(fd, address, length); }
void storage_drain_task_io(uint32_t pid) { assert(pid == current.pid); }
void vfs_walk_cancel(struct vfs_walk **walk) { assert(!*walk); }
void vfs_path_put(struct vfs_path *path) { assert(!path->mount); }
void storage_inode_allow_write(struct storage_inode_ref *inode) { (void)inode; assert(0); }
int storage_inode_put(struct storage_inode_ref *inode) { (void)inode; assert(0); return 0; }
void task_process_vm_scope_release(void *state) { (void)state; assert(0); }
int64_t syscall_vector_io(struct task *, uint64_t, uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t);
static int64_t run(struct iovec *vectors)
{
    struct trap_frame frame = {.rax = LINUX_SYS_WRITEV, .rdi = 3,
                               .rsi = (uintptr_t)vectors, .rdx = 2};
    task_lookup_scope_begin(&current, &frame);
    return syscall_vector_io(&current, LINUX_SYS_WRITEV, 3, (uintptr_t)vectors, 2, 0, 0, 0);
}
int main(void)
{
    current = (struct task){.pid = 88, .kind = TASK_KIND_USER, .syscall_file = &file};
    file = (struct task_file){.flags = TASK_FILE_FLAG_PIPE | RELIEFOS_O_WRONLY};
    char a[8] = {0}, b[8] = {0}, replacement[8] = {0};
    struct iovec vectors[2] = {{a, sizeof(a)}, {b, sizeof(b)}};
    vector_addr = (uintptr_t)vectors; blocked = vector_addr; contended = true;
    assert(run(vectors) == KERNEL_SYSCALL_SWAP_CONTENDED && !calls);
    blocked = (uintptr_t)a;
    assert(run(vectors) == KERNEL_SYSCALL_SWAP_CONTENDED && calls == 1);
    vectors[0].iov_base = replacement;
    blocked = 0; calls = 0;
    assert(run(vectors) == 16 && calls == 2 && seen[0] == (uintptr_t)a && seen[1] == (uintptr_t)b);
    task_lookup_scope_cancel(&current); assert(!allocations);
    vectors[0].iov_base = a; blocked = (uintptr_t)b; calls = 0;
    assert(run(vectors) == 8 && calls == 2 && seen[0] == (uintptr_t)a);
    task_lookup_scope_cancel(&current); assert(!allocations);
    puts("PASS special vector: imported metadata snapshot, prerequisite retry and accepted prefix");
    return 0;
}
