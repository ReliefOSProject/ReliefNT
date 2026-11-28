/* SPDX-License-Identifier: Apache-2.0 */
#include "futex_internal.h"
#include <reliefnt/futex.h>
#include <reliefnt/mm.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/time.h>
#include <reliefnt/usercopy.h>
#include <linux/errno.h>
#include <linux/futex.h>

/** @brief Mark a successful futex store dirty when the page cache is present.
 * @param physical Resident frame retained by the execution-owned user mapping.
 * The optional page-cache implementation serializes its own dirty state.
 */
extern void page_cache_note_write(uint64_t physical) __attribute__((weak));

/** @brief Match one PI queue entry by its private-MM or shared-page identity.
 * @param task Queued waiter. @param domain Key domain. @param key Word identity.
 * @return True only for PI waits on this exact key. Requires futex_lock.
 */
static bool pi_key_matches(const struct task *task, uint64_t domain, uint64_t key)
{
    return task->futex_pi && task->futex_domain == domain && task->futex_key == key;
}

/** @brief Finish a queued PI wait and drop its shared-frame/dependency ownership.
 * @param task Retained queued task. @param result Syscall result on continuation.
 * Caller holds futex_lock followed by scheduler lock when marking ready.
 */
static void pi_complete_locked(struct task *task, int result)
{
    futex_dequeue_locked(task);
    task->futex_pi_result = result;
    task->futex_state = 2;
    sched_mark_ready(task->pid);
}

/** @brief Publish the next same-priority owner's TID before waking it.
 * @param domain Private MM root or zero. @param key VA or physical key.
 * @param word Resident writable alias. @param value Expected owner word.
 * @param died Preserve OWNER_DIED for robust acquisition.
 * @return Zero after transfer, EINVAL if user space changed the owner word.
 * SCHED_OTHER is the sole admitted policy, so Linux rtmutex uses equal effective
 * priority for all these waiters. Their arrival order resolves the tie.
 */
static int pi_handoff_locked(uint64_t domain, uint64_t key, uint32_t *word,
                              uint32_t value, bool died)
{
    struct task *next = NULL;
    bool more = false;
    for (struct task *task = futex_waiters; task;) {
        struct task *following = task->futex_next;
        if (pi_key_matches(task, domain, key)) {
            if (task->futex_deadline && task->futex_deadline <= time_ticks())
                pi_complete_locked(task, -ETIMEDOUT);
            else if (!next) next = task;
            else more = true;
        }
        task = following;
    }
    uint32_t replacement = died ? FUTEX_OWNER_DIED : 0;
    if (next) replacement |= next->pid;
    if (more) replacement |= FUTEX_WAITERS;
    if (!__atomic_compare_exchange_n(word, &value, replacement, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return -EINVAL;
    if (next) {
        pi_complete_locked(next, 0);
        for (struct task *task = futex_waiters; task; task = task->futex_next)
            if (pi_key_matches(task, domain, key)) task->futex_pi_owner = next;
    }
    return 0;
}

/** @brief Retry a parked PI acquisition before touching its possibly removed VA.
 * @param task Current task. @return Completion result or the parked-syscall
 * marker. Pending signals keep the invocation restartable. Requires futex_lock.
 */
static int64_t pi_resume_locked(struct task *task)
{
    int64_t result = KERNEL_SYSCALL_BLOCKED;
    if (task->futex_state == 2) result = task->futex_pi_result;
    else if (task->futex_deadline && task->futex_deadline <= time_ticks()) result = -ETIMEDOUT;
    else if (sched_task_pending(task) & ~task->blocked_signals) {
        /* Preserve the parked invocation until signal delivery can install its
         * unconditional restart frame. EINTR must never escape a PI lock. */
        sched_mark_ready(task->pid);
        return KERNEL_SYSCALL_BLOCKED;
    }
    if (result != KERNEL_SYSCALL_BLOCKED) {
        futex_dequeue_locked(task);
        task->futex_state = 0;
        task->futex_pi = false;
        task->futex_pi_result = 0;
    } else if (task->futex_deadline) sched_sleep_current_until(task->futex_deadline);
    else sched_block_current();
    return result;
}

/** @brief Execute Linux PI futex locking for supported SCHED_OTHER tasks.
 * @param task Current task. @param address Aligned user lock word.
 * @param operation Classic futex flags/opcode. @param timeout Absolute timespec
 * for LOCK_PI/LOCK_PI2, ignored for TRYLOCK_PI/UNLOCK_PI.
 * @return Zero, negative errno, or KERNEL_SYSCALL_BLOCKED with one queued wait.
 * Page-in/COW precede the queue lock. Shared waits retain their keyed frame.
 */
int64_t futex_pi_syscall(struct task *task, uint64_t address,
                         uint32_t operation, uint64_t timeout)
{
    uint32_t op = operation & FUTEX_CMD_MASK;
    bool locking = op == FUTEX_LOCK_PI || op == FUTEX_LOCK_PI2;
    uint64_t flags, until = 0, domain, key;
    if ((operation & FUTEX_CLOCK_REALTIME) && op != FUTEX_LOCK_PI2) return -ENOSYS;
    kernel_spin_lock_irqsave(&futex_lock, &flags);
    if (locking && task->futex_pi && task->futex_state) {
        int64_t result = pi_resume_locked(task);
        kernel_spin_unlock_irqrestore(&futex_lock, flags);
        return result;
    }
    kernel_spin_unlock_irqrestore(&futex_lock, flags);
    if (address & 3) return -EINVAL;
    int ret;
    if (locking) {
        uint32_t clock_op = operation;
        if (op == FUTEX_LOCK_PI) clock_op |= FUTEX_CLOCK_REALTIME;
        ret = futex_deadline(timeout, clock_op, &until);
        if (ret) return ret;
    }
    ret = futex_prepare_user(address, sizeof(uint32_t), true);
    if (ret) return ret;
    if (!user_range_writable(address, sizeof(uint32_t))) return -EFAULT;
    ret = futex_key(task, address, operation, &domain, &key);
    if (ret) return ret;
    uint32_t *word = (uint32_t *)(uintptr_t)address;
    kernel_spin_lock_irqsave(&futex_lock, &flags);
    if (futex_queue_conflicts_locked(domain, key, true)) { ret = -EINVAL; goto out; }
    uint32_t value = __atomic_load_n(word, __ATOMIC_SEQ_CST);
    if (op == FUTEX_UNLOCK_PI) {
        if ((value & FUTEX_TID_MASK) != task->pid) { ret = -EPERM; goto out; }
        ret = pi_handoff_locked(domain, key, word, value, false);
        goto changed;
    }
    for (;;) {
        uint32_t tid = value & FUTEX_TID_MASK;
        if (tid == task->pid) { ret = -EDEADLK; goto out; }
        if (!tid) {
            uint32_t replacement = (value & FUTEX_OWNER_DIED) | task->pid;
            if (__atomic_compare_exchange_n(word, &value, replacement, false,
                                             __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                ret = 0;
                goto changed;
            }
            continue;
        }
        struct task *owner = sched_find(tid);
        if (!owner || owner->kind != TASK_KIND_USER || owner->state == TASK_EXITED) {
            ret = -ESRCH;
            goto out;
        }
        for (struct task *dependency = owner; dependency;
             dependency = dependency->futex_pi && dependency->futex_state == 1
                 ? dependency->futex_pi_owner : NULL) {
            if (dependency == task) { ret = -EDEADLK; goto out; }
        }
        if (op == FUTEX_TRYLOCK_PI) { ret = -EAGAIN; goto out; }
        if (timeout && until <= time_ticks()) { ret = -ETIMEDOUT; goto out; }
        uint32_t replacement = value | FUTEX_WAITERS;
        if (!__atomic_compare_exchange_n(word, &value, replacement, false,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) continue;
        task->futex_pi = true;
        task->futex_pi_owner = owner;
        task->futex_pi_result = 0;
        task->futex_domain = domain;
        task->futex_key = key;
        task->futex_address = address;
        task->futex_deadline = until;
        task->futex_waitv_count = 0;
        task->futex_state = 1;
        task->futex_next = NULL;
        if (!domain) {
            task->futex_pi_page = key & ~4095ULL;
            mm_retain_page(task->futex_pi_page);
        }
        struct task **tail = &futex_waiters;
        while (*tail) tail = &(*tail)->futex_next;
        *tail = task;
        if (until) sched_sleep_current_until(until);
        else sched_block_current();
        ret = KERNEL_SYSCALL_BLOCKED;
        goto changed;
    }
changed:
    if ((ret == 0 || ret == KERNEL_SYSCALL_BLOCKED) && page_cache_note_write)
        page_cache_note_write(address_space_user_page_phys(sched_task_as(task), address));
out:
    kernel_spin_unlock_irqrestore(&futex_lock, flags);
    return ret;
}

/** @brief Recover a tagged robust PI word and hand ownership to its first waiter.
 * @param task Exiting owner. @param address Its original user VA.
 * @param word Writable resident alias. Caller retains the MM and futex_lock.
 */
void futex_pi_robust_exit_locked(struct task *task, uint64_t address, uint32_t *word)
{
    uint32_t value = __atomic_load_n(word, __ATOMIC_SEQ_CST);
    if ((value & FUTEX_TID_MASK) != task->pid) return;
    uint64_t domain = sched_task_as(task)->cr3;
    uint64_t key = address;
    uint64_t physical = address_space_user_page_phys(sched_task_as(task), address);
    for (struct task *waiter = futex_waiters; waiter; waiter = waiter->futex_next) {
        if (pi_key_matches(waiter, 0, physical + (address & 4095))) {
            domain = 0;
            key = physical + (address & 4095);
            break;
        }
    }
    if (!pi_handoff_locked(domain, key, word, value, true) && page_cache_note_write)
        page_cache_note_write(physical);
}

/** @brief Complete all remaining nonrobust waits before the owner's slot is freed.
 * @param owner Exiting task retained by the caller. Requires futex_lock.
 */
void futex_pi_owner_exit_locked(struct task *owner)
{
    for (struct task *task = futex_waiters; task;) {
        struct task *next = task->futex_next;
        if (task->futex_pi && task->futex_pi_owner == owner) pi_complete_locked(task, -ESRCH);
        task = next;
    }
}
