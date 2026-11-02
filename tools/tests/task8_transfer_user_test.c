/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: transfer user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/stat.h>
#include <reliefnt/mount.h>
#include <reliefnt/sched.h>
#include <reliefnt/swap.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/usercopy.h>
#include "../../kernel/syscall/io/transfer.c"

static struct task current;
static struct task_file source, destination;
static uintptr_t blocked, block_after_write;
static bool contended, sync_wait;
static unsigned writes, reads, releases, drains, refs, allocations;
struct task *sched_current_task(void) { return &current; }
struct task_file *task_file_for_io(struct task *task, int fd)
{ assert(task == &current); return fd == 3 ? &source : fd == 4 ? &destination : NULL; }
struct task_file *task_file_get(struct task_file *file) { if (file) ++refs; return file; }
void task_file_put(struct task_file *file) { if (file) { assert(refs); --refs; } }
void *kernel_malloc(size_t size) { void *p = malloc(size); if (p) ++allocations; return p; }
void kernel_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
int file_can_read(const struct task_file *file) { return file == &source; }
int file_can_write(const struct task_file *file) { return file == &destination; }
const struct storage_node *vfs_path_node(const struct vfs_path *path)
{ return path == &source.path_ref ? &source.node : path == &destination.path_ref ? &destination.node : NULL; }
bool vfs_path_same_filesystem(const struct vfs_path *a, const struct vfs_path *b)
{ return a == &source.path_ref && b == &destination.path_ref; }
bool vfs_path_same_inode(const struct vfs_path *a, const struct vfs_path *b)
{ (void)a; (void)b; return false; }
int vfs_path_stat(const struct vfs_path *path, struct linux_stat_abi *out)
{ assert(path == &source.path_ref); *out = (struct linux_stat_abi){.st_size = 32}; return 0; }
int vfs_path_read_cursor(const struct vfs_path *path, uint64_t offset, void *buffer,
                         uint32_t capacity, uint32_t *read, struct storage_read_cursor *cursor)
{ assert(path == &source.path_ref && offset == 1 && capacity == 3 && cursor); ++reads;
  memcpy(buffer, "abc", 3); *read = 3; return 0; }
int vfs_path_write(const struct vfs_path *path, uint64_t offset, const void *bytes,
                   uint32_t length, uint32_t *written)
{ assert(path == &destination.path_ref && offset == 2 && length == 3 && !memcmp(bytes, "abc", 3));
  ++writes; *written = 3; if (block_after_write) { blocked = block_after_write; contended = true; }
  return 0; }
int vfs_path_sync_after_write(const struct vfs_path *path, bool data_only)
{ assert(path == &destination.path_ref && data_only); return sync_wait ? -EAGAIN : 0; }
void storage_set_io_async_context(bool enabled) { assert(!enabled); }
void storage_release_task_io(uint32_t pid) { assert(pid == current.pid); ++releases; }
void storage_drain_task_io(uint32_t pid) { assert(pid == current.pid); ++drains; }
uint64_t time_uptime_us(void) { return 0; }
uint64_t time_work_counter_us(void) { return time_uptime_us(); }
bool user_range_ok(uint64_t address, uint64_t length)
{ return address >= 4096 && length <= UINT64_MAX - address && !(contended && address == blocked); }
bool user_range_writable(uint64_t address, uint64_t length) { return user_range_ok(address, length); }
int user_copy_to_task(struct task *task, uint64_t address, const void *bytes, uint64_t length)
{ assert(task == &current); if (!user_range_writable(address, length)) return -EFAULT;
  memcpy((void *)(uintptr_t)address, bytes, length); return 0; }
int swap_prepare_user_range(struct task *task, uint64_t address, uint64_t length, bool output)
{ assert(task == &current && length == 8 && output);
  return contended && address == blocked ? SWAP_STORAGE_CONTENDED : 0; }

int main(void)
{
    current = (struct task){.pid = 77, .kind = TASK_KIND_USER, .state = TASK_RUNNING};
    source = (struct task_file){.node.type = RELIEFOS_FS_TYPE_FILE};
    destination = (struct task_file){.node.type = RELIEFOS_FS_TYPE_FILE};
    int64_t from = 1, to = 2;
    blocked = (uintptr_t)&from; contended = true;
    assert(syscall_copy_file_range(3, (uintptr_t)&from, 4, (uintptr_t)&to, 3, 0) ==
           KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(current.transfer_io && refs == 2 && !reads && !writes);
    blocked = (uintptr_t)&to;
    assert(syscall_copy_file_range(3, (uintptr_t)&from, 4, (uintptr_t)&to, 3, 0) ==
           KERNEL_SYSCALL_SWAP_CONTENDED);
    from = 99;
    blocked = 0; contended = false; block_after_write = (uintptr_t)&to;
    assert(syscall_copy_file_range(3, (uintptr_t)&from, 4, (uintptr_t)&to, 3, 0) ==
           KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(reads == 1 && writes == 1 && from == 4 && to == 2 && refs == 2);
    from = 101; to = 99; blocked = block_after_write = 0; contended = false;
    assert(syscall_copy_file_range(3, (uintptr_t)&from, 4, (uintptr_t)&to, 3, 0) == 3);
    assert(from == 101 && to == 5 && writes == 1 && !refs && !allocations && releases);
    puts("PASS transfer: stable offsets, retained OFDs, completed provider and once-only output cursor");
    return 0;
}
