/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: module user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <reliefnt/module.h>
#include <reliefnt/sched.h>
#include <reliefnt/swap.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>

static struct task current;
static struct task_file file;
static _Alignas(4096) unsigned char image[65536];
static _Alignas(4096) char args[512], name[64];
static uint64_t blocked;
static bool contended, pending_file;
static unsigned calls, pins, allocations;
struct task *sched_current_task(void) { return &current; }
void *kernel_malloc(size_t n) { void *p = malloc(n); if (p) ++allocations; return p; }
void kernel_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
int swap_prepare_user_range(struct task *task, uint64_t address, uint64_t length, bool output)
{ assert(task == &current && length <= 32768 && !output);
  return contended && address == blocked ? SWAP_STORAGE_CONTENDED : 0; }
int user_copy_from_task(struct task *task, void *out, uint64_t address, uint64_t size)
{ assert(task == &current); if (contended && address == blocked) return -EFAULT;
  memcpy(out, (void *)(uintptr_t)address, size); return 0; }
struct task_file *task_file_for_io(struct task *task, int fd)
{ assert(task == &current); return fd == 7 ? &file : NULL; }
struct task_file *task_file_get(struct task_file *f) { assert(f == &file); ++pins; return f; }
void task_file_put(struct task_file *f) { assert(f == &file && pins); --pins; }
int module_file_validate(const struct task_file *f, unsigned int flags)
{ assert(f == &file && !flags); return 0; }
int module_load_image(const void *bytes, size_t length, const char *params, unsigned int flags)
{
    assert(length == sizeof(image) && !flags && ((const unsigned char *)bytes)[0] == 0x5a);
    assert(((const unsigned char *)bytes)[40000] == 0x5a);
    assert(params[0] == 'A' && params[299] == 'A' && !params[300]); ++calls; return 0;
}
int module_load_file(struct task_file *f, const char *params, unsigned int flags)
{ assert(f == &file && !flags && params[0] == 'A' && params[299] == 'A' && !params[300]);
  if (pending_file) { pending_file = false; return MODULE_FILE_IO_RETRY; } ++calls; return 0; }
int module_unload(const char *text, unsigned int flags)
{ assert(!strcmp(text, "provider") && flags == 0x800); ++calls; return 0; }
void storage_drain_task_io(uint32_t pid) { assert(pid == current.pid); }
void vfs_walk_cancel(struct vfs_walk **walk) { assert(!*walk); }
void vfs_path_put(struct vfs_path *path) { assert(!path->mount); }
void storage_inode_allow_write(struct storage_inode_ref *inode) { (void)inode; assert(0); }
int storage_inode_put(struct storage_inode_ref *inode) { (void)inode; assert(0); return 0; }
void task_process_vm_scope_release(void *state) { (void)state; assert(0); }
int64_t syscall_modules(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2);
static int64_t run(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2)
{
    struct trap_frame frame = {.rax = nr, .rdi = a0, .rsi = a1, .rdx = a2};
    task_lookup_scope_begin(&current, &frame);
    return syscall_modules(nr, a0, a1, a2);
}
int main(void)
{
    current = (struct task){.pid = 89, .kind = TASK_KIND_USER,
                            .cap_effective = 1ULL << CAP_SYS_MODULE};
    memset(image, 0x5a, sizeof(image)); memset(args, 'A', 300); args[300] = 0;
    blocked = (uintptr_t)image + 32768; contended = true;
    assert(run(LINUX_SYS_INIT_MODULE, (uintptr_t)image, sizeof(image), (uintptr_t)args) ==
           KERNEL_SYSCALL_SWAP_CONTENDED && !calls);
    image[0] = 0x11; blocked = (uintptr_t)args + 256;
    assert(run(LINUX_SYS_INIT_MODULE, (uintptr_t)image, sizeof(image), (uintptr_t)args) ==
           KERNEL_SYSCALL_SWAP_CONTENDED && !calls);
    image[40000] = 0x22; args[0] = 'B'; blocked = 0; contended = false;
    assert(!run(LINUX_SYS_INIT_MODULE, (uintptr_t)image, sizeof(image), (uintptr_t)args));
    assert(calls == 1); task_lookup_scope_cancel(&current); assert(!allocations);
    memset(args, 'A', 300); args[300] = 0; pending_file = true;
    assert(run(LINUX_SYS_FINIT_MODULE, 7, (uintptr_t)args, 0) == MODULE_FILE_IO_RETRY);
    assert(!pins); args[0] = 'C';
    assert(!run(LINUX_SYS_FINIT_MODULE, 7, (uintptr_t)args, 0));
    assert(calls == 2 && !pins); task_lookup_scope_cancel(&current); assert(!allocations);
    strcpy(name, "provider"); blocked = (uintptr_t)name + 3; contended = true;
    assert(run(LINUX_SYS_DELETE_MODULE, (uintptr_t)name, 0x800, 0) == KERNEL_SYSCALL_SWAP_CONTENDED);
    name[0] = 'X'; blocked = 0; contended = false;
    assert(!run(LINUX_SYS_DELETE_MODULE, (uintptr_t)name, 0x800, 0));
    assert(calls == 3); task_lookup_scope_cancel(&current); assert(!allocations);
    puts("PASS module: bounded image/parameter/name import, retained prefixes and file retry snapshot");
    return 0;
}
