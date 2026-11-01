/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <linux/capability.h>
#include <linux/errno.h>

#ifdef CONFIG_SWAP
/** @brief Activate a validated swap path under execution ownership.
 * @param path User pathname. @param flags Validated low-32-bit Linux flags.
 * @return Zero or terminal negative errno.
 */
int swap_activate_user(uint64_t path,uint32_t flags);
/** @brief Deactivate a validated swap path under execution ownership.
 * @param path User pathname. @return Zero or terminal negative errno.
 */
int swap_deactivate_user(uint64_t path);
#endif

/** @brief Apply Linux swapon flag width and capability ordering.
 * @param user_path User pathname, validated after flags and capability.
 * @param flags Raw ABI register; Linux defines the argument as a 32-bit int.
 * @return Zero or terminal negative errno; disabled builds return -ENOSYS.
 */
int64_t syscall_mm_swapon(uint64_t user_path,uint64_t flags)
{
#ifdef CONFIG_SWAP
    uint32_t value=(uint32_t)flags;
    if (value & ~0x7ffffu) return -EINVAL;
    struct task *task=sched_current_task();
    if (!task || !(task->cap_effective&(1ULL<<CAP_SYS_ADMIN))) return -EPERM;
    return swap_activate_user(user_path,value);
#else
    (void)user_path;(void)flags;return -ENOSYS;
#endif
}

/** @brief Apply Linux swapoff capability ordering before pathname access.
 * @param user_path User pathname. @return Zero or terminal negative errno;
 * disabled builds return -ENOSYS without dereferencing the operand.
 */
int64_t syscall_mm_swapoff(uint64_t user_path)
{
#ifdef CONFIG_SWAP
    struct task *task=sched_current_task();
    if (!task || !(task->cap_effective&(1ULL<<CAP_SYS_ADMIN))) return -EPERM;
    return swap_deactivate_user(user_path);
#else
    (void)user_path;return -ENOSYS;
#endif
}
