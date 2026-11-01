/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: fbdev user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/syscall/device/fbdev_ioctl.c"
#include <reliefnt/swap.h>
static struct task current;
static bool busy, writable = true;
static unsigned mutations;
struct task *sched_current_task(void) { return &current; }
bool user_range_ok(uint64_t p, uint64_t n) { return p >= 4096 && n <= UINT64_MAX-p; }
bool user_range_writable(uint64_t p, uint64_t n) { return writable && user_range_ok(p,n); }
int swap_prepare_user_range(struct task *task,uint64_t p,uint64_t n,bool writing)
{
    assert(task == &current && p >= 4096 && n == sizeof(struct fb_var_screeninfo));
    return writing && busy ? SWAP_STORAGE_CONTENDED : 0;
}
int framebuffer_fbdev_put_var(struct fb_var_screeninfo *info)
{ assert(info->xres == 640); ++mutations; return 0; }
int main(void)
{
    current.kind = TASK_KIND_USER;
    struct fb_var_screeninfo info = {.xres=640};
    busy = true;
    assert(syscall_fbdev_put_var((uintptr_t)&info) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!mutations);
    busy = false;
    assert(!syscall_fbdev_put_var((uintptr_t)&info) && mutations == 1);
    writable = false;
    assert(syscall_fbdev_put_var((uintptr_t)&info) == -RELIEFOS_EFAULT && mutations == 2);
    puts("PASS fbdev: prerequisite before mode change; original late EFAULT retained");
    return 0;
}
