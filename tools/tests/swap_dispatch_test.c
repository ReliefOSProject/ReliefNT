/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <linux/capability.h>

/** @brief Dispatch raw Linux registers. @param task Current task. @param nr Number.
 * @param a0 First argument. @param a1 Second argument. @param a2 Third argument.
 * @param a3 Fourth argument. @param a4 Fifth argument. @param a5 Sixth argument.
 * @return Syscall result. */
int64_t syscall_dispatch_regs(struct task *task,uint64_t nr,uint64_t a0,uint64_t a1,
                              uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5);
static struct task task;
static unsigned calls;
static unsigned contention_cleanups;
static int activation_status=-EAGAIN;
static int64_t legacy_status=KERNEL_SYSCALL_SWAP_CONTENDED;
static uint32_t received;
static struct task_file debug_file;
/** @brief Current credentials for real swap syscall handlers. @return Fixture task. */
struct task *sched_current_task(void) { return &task; }
/** @brief Record the exact activation operand. @param path Path register. @param flags Flags.
 * @return Terminal test status. */
int swap_activate_user(uint64_t path,uint32_t flags)
{
    assert(path==123);++calls;received=flags;
    if(activation_status==KERNEL_SYSCALL_SWAP_CONTENDED) {
        task.syscall_file=&debug_file;task.regular_io.active=true;
    }
    return activation_status;
}
/** @brief Record deactivation. @param path Path register. @return Test status. */
int swap_deactivate_user(uint64_t path) { assert(path==123);++calls;return -EINVAL; }
/** @brief Model a normalized read whose preparation meets another owner.
 * @param number Native syscall selector. @param a0 First argument.
 * @param a1 Second argument. @param a2 Third argument. @param a3 Fourth argument.
 * @param a4 Fifth argument. @param a5 Sixth argument. @return Private wait marker.
 */
int64_t syscall_legacy_io(uint64_t number,uint64_t a0,uint64_t a1,uint64_t a2,
                          uint64_t a3,uint64_t a4,uint64_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    assert(number == LINUX_SYS_READ && task.syscall_file == &debug_file);
    task.regular_io.active = true;
    return legacy_status;
}
/** @brief Observe terminal no-frame cleanup. @param owner Fixture task. */
void task_release_syscall_file(struct task *owner)
{
    assert(owner == &task && owner->regular_io.active && owner->syscall_file == &debug_file);
    owner->regular_io.active = false; owner->syscall_file = NULL; ++contention_cleanups;
}
/** @brief Exercise normalized and register dispatch into the actual swap handlers.
 * @return Zero when width, ordering, and disabled results match.
 */
int main(void)
{
    struct syscall_frame f={.number=LINUX_SYS_SWAPON,.args={0,0x80000}};
#ifdef CONFIG_SWAP
    assert(syscall_dispatch(&f)==-EINVAL);
    assert(syscall_dispatch_regs(&task,LINUX_SYS_SWAPON,0,0x80000,0,0,0,0)==-EINVAL);
    f.args[1]=0;assert(syscall_dispatch(&f)==-EPERM);
    assert(syscall_dispatch_regs(&task,LINUX_SYS_SWAPOFF,0,0,0,0,0,0)==-EPERM && !calls);
    task.cap_effective=1ULL<<CAP_SYS_ADMIN;
    f.args[0]=123;f.args[1]=0x100018005ULL;
    assert(syscall_dispatch(&f)==-EAGAIN && received==0x18005);
    assert(syscall_dispatch_regs(&task,LINUX_SYS_SWAPON,123,0x100018005ULL,0,0,0,0)==-EAGAIN);
    f.number=LINUX_SYS_SWAPOFF;assert(syscall_dispatch(&f)==-EINVAL);
    assert(syscall_dispatch_regs(&task,LINUX_SYS_SWAPOFF,123,0,0,0,0,0)==-EINVAL && calls==4);
#else
    assert(syscall_dispatch(&f)==-ENOSYS);
    assert(syscall_dispatch_regs(&task,LINUX_SYS_SWAPON,0,0x80000,0,0,0,0)==-ENOSYS);
    f.number=LINUX_SYS_SWAPOFF;assert(syscall_dispatch(&f)==-ENOSYS);
    assert(syscall_dispatch_regs(&task,LINUX_SYS_SWAPOFF,0,0,0,0,0,0)==-ENOSYS && !calls);
#endif
    task.syscall_file = &debug_file; task.syscall_fd = 8; task.syscall_file_number = LINUX_SYS_READ;
    f = (struct syscall_frame){.number=LINUX_SYS_READ,.args={8,0x10000,32}};
    assert(syscall_dispatch(&f) == -EBUSY && contention_cleanups == 1);
    assert(!task.regular_io.active && !task.syscall_file);
    legacy_status=KERNEL_SYSCALL_DRM_ENUM_WAIT;task.syscall_file=&debug_file;
    assert(syscall_dispatch(&f)==-EBUSY && contention_cleanups==2);
    assert(!task.regular_io.active && !task.syscall_file);
#ifdef CONFIG_SWAP
    activation_status=KERNEL_SYSCALL_SWAP_CONTENDED;
    f=(struct syscall_frame){.number=LINUX_SYS_SWAPON,.args={123,0}};
    assert(syscall_dispatch(&f)==-EBUSY && contention_cleanups==3);
    assert(!task.regular_io.active && !task.syscall_file);
#endif
    puts("swap normalized and register syscall routes: ok");return 0;
}
