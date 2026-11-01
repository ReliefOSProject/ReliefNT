/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/lock.h>
#include <reliefnt/memory_pressure.h>
#include <reliefnt/mm.h>
#include <reliefnt/sched.h>
#include <reliefnt/time.h>
#include <reliefnt/wait.h>
#include <linux/errno.h>

/* Task links are intrusive and require no memory allocation under pressure. */
static struct kernel_wait_queue pressure_waiters = { .lock = KERNEL_SPINLOCK_INIT };
static bool release_notify_installed;
/* Distinct-tick observations, not hardware transfer or wall-clock deadlines. */
/* One leaf may need a data frame, staged root and three staged children.
 * Above this count a provider may still need more, but OOM victim selection
 * must not be driven solely by its opaque allocation failure. */
#define PRESSURE_FAULT_FRAME_TARGET 5u

/** @brief Wake one waiter after real page gain or completed MM retirement.
 * Queue and scheduler locks protect links across CPUs. The notification does
 * not claim that a signal or a partially retired MM freed a frame.
 */
void memory_pressure_notify(void)
{ (void)kernel_wait_queue_wake_one(&pressure_waiters); }

/** @brief Detach a cancelled/completed task and forget its stall accounting.
 * @param task Execution-owned task, nullable. A different queue is untouched.
 * Any headroom/fault ownership of the removed link is forgotten as well.
 */
void memory_wait_cancel(struct task *task)
{
    if (!task) return;
    if (task->waiting_queue == &pressure_waiters)
        kernel_wait_queue_remove(&pressure_waiters, task);
#ifdef CONFIG_SWAP
    struct pressure_headroom_state *state=sched_pressure_headroom_state(task);
    if(state)state->wait_owned=false;
#endif
    sched_pressure_reset(task);
}

/** @brief Validate and prepare a current task's allocation-free pressure wait.
 * @param task Candidate current user task. @return Zero or terminal errno.
 */
static int memory_fault_wait_admit(struct task *task)
{
    if (!kernel_execution_is_outermost()) return -EDEADLK;
    if (!task || task != sched_current_task() || !task->pid ||
        task->kind != TASK_KIND_USER || task->state != TASK_RUNNING) return -ESRCH;
    if (task->waiting_queue && task->waiting_queue != &pressure_waiters) return -EBUSY;
    if (!release_notify_installed) {
        sched_set_memory_release_notify(memory_pressure_notify);
        release_notify_installed = true;
    }
    return 0;
}

/** @brief Bound one repeated stall and publish a timed queue sleep.
 * @param task Admitted current task. @param address User fault address.
 * @param cause Completed storage/victim stall, or NONE during cursor progress.
 * @return -EAGAIN after sleep publication, otherwise a terminal errno.
 * A new generic wait supersedes any prior headroom/fault ownership of the link;
 * the private fault dispatcher reclaims ownership after a successful park.
 */
static int memory_fault_wait_defer(struct task *task, uint64_t address,
                                   enum pressure_stall_cause cause)
{
#ifdef CONFIG_SWAP
    struct pressure_headroom_state *state=sched_pressure_headroom_state(task);
    if(state)state->wait_owned=false;
#endif
    uint64_t now = time_ticks();
    if (cause != PRESSURE_STALL_NONE) {
        uint32_t stalls = sched_pressure_stall(task, address, mm_free_memory_kib() / 4, cause, now);
        if ((cause == PRESSURE_STALL_STORAGE && stalls > PRESSURE_STORAGE_STALL_LIMIT) ||
            (cause == PRESSURE_STALL_VICTIM && stalls > PRESSURE_VICTIM_STALL_LIMIT)) {
            memory_wait_cancel(task);
            return cause == PRESSURE_STALL_STORAGE ? -EBUSY : -ENOMEM;
        }
    }
    uint64_t deadline = now == UINT64_MAX ? UINT64_MAX : now + 1;
    if (kernel_wait_queue_sleep_current_until(&pressure_waiters, deadline)) {
        memory_wait_cancel(task); return -EBUSY;
    }
    return -EAGAIN;
}

/** @brief Reclaim once or sleep a faulting task until a safe retry boundary.
 * @param task Current live user task under the sole outer execution writer.
 * @param address Faulting address, used only to distinguish progress attempts.
 * @return Zero for retry, -EAGAIN after timed blocking, -ENOMEM if no victim
 * exists, snapshot churn prevents safe selection, or an ample-free scan has
 * no releasable frame, or another terminal errno.
 * Churn can produce -ENOMEM even while a victim exists.
 * No lock or borrowed MM pointer crosses the scheduling boundary. A competing
 * accepted storage operation is never cancelled. Whole victim scans count as
 * stalls only once per completed scan, not once per bounded cursor batch.
 */
int memory_fault_oom(struct task *task, uint64_t address)
{
    int rc = memory_fault_wait_admit(task);
    if (rc) return rc;
    uint64_t free = mm_free_memory_kib() / 4, reserve = mm_emergency_reserve_pages();
    uint64_t ordinary = free > reserve ? free - reserve : 0;
    /* A failed attempt may have freed its data frame and staged tables while
     * rolling back. Require one more ordinary page than survives rollback;
     * asking for one unconditionally could report success without progress. */
    if (ordinary >= PRESSURE_FAULT_FRAME_TARGET)
        rc = memory_try_reclaim_no_oom(ordinary + 1u);
    else
        rc = memory_try_reclaim_fault((uint32_t)ordinary + 1u);
    if (rc != -EAGAIN) { memory_wait_cancel(task); return rc; }
    return memory_fault_wait_defer(task, address, memory_reclaim_stall_cause());
}

/** @brief Defer an unsubmitted slot read behind an existing storage owner.
 * @param task Current live user task under the outer writer.
 * @param address Faulting byte address for stall accounting.
 * @return -EAGAIN after timed blocking, or terminal negative errno; never
 * cancels or transfers the independently accepted foreign request.
 */
int memory_fault_storage_wait(struct task *task, uint64_t address)
{
    int rc = memory_fault_wait_admit(task);
    return rc ? rc : memory_fault_wait_defer(task, address, PRESSURE_STALL_STORAGE);
}

#ifdef CONFIG_SWAP
/** @brief Detach only an owned headroom or private-fault pressure link.
 * @param task Execution-owned task, nullable. No allocation or scheduling;
 * unrelated pressure and other queues remain linked.
 */
void memory_headroom_unlink(struct task *task)
{
    if(!task)return;
    struct pressure_headroom_state *state=sched_pressure_headroom_state(task);
    if(state && state->wait_owned && task->waiting_queue==&pressure_waiters)
        kernel_wait_queue_remove(&pressure_waiters,task);
    if(state)state->wait_owned=false;
}

/** @brief Bound optional refill waits without using ordinary fault counters.
 * @param task Current live user task under the sole outer writer.
 * @return -EAGAIN after timed blocking or a terminal optional-refill error.
 * A successfully published link belongs to this task's optional refill.
 */
int memory_headroom_wait(struct task *task)
{
    int rc=memory_fault_wait_admit(task);if(rc)return rc;
    struct pressure_headroom_state *state=sched_pressure_headroom_state(task);
    if(!state)return -ESRCH;
    uint64_t now=time_ticks(),free=mm_free_memory_kib()/4;
    if(!state->waits || state->wait_tick!=now) {
        state->wait_tick=now;
        if(state->waits!=UINT32_MAX)++state->waits;
        if(memory_reclaim_stall_cause()==PRESSURE_STALL_STORAGE) {
            if(state->wait_free!=free)state->storage_waits=0;
            state->wait_free=free;
            if(state->storage_waits!=UINT32_MAX)++state->storage_waits;
        } else state->storage_waits=0;
    }
    if(state->waits>PRESSURE_VICTIM_STALL_LIMIT ||
       state->storage_waits>PRESSURE_STORAGE_STALL_LIMIT)return -EBUSY;
    uint64_t deadline=now==UINT64_MAX ? UINT64_MAX : now+1;
    if(kernel_wait_queue_sleep_current_until(&pressure_waiters,deadline))return -EBUSY;
    state->wait_owned=true;
    return -EAGAIN;
}
#endif
