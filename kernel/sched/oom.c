/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

static uint64_t oom_membership_revision;

/** @brief Invalidate bounded OOM membership validation after publication.
 * This atomic notification is safe under scheduler_lock and allocates nothing.
 */
void sched_oom_membership_changed(void)
{
    __atomic_add_fetch(&oom_membership_revision, 1, __ATOMIC_RELAXED);
}

/** @brief Snapshot the scheduler's OOM membership and protection revision.
 * @return Atomic scalar revision; no task or identity is retained.
 */
uint64_t sched_oom_membership_revision(void)
{
    return __atomic_load_n(&oom_membership_revision, __ATOMIC_RELAXED);
}

/** @brief Read an already allocated task object's OOM adjustment and minimum.
 * @param task Execution-owned task, nullable. @param adjustment Optional output.
 * @param minimum Optional minimum output. No allocation or ownership transfer.
 */
void sched_oom_policy_get(const struct task *task, int *adjustment, int *minimum)
{
    const struct sched_task_object *object = (const struct sched_task_object *)task;
    if (adjustment) *adjustment = object ? object->oom_adjustment : 0;
    if (minimum) *minimum = object ? object->oom_minimum : 0;
}

/** @brief Set a published task object's policy while the execution writer owns it.
 * @param task Execution-owned task, nullable. @param adjustment New value.
 * @param minimum New permitted minimum. A change invalidates bounded OOM holder
 * validation; no allocation, extra lock, or ownership transfer occurs.
 */
void sched_oom_policy_set(struct task *task, int adjustment, int minimum)
{
    struct sched_task_object *object = (struct sched_task_object *)task;
    if (!object) return;
    bool changed = object->oom_adjustment != adjustment || object->oom_minimum != minimum;
    object->oom_adjustment = adjustment; object->oom_minimum = minimum;
    if (changed) sched_oom_membership_changed();
}

/** @brief Record an already published OOM victim on its private task object.
 * @param task Execution-owned task, nullable. @param epoch Nonzero selection ID.
 * No allocation, scheduler lock, signal or reference transfer occurs.
 */
void sched_oom_victim_mark(struct task *task, uint64_t epoch)
{
    struct sched_task_object *object = (struct sched_task_object *)task;
    if (object) object->oom_victim_epoch = epoch;
}

/** @brief Test a private OOM-victim mark while the task is pinned.
 * @param task Execution-owned task, nullable. @param epoch Selection ID.
 * @return True when the nonzero ID matches this task object's mark.
 */
bool sched_oom_victim_marked(const struct task *task, uint64_t epoch)
{
    const struct sched_task_object *object = (const struct sched_task_object *)task;
    return object && epoch && object->oom_victim_epoch == epoch;
}

/** @brief Advance one per-task repeated pressure-stall observation.
 * @param task Execution-owned task, nullable. @param address Faulting byte.
 * @param free_pages Free-page snapshot. @param cause Current nonzero stall kind.
 * @param tick Scheduler tick; at most one observation counts per distinct tick.
 * @return Consecutive observations, reset when address, count or cause changes.
 */
uint32_t sched_pressure_stall(struct task *task, uint64_t address,
                              uint64_t free_pages, uint32_t cause, uint64_t tick)
{
    struct sched_task_object *object = (struct sched_task_object *)task;
    if (!object) return 0;
    if (object->pressure_address != address || object->pressure_free != free_pages ||
        object->pressure_cause != cause) {
        object->pressure_address = address; object->pressure_free = free_pages;
        object->pressure_cause = cause; object->pressure_stalls = 0;
    }
    if (cause && (!object->pressure_stalls || object->pressure_stall_tick != tick)) {
        object->pressure_stall_tick = tick;
        if (object->pressure_stalls != UINT32_MAX) ++object->pressure_stalls;
    }
    return object->pressure_stalls;
}

/** @brief Forget a completed or cancelled fault's pressure stall state.
 * @param task Execution-owned task, nullable. No scheduling or allocation.
 */
void sched_pressure_reset(struct task *task)
{
    struct sched_task_object *object = (struct sched_task_object *)task;
    if (!object) return;
    object->pressure_address = object->pressure_free = 0;
    object->pressure_stall_tick = 0;
    object->pressure_cause = object->pressure_stalls = 0;
    object->pressure_fault_cursor[0].cache_attempt = false;
    object->pressure_fault_cursor[1].cache_attempt = false;
}

/** @brief Borrow the task's policy-specific fault progress until writer exit.
 * @param task Current live task held by the sole execution writer.
 * @param allow_oom Select the OOM-capable cursor. @return Borrowed cursor or
 * NULL for a NULL task; no allocation, task reference or scheduling occurs.
 */
struct pressure_cursor *sched_pressure_cursor(struct task *task, bool allow_oom)
{
    struct sched_task_object *object = (struct sched_task_object *)task;
    return object ? &object->pressure_fault_cursor[allow_oom ? 1 : 0] : NULL;
}

#ifdef CONFIG_SWAP
/** @brief Borrow private optional refill state while execution pins the task.
 * @param task Live task, nullable. @return Borrowed state or NULL.
 */
struct pressure_headroom_state *sched_pressure_headroom_state(struct task *task)
{
    struct sched_task_object *object=(struct sched_task_object *)task;
    return object ? &object->pressure_headroom : NULL;
}
#endif

/** @brief Remember bounded recent fault pages for the same root/instruction.
 * @param task Current task pinned by execution ownership.
 * @param rip Interrupted instruction. @param address Resolved byte address.
 * The four-page history contains scalar identities and acquires no page pins.
 */
void sched_pressure_fault_resolved(struct task *task, uint64_t rip, uint64_t address)
{
    struct sched_task_object *object = (struct sched_task_object *)task;
    if (!object) return;
    object->event_fault_active = false;
    uint64_t cr3 = sched_task_as(task)->cr3, page = address & ~4095ULL;
    if (object->pressure_fault_cr3 != cr3 || object->pressure_fault_rip != rip) {
        object->pressure_fault_cr3 = cr3; object->pressure_fault_rip = rip;
        for (unsigned i = 0; i < 4; ++i) object->pressure_fault_pages[i] = 0;
    }
    if (object->pressure_fault_pages[0] != page) {
        unsigned last = 3;
        for (unsigned i = 1; i < 4; ++i)
            if (object->pressure_fault_pages[i] == page) { last = i; break; }
        for (unsigned i = last; i; --i)
            object->pressure_fault_pages[i] = object->pressure_fault_pages[i - 1];
        object->pressure_fault_pages[0] = page;
    }
}

/** @brief Admit one logical user fault; identical pressure retries stay one.
 * @param task Current execution-owned task. @param rip User instruction.
 * @param address Faulting byte. @return True when a new fault is admitted.
 */
bool sched_fault_event_begin(struct task *task, uint64_t rip, uint64_t address)
{
    struct sched_task_object *object = (struct sched_task_object *)task;
    if (!object) return false;
    uint64_t cr3 = sched_task_as(task)->cr3, page = address & ~4095ULL;
    bool first = !object->event_fault_active || object->event_fault_cr3 != cr3 ||
                 object->event_fault_rip != rip || object->event_fault_page != page;
    if (first) object->event_fault_major = false;
    object->event_fault_active = true;
    object->event_fault_cr3 = cr3; object->event_fault_rip = rip; object->event_fault_page = page;
    return first;
}

/** @brief Record the first backing read associated with one logical fault.
 * @param task Current execution-owned task. @return True on first completed read.
 */
bool sched_fault_event_major(struct task *task)
{
    struct sched_task_object *object=(struct sched_task_object *)task;
    if (!object || !object->event_fault_active || object->event_fault_major) return false;
    object->event_fault_major=true; return true;
}

/** @brief Retire the scalar accounting identity of a completed user fault.
 * @param task Current execution-owned task, nullable.
 */
void sched_fault_event_end(struct task *task)
{ if (task) ((struct sched_task_object *)task)->event_fault_active = false; }

/** @brief Snapshot matching recent fault pages without retaining the task.
 * @param task Execution-owned task, nullable. @param rip Interrupted instruction.
 * @param pages Optional output array of four page addresses, newest first.
 */
void sched_pressure_fault_pages(const struct task *task, uint64_t rip,
                                uint64_t pages[4])
{
    const struct sched_task_object *object = (const struct sched_task_object *)task;
    bool match = object && object->pressure_fault_cr3 == sched_task_as(task)->cr3 &&
                 object->pressure_fault_rip == rip;
    if (pages) for (unsigned i = 0; i < 4; ++i)
        pages[i] = match ? object->pressure_fault_pages[i] : 0;
}
