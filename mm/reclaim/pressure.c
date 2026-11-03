/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/lock.h>
#include <reliefnt/memory_pressure.h>
#include <reliefnt/memory_stats.h>
#include <reliefnt/mm.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/lookup_cache.h>
#include <reliefnt/paging.h>
#include <reliefnt/sched.h>
#include <reliefnt/swap.h>
#ifdef CONFIG_SWAP
#include <reliefnt/swap_internal.h>
#endif
#include <linux/errno.h>

/* Focused standalone fixtures may omit the reporting producer. The native
 * kernel links stats.o and its proc consumers unconditionally. */
extern void memory_event_add(enum memory_event, uint64_t) __attribute__((weak));

enum pressure_stage { PRESSURE_CACHE, PRESSURE_SWAP, PRESSURE_OOM };
/* Serialized by the outer execution writer; no borrowed MM pointer persists. */
static struct pressure_cursor foreground_cursor, foreground_no_oom_cursor, background_cursor;
static bool pressure_active, background_active;
static enum pressure_stall_cause last_stall;
static struct {
    struct task *task;
    uint64_t cr3, fault_page, rip, instruction_page, instruction_end, recent[4];
} fault_scope;

/** @brief Protect current and recently resolved pages of one user instruction.
 * @param task Current task pinned by the sole outer writer.
 * @param rip Interrupted instruction address. @param address Faulting byte.
 * Only scalar identities survive until the matching scope end; no pin is added.
 */
void memory_reclaim_fault_scope_begin(struct task *task, uint64_t rip, uint64_t address)
{
    if (!kernel_execution_is_outermost() || !task || task != sched_current_task()) return;
    fault_scope.task = task;
    fault_scope.cr3 = sched_task_as(task)->cr3;
    fault_scope.rip = rip;
    fault_scope.fault_page = address & ~4095ULL;
    fault_scope.instruction_page = rip & ~4095ULL;
    fault_scope.instruction_end = (rip <= UINT64_MAX - 14 ? rip + 14 : rip) & ~4095ULL;
    sched_pressure_fault_pages(task, rip, fault_scope.recent);
}

/** @brief Discard all temporary scalar exclusions before leaving the writer. */
void memory_reclaim_fault_scope_end(void)
{ fault_scope = (__typeof__(fault_scope)){0}; }

#ifdef CONFIG_SWAP
/** @brief Test a candidate against this synchronous fault's scoped pages.
 * @param as Execution-owned candidate root. @param page Aligned user address.
 * @return True when this page is excluded for the active fault transaction.
 */
static bool pressure_fault_excluded(const struct address_space *as, uint64_t page)
{
    return fault_scope.cr3 && as->cr3 == fault_scope.cr3 &&
           (page == fault_scope.fault_page || page == fault_scope.instruction_page ||
            page == fault_scope.instruction_end || page == fault_scope.recent[0] ||
            page == fault_scope.recent[1] || page == fault_scope.recent[2] ||
            page == fault_scope.recent[3]);
}
#endif

/** @brief Observe the last foreground stop while the writer is still held.
 * @return NONE, STORAGE or VICTIM for the serialized foreground attempt.
 */
enum pressure_stall_cause memory_reclaim_stall_cause(void) { return last_stall; }

/** @brief Count ordinary allocatable pages without spending the reserve.
 * @return Free physical pages above the emergency reserve, or zero.
 */
static uint64_t ordinary_free(void)
{
    uint64_t free = mm_free_memory_kib() / 4, reserve = mm_emergency_reserve_pages();
    return free > reserve ? free - reserve : 0;
}

/** @brief Derive requested watermarks from real allocator capacity.
 * @param low Required output for activation. @param high Required stop output.
 * Values count ordinary pages and are capped by the usable physical pool.
 */
static void pressure_watermarks(uint64_t *low, uint64_t *high)
{
    uint64_t total = mm_total_memory_kib() / 4, reserve = mm_emergency_reserve_pages();
    uint64_t available = total > reserve ? total - reserve : 0;
    uint64_t minimum = total / 128;
    if (minimum < 32) minimum = 32;
    if (minimum > 1024) minimum = 1024;
    if (minimum > available) minimum = available;
    *low = minimum > available / 2 ? available : minimum * 2;
    *high = minimum > available / 3 ? available : minimum * 3;
}

/** @brief Check whether all shared cache slots have been scanned since a fault.
 * @param cursor Task-owned scalar attempt state. @return True once a partial
 * initial pass and one complete subsequent pass have both ended. Other cache
 * consumers may advance the shared scanner; retired chunks are handled there.
 */
static bool pressure_cache_cycle_seen(const struct pressure_cursor *cursor)
{
    return !fault_scope.task || (uint64_t)(page_cache_pressure_epoch() - cursor->cache_start) >= 2;
}

/** @brief Run one cooperative cache, swap and optional OOM batch.
 * @param cursor Caller-owned scalar cursor, stable under the outer writer.
 * @param target Required ordinary free-page count. @param flags Reclaim policy.
 * @param budget Maximum inspected candidates/completions. @param allow_oom
 * True only for an admitted foreground fault. @param require_release True if
 * success additionally requires a frame actually released in this batch.
 * @param release_limit Maximum releases when @p released_count is non-NULL.
 * @param released_count Optional output receiving actual released frames.
 * @return Zero on target, -EAGAIN for bounded progress/wait, or terminal
 * errno. No borrowed MM survives return.
 */
static int pressure_batch(struct pressure_cursor *cursor, uint64_t target, uint32_t flags,
                          uint32_t budget, bool allow_oom, bool require_release,
                          uint32_t release_limit, uint32_t *released_count)
{
    bool released = false;
    bool names_scanned = false;
    uint32_t count = 0;
    if (released_count) *released_count = 0;
    if (cursor->flags != flags) *cursor = (struct pressure_cursor){.flags = flags};
    while (budget && (ordinary_free() < target || (require_release && !released))) {
        if (cursor->stage == PRESSURE_CACHE) {
            /* Name snapshots have no backing pins and never perform I/O.
             * Charge every eviction to this batch even when its slab page
             * remains shared with other allocations. The remaining batch
             * budget reaches file pages even while new names keep arriving. */
            uint64_t before_names=ordinary_free();
            uint32_t name_budget=names_scanned ? 0 : budget<8 ? budget : 8;
            if(released_count && name_budget>release_limit-count)name_budget=release_limit-count;
            names_scanned=true;
            uint32_t names=vfs_lookup_cache_reclaim(name_budget);
            if(names) {
                uint64_t after_names=ordinary_free();
                uint64_t released_names=after_names>before_names ? after_names-before_names : 0;
                uint32_t frames=released_names>names ? names : (uint32_t)released_names;
                if(released_count && frames>release_limit-count)frames=release_limit-count;
                if(frames)released=true;
                count+=frames;if(released_count)*released_count=count;
                budget-=names;
                if(released_count && count==release_limit)return -EAGAIN;
                continue;
            }
            uint32_t inspected = 0, candidates = 0; bool complete = false;
            uint32_t slice = budget;
            if (released_count && slice > release_limit - count) slice = release_limit - count;
            if (!slice) return -EAGAIN;
            uint32_t freed = page_cache_pressure_scan_count(slice, &inspected, &complete, &candidates);
            if (inspected > slice || freed > inspected) return -EIO;
            if (memory_event_add) {
                memory_event_add(cursor == &background_cursor ? MEMORY_EVENT_PGSCAN_KSWAPD : MEMORY_EVENT_PGSCAN_DIRECT, candidates);
                memory_event_add(cursor == &background_cursor ? MEMORY_EVENT_PGSTEAL_KSWAPD : MEMORY_EVENT_PGSTEAL_DIRECT, freed);
            }
            if (freed) released = true;
            count += freed;
            if (released_count) *released_count = count;
            if (released_count && count == release_limit) return -EAGAIN;
            budget -= inspected;
            if (complete && pressure_cache_cycle_seen(cursor))
                cursor->stage = (flags & MEMORY_RECLAIM_NOIO) ? PRESSURE_OOM : PRESSURE_SWAP;
            else if (!inspected) return -EAGAIN;
            continue;
        }
        if (cursor->stage == PRESSURE_SWAP) {
#ifdef CONFIG_SWAP
            if (swap_storage_busy()) { last_stall = PRESSURE_STALL_STORAGE; return -EAGAIN; }
            struct task *task = sched_task_at(cursor->task_index);
            if (!task) {
                --budget;
                if (cursor->swap_progress) {
                    cursor->task_index = cursor->pid = 0;
                    cursor->swap_progress = false;
                } else cursor->stage = PRESSURE_OOM;
                continue;
            }
            struct address_space *as = sched_task_as(task);
            struct task *self = sched_current_task();
            if (!task->pid || task->kind != TASK_KIND_USER || task->state == TASK_EXITED ||
                (task->flags & TASK_FLAG_RESOURCES_RELEASED) || !as->cr3 ||
                ((flags & MEMORY_RECLAIM_NO_SELF_MM) && self &&
                 sched_task_as(self)->cr3 == as->cr3)) {
                --budget; ++cursor->task_index; cursor->pid = 0; continue;
            }
            uint64_t generation = mm_context_generation_existing(as);
            if (!generation) {
                int rc = mm_context_ensure_reclaim_generation(as, &generation);
                if (rc) return rc;
            }
            if (cursor->pid != task->pid || cursor->cr3 != as->cr3 ||
                cursor->generation != generation) {
                cursor->pid = task->pid; cursor->cr3 = as->cr3;
                cursor->generation = generation; cursor->page = RELIEFNT_USER_BASE;
            }
            uint64_t page = 0;
            int state = paging_user_pressure_step(as, &cursor->page, &page);
            --budget;
            if (!state) { ++cursor->task_index; cursor->pid = 0; continue; }
            if (state != 2) continue;
            if (memory_event_add) memory_event_add(cursor == &background_cursor ? MEMORY_EVENT_PGSCAN_KSWAPD : MEMORY_EVENT_PGSCAN_DIRECT, 1);
            if (pressure_fault_excluded(as, page)) continue;
            if (!budget) { cursor->page = page; return -EAGAIN; }
            --budget; /* one completed synchronous slot transaction */
            int rc = swap_try_evict(as, page);
            if (rc == -EBUSY || rc == SWAP_STORAGE_CONTENDED) {
                cursor->page = page; last_stall = PRESSURE_STALL_STORAGE; return -EAGAIN;
            }
            if (rc == -ENOSPC) { cursor->stage = PRESSURE_OOM; continue; }
            if (rc < 0) return rc;
            if (rc > 0) {
                if (memory_event_add) memory_event_add(cursor == &background_cursor ? MEMORY_EVENT_PGSTEAL_KSWAPD : MEMORY_EVENT_PGSTEAL_DIRECT, 1);
                cursor->swap_progress = true; released = true;
                ++count;
                if (released_count) *released_count = count;
                if (released_count && count == release_limit) return -EAGAIN;
            }
#else
            --budget; cursor->stage = PRESSURE_OOM;
#endif
            continue;
        }
        if (!pressure_cache_cycle_seen(cursor)) {
            cursor->stage = PRESSURE_CACHE;
            continue;
        }
        if (!allow_oom || (flags & MEMORY_RECLAIM_NOIO)) {
            *cursor = (struct pressure_cursor){.flags = flags};
            return -ENOMEM;
        }
        int oom = memory_oom_advance(budget);
        if (oom == -EINPROGRESS) { last_stall = PRESSURE_STALL_VICTIM; return -EAGAIN; }
        return oom;
    }
    if (ordinary_free() >= target && (!require_release || released)) {
        /* Retain a productive foreground walk so successive one-page faults
         * advance through the MM instead of rescanning the entire cache and
         * address-space prefix before every page.  A complete nonproductive
         * wrap is still required before selecting OOM. */
        if (cursor->stage != PRESSURE_SWAP)
            *cursor = (struct pressure_cursor){.flags = flags};
        return 0;
    }
    return -EAGAIN;
}

/** @brief Execute one shared bounded foreground reclaim batch.
 * @param pages Required ordinary free pages. @param flags Reclaim policy.
 * @param allow_oom Whether this admitted fault may select an OOM victim.
 * @param require_release Whether a free-count rise alone is insufficient.
 * @param supplied Optional caller-owned cursor for a distinct refill policy.
 * @param release_limit Maximum released frames when @p released_count is set.
 * @param released_count Optional actual released-frame output.
 * @return Zero on target, -EAGAIN on bounded progress, or terminal errno.
 * The outer writer serializes each independent cursor; no PMM lock spans I/O.
 */
static int memory_try_reclaim_internal(uint64_t pages, uint32_t flags,
                                       bool allow_oom, bool require_release,
                                       struct pressure_cursor *supplied,
                                       uint32_t release_limit, uint32_t *released_count)
{
    if (released_count) *released_count = 0;
    if (flags & ~(MEMORY_RECLAIM_ATOMIC | MEMORY_RECLAIM_NOIO | MEMORY_RECLAIM_NO_SELF_MM))
        return -EINVAL;
    if (!pages) return 0;
    if (!require_release && ordinary_free() >= pages) return 0;
    if ((flags & MEMORY_RECLAIM_ATOMIC) || !kernel_execution_is_outermost() || pressure_active)
        return -EAGAIN;
    pressure_active = true;
    last_stall = PRESSURE_STALL_NONE;
    uint64_t before = ordinary_free();
    struct pressure_cursor *cursor = supplied ? supplied :
        fault_scope.task ? sched_pressure_cursor(fault_scope.task, allow_oom) :
        allow_oom ? &foreground_cursor : &foreground_no_oom_cursor;
    if (!cursor) { pressure_active = false; return -ESRCH; }
    if (fault_scope.task && (!cursor->cache_attempt || cursor->fault_cr3 != fault_scope.cr3 ||
        cursor->fault_page != fault_scope.fault_page || cursor->fault_rip != fault_scope.rip)) {
        /* A productive swap walk may continue across faults, but before its
         * terminal result this fault still requires its own full cache pass. */
        if (cursor->stage != PRESSURE_SWAP) *cursor = (struct pressure_cursor){.flags = flags};
        cursor->cache_attempt = true;
        cursor->cache_start = page_cache_pressure_epoch();
        cursor->fault_cr3 = fault_scope.cr3;
        cursor->fault_page = fault_scope.fault_page;
        cursor->fault_rip = fault_scope.rip;
    }
    int rc = pressure_batch(cursor, pages, flags, 32, allow_oom, require_release,
                            release_limit, released_count);
    pressure_active = false;
    if (ordinary_free() > before) memory_pressure_notify();
    return rc;
}

/** @brief Try one bounded, nonrecursive batch at an audited writer boundary.
 * @param pages Required ordinary free pages. @param flags Explicit reclaim policy.
 * @return Zero on target, -EAGAIN on deferred progress, -ENOMEM when no
 * victim exists or concurrent changes repeatedly invalidate its snapshot,
 * or another terminal errno; churn can fail even when a victim exists.
 * The outer writer serializes cursor state; PMM locks never enter this function.
 */
int memory_try_reclaim(uint32_t pages, uint32_t flags)
{ return memory_try_reclaim_internal(pages, flags, true, false, NULL, 0, NULL); }

/** @brief Reclaim a real frame for a fault eligible for OOM selection.
 * @param pages Required ordinary free pages. @return Zero only on a real
 * release and target, -EAGAIN on progress, or terminal negative errno.
 * A victim's retirement causes a timed retry of the original allocation. The
 * sole outer writer is held, with no unrelated lock; one call charges at most
 * 32 inspected candidates or completed transactions.
 */
int memory_try_reclaim_fault(uint32_t pages)
{ return pages ? memory_try_reclaim_internal(pages, 0, true, true, NULL, 0, NULL) : -EINVAL; }

/** @brief Reclaim a real frame without OOM selection for an ample-free fault.
 * @param pages Required ordinary free pages, retained as 64-bit.
 * @return Zero only after a real release and target, -EAGAIN for bounded
 * progress, or negative errno. Sole outer writer; at most 32 units per call.
 */
int memory_try_reclaim_no_oom(uint64_t pages)
{ return pages ? memory_try_reclaim_internal(pages, 0, false, true, NULL, 0, NULL) : -EINVAL; }

#ifdef CONFIG_SWAP
/** @brief Advance a caller-owned non-OOM recovery scan with a release limit.
 * @param cursor Persistent scalar scan state. @param pages Ordinary-free target.
 * @param limit Maximum frames to release. @param released Actual release count.
 * @return Zero at target, -EAGAIN on bounded progress, -EBUSY on foreign
 * storage contention, or a terminal errno. Sole outer writer, no storage token.
 */
int memory_reclaim_swapoff_step(struct pressure_cursor *cursor, uint64_t pages,
                                uint32_t limit, uint32_t *released)
{
    if (!cursor || !pages || !limit || !released) return -EINVAL;
    int rc = memory_try_reclaim_internal(pages, 0, false, false, cursor, limit, released);
    return rc == -EAGAIN && last_stall == PRESSURE_STALL_STORAGE ? -EBUSY : rc;
}

/** @brief Snapshot cooldown invalidators after an optional nonproductive scan.
 * @param state Current task's writer-owned headroom state.
 * A completed cache cycle or changed swap capacity/free count permits another
 * attempt, including activation while ordinary memory stays below low.
 */
static void headroom_cooldown(struct pressure_headroom_state *state)
{
    struct swap_stats stats;swap_get_stats(&stats);
    state->cache_epoch=page_cache_pressure_epoch();
    state->swap_total=stats.total;state->swap_free=stats.free;
    state->active=false;state->cooldown=true;
    state->cursor=(struct pressure_cursor){0};
    state->waits=state->storage_waits=0;state->wait_tick=state->wait_free=0;
}

/** @brief Check the IRQ-safe, read-only admission hint for optional refill.
 * @param task Current live task protected by execution ownership.
 * @return True below low unless an unchanged failed scan is cooling down, or
 * when a prior active/cooldown state needs reconciliation at high watermark.
 */
bool memory_headroom_needed(struct task *task)
{
    if(!task || task->kind!=TASK_KIND_USER)return false;
    struct pressure_headroom_state *state=sched_pressure_headroom_state(task);
    if(!state)return false;
    if(state->active || state->fault_pending)return true;
    uint64_t low,high;pressure_watermarks(&low,&high);
    uint64_t free=ordinary_free();
    if(free>=high)return state->cooldown;
    if(!free || free>=low)return false;
    if(!state->cooldown)return true;
    struct swap_stats stats;swap_get_stats(&stats);
    return state->cache_epoch!=page_cache_pressure_epoch() ||
           state->swap_total!=stats.total || state->swap_free!=stats.free;
}

/** @brief Cancel only a task's optional refill state and its queue link.
 * @param task Execution-owned task, nullable. No MM pointer or pin is retained.
 */
void memory_headroom_cancel(struct task *task)
{
    if(!task)return;
    struct pressure_headroom_state *state=sched_pressure_headroom_state(task);
    memory_headroom_unlink(task);
    if(state)*state=(struct pressure_headroom_state){0};
}

/** @brief Discard the old MM's optional and failed-fault state on exec commit.
 * @param task Current execution-owned task, nullable. Only its owned pressure
 * link is removed; independent waits, including on that queue, are untouched.
 * Called without scheduler/unrelated spinlocks; no allocation, I/O or sleep.
 */
void memory_headroom_exec_cancel(struct task *task)
{
    if(!task)return;
    struct pressure_headroom_state *state=sched_pressure_headroom_state(task);
    if(!state)return;
    memory_headroom_unlink(task);
    *state=(struct pressure_headroom_state){0};
}

/** @brief Advance one non-OOM refill batch for an admitted anonymous fault.
 * @param task Current user task under the sole outer writer.
 * @param rip User instruction. @param address Faulting byte.
 * @return -EAGAIN after a timed wait, otherwise zero to attempt the real fault.
 * At most 32 candidates/completions are charged; a terminal optional scan
 * cannot select OOM or turn a valid fault into a signal. Each gap revalidates
 * task, CR3 generation, fault identity, VMA, permissions and PTE before retry.
 */
int memory_headroom_step(struct task *task,uint64_t rip,uint64_t address)
{
    if(!kernel_execution_is_outermost() || !task || task!=sched_current_task() ||
       task->kind!=TASK_KIND_USER || task->state!=TASK_RUNNING)return 0;
    struct pressure_headroom_state *state=sched_pressure_headroom_state(task);
    struct address_space *as=sched_task_as(task);
    if(!state || !as || !as->cr3)return 0;
    uint64_t generation=mm_context_generation_existing(as),page=address&~4095ULL;
    if(state->active && (state->cr3!=as->cr3 || state->generation!=generation ||
                         state->rip!=rip || state->page!=page))
        memory_headroom_cancel(task);
    uint64_t low,high;pressure_watermarks(&low,&high);
    uint64_t free=ordinary_free();
    if(free>=high) {
        state->active=state->cooldown=false;state->cursor.cache_attempt=false;
        state->waits=state->storage_waits=0;state->wait_tick=state->wait_free=0;
        memory_headroom_unlink(task);return 0;
    }
    if(!state->active) {
        if(!free || free>=low || !memory_headroom_needed(task))return 0;
        /* A free-count snapshot does not prove a normal allocation can make
         * progress. Probe once per new refill and release immediately; a
         * failure falls through to the actual fault, and success cannot
         * guarantee later allocation under concurrent memory activity. */
        uint64_t probe=mm_alloc_page();
        if(!probe)return 0;
        mm_free_page(probe);
        if(!generation && mm_context_ensure_reclaim_generation(as,&generation)) {
            headroom_cooldown(state);memory_headroom_unlink(task);return 0;
        }
        state->active=true;state->cooldown=false;
        state->cr3=as->cr3;state->generation=generation;state->rip=rip;state->page=page;
        state->waits=state->storage_waits=0;state->wait_tick=state->wait_free=0;
    }
    memory_reclaim_fault_scope_begin(task,rip,address);
    int rc=memory_try_reclaim_internal(high,0,false,false,&state->cursor,0,NULL);
    memory_reclaim_fault_scope_end();
    if(rc==-EAGAIN && memory_headroom_wait(task)==-EAGAIN)return -EAGAIN;
    if(!rc) {
        state->active=state->cooldown=false;state->cursor.cache_attempt=false;
        state->waits=state->storage_waits=0;state->wait_tick=state->wait_free=0;
    } else headroom_cooldown(state);
    memory_headroom_unlink(task);return 0;
}
#endif

/** @brief Run one service-stack batch when below the low watermark.
 * @param budget_pages Maximum inspected units, capped at 32. The acknowledged
 * user timer return owns the sole writer and excludes the current MM/storage.
 */
void memory_pressure_poll(uint32_t budget_pages)
{
    if (!budget_pages || !kernel_execution_is_outermost() || pressure_active) return;
    struct task *current = sched_current_task();
    if (!current || current->kind != TASK_KIND_USER || current->state != TASK_RUNNING ||
        !current->pid || sched_current_pid() != current->pid) return;
#ifdef CONFIG_SWAP
    if (swap_storage_busy()) return;
#endif
    uint64_t low, high; pressure_watermarks(&low, &high);
    uint64_t free = ordinary_free();
    if (free < low) background_active = true;
    if (free >= high) background_active = false;
    if (!background_active) return;
    if (budget_pages > 32) budget_pages = 32;
    pressure_active = true;
    int rc = pressure_batch(&background_cursor, high, MEMORY_RECLAIM_NO_SELF_MM, budget_pages, false, false, 0, NULL);
    if (rc == -ENOMEM || ordinary_free() >= high) {
        background_active = false;
        background_cursor = (struct pressure_cursor){0};
    }
    pressure_active = false;
    if (ordinary_free() > free) memory_pressure_notify();
}
