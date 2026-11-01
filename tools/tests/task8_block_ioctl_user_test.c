/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: block ioctl user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/fs.h>
#include <reliefnt/sched.h>
#include <reliefnt/swap.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>

static struct task current;
static struct task_file file;
static uint64_t result;
static bool pending_owner, foreign_contention;
static unsigned provider_calls, releases, allocations;
struct task *sched_current_task(void) { return &current; }
struct task_file *task_file_for_io(struct task *task, int fd)
{ assert(task == &current && fd == 3); return &file; }
int task_block_device(const struct task_file *f, uint32_t *disk, int32_t *part)
{ assert(f == &file); *disk = 7; *part = 2; return 1; }
int task_evdev_ioctl(struct task_file *f, uint64_t request, uint64_t argument)
{ assert(f == &file); (void)request; (void)argument; return -ENOTTY; }
void *kernel_malloc(size_t n) { void *p = malloc(n); if (p) ++allocations; return p; }
void kernel_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
bool user_range_ok(uint64_t address, uint64_t length)
{ return address == (uintptr_t)&result && length == sizeof(result) && !foreign_contention; }
bool user_range_writable(uint64_t address, uint64_t length) { return user_range_ok(address, length); }
int swap_prepare_user_range(struct task *task, uint64_t address, uint64_t length, bool writing)
{ assert(task == &current && address == (uintptr_t)&result && length == sizeof(result));
  if (pending_owner) return -EBUSY;
  return writing && foreign_contention ? SWAP_STORAGE_CONTENDED : 0; }
int storage_disk_block_info(uint32_t disk, int32_t part, uint64_t *first, uint64_t *sectors)
{ assert(disk == 7 && part == 2); ++provider_calls;
  if (provider_calls == 1) { pending_owner = true; return -EAGAIN; }
  assert(provider_calls == 2 && pending_owner); *first = 0; *sectors = 123;
  foreign_contention = true; return 0; }
void storage_release_task_io(uint32_t pid)
{ assert(pid == current.pid && pending_owner && provider_calls == 2); pending_owner = false; ++releases; }
void storage_drain_task_io(uint32_t pid) { assert(pid == current.pid); pending_owner = false; }
void vfs_walk_cancel(struct vfs_walk **walk) { assert(!*walk); }
void vfs_path_put(struct vfs_path *path) { assert(!path->mount); }
void storage_inode_allow_write(struct storage_inode_ref *inode) { (void)inode; assert(0); }
int storage_inode_put(struct storage_inode_ref *inode) { (void)inode; assert(0); return 0; }
void task_process_vm_scope_release(void *state) { (void)state; assert(0); }
int64_t syscall_legacy_ioctl(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static int64_t run(void)
{
    struct trap_frame frame = {.rax = LINUX_SYS_IOCTL, .rdi = 3,
                               .rsi = BLKGETSIZE64, .rdx = (uintptr_t)&result};
    task_lookup_scope_begin(&current, &frame);
    return syscall_legacy_ioctl(LINUX_SYS_IOCTL, 3, BLKGETSIZE64, (uintptr_t)&result, 0, 0, 0);
}
int main(void)
{
    current = (struct task){.pid = 90, .kind = TASK_KIND_USER, .syscall_file = &file};
    assert(run() == -EAGAIN && pending_owner && provider_calls == 1);
    assert(run() == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(provider_calls == 2 && !pending_owner && releases == 1 && !result);
    foreign_contention = false;
    assert(!run() && result == 123 * 512ULL && provider_calls == 2);
    task_lookup_scope_cancel(&current); assert(!allocations);
    puts("PASS block ioctl: pending owner completes before page-in, retained result avoids provider replay");
    return 0;
}
