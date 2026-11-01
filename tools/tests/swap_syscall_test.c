/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <reliefnt/sched.h>
#include <linux/capability.h>
extern int64_t syscall_mm_swapon(uint64_t,uint64_t) __attribute__((weak));
extern int64_t syscall_mm_swapoff(uint64_t) __attribute__((weak));
static struct task task;static int calls;static uint32_t observed_flags;
/** @brief Return fixture task. @return Borrowed task. */
struct task *sched_current_task(void) { return &task; }
/** @brief Observe authorized activation. @param path Raw path. @param flags Int flags.
 * @return Injected terminal failure. */
int swap_activate_user(uint64_t path,uint32_t flags)
{ assert(path==123);++calls;observed_flags=flags;return -EAGAIN; }
/** @brief Observe authorized deactivation. @param path Raw path. @return Terminal failure. */
int swap_deactivate_user(uint64_t path) { assert(path==123);++calls;return -EINVAL; }

/** @brief Validate flag width and privilege ordering at the production syscall boundary.
 * @return Zero when all assertions hold.
 */
int main(void)
{
    if(!syscall_mm_swapon || !syscall_mm_swapoff){fputs("FAIL: swap syscalls are absent\n",stderr);return 1;}
#ifdef CONFIG_SWAP
    assert(syscall_mm_swapon(0,0x80000)==-EINVAL && !calls);
    assert(syscall_mm_swapon(0,0)==-EPERM && !calls);
    assert(syscall_mm_swapoff(0)==-EPERM && !calls);
    task.cap_effective=1ULL<<CAP_SYS_ADMIN;
    assert(syscall_mm_swapon(123,0x100000000ULL|0x18005)==-EAGAIN && calls==1 && observed_flags==0x18005);
    assert(syscall_mm_swapoff(123)==-EINVAL && calls==2);
#else
    assert(syscall_mm_swapon(0,0x80000)==-ENOSYS && !calls);
    assert(syscall_mm_swapoff(0)==-ENOSYS && !calls);
#endif
    puts("swap syscall ABI ordering: ok");return 0;
}
