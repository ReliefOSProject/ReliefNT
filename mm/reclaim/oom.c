/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/lock.h>
#include <reliefnt/console.h>
#include <reliefnt/memory_pressure.h>
#include <reliefnt/memory_stats.h>
#include <reliefnt/mm.h>
#include <reliefnt/paging.h>
#include <reliefnt/sched.h>
#include <reliefnt/signal.h>
#include <reliefnt/swap.h>
#include <linux/capability.h>
#include <linux/errno.h>

extern void memory_event_add(enum memory_event, uint64_t) __attribute__((weak));

/** @brief Identify tasks that cannot serve as an OOM victim.
 * @param task Execution-owned task, nullable. @return True for protected tasks.
 */
static bool oom_protected(const struct task *task)
{
    if (!task || !task->pid || task->pid == 1 || task->kind != TASK_KIND_USER ||
        (task->flags & TASK_FLAG_SERVICE)) return true;
    int adjustment;
    sched_oom_policy_get(task, &adjustment, NULL);
    return adjustment == -1000;
}

/** @brief Detect vfork participants whose MM lifetime has special ordering.
 * @param task Execution-owned task, nullable. @return True for either participant.
 */
static bool oom_vfork(const struct task *task)
{ return task && (task->vfork_parent || task->vfork_child); }

/** @brief Obtain RAM plus swap capacity used to normalize MM scores.
 * @return Saturated nonzero page count; no memory or area ownership changes.
 */
static uint64_t oom_total_pages(void)
{
    struct swap_stats swap; swap_get_stats(&swap);
    uint64_t total = mm_total_memory_kib() / 4;
    if (swap.total > UINT64_MAX - total) total = UINT64_MAX;
    else total += swap.total;
    const uint64_t limit = 0x7fffffffffffffffULL / 2000;
    return total > limit ? limit : total ? total : 1;
}

/** @brief Compare one root's owned resident and nonresident pages plus adjustment.
 * @param task Execution-owned user task. @param resident Owned RAM leaf count.
 * @return Signed badness including the root's swap and task adjustment.
 */
static int64_t oom_badness(struct task *task, uint64_t resident)
{
    struct address_space *as = sched_task_as(task);
    uint64_t total = oom_total_pages();
    uint64_t pages = resident, swapped = mm_swap_count_existing(as);
    const uint64_t limit = 0x7fffffffffffffffULL / 2;
    pages = pages > limit || swapped > limit - pages ? limit : pages + swapped;
    int adjustment; sched_oom_policy_get(task, &adjustment, NULL);
    int64_t bias = (int64_t)(total / 1000) * adjustment;
    return (int64_t)pages + bias;
}

/** @brief Return a normalized score from owned RAM and swap for one task.
 * @param task User task and MM pinned by the execution owner, nullable.
 * @return Score 0..1000, or -ESRCH for an invalid or released task. This
 * synchronous query walks page tables; the bounded selector does not call it.
 */
int oom_score_for_task(struct task *task)
{
    if (!task || task->kind != TASK_KIND_USER || !task->pid ||
        (task->flags & TASK_FLAG_RESOURCES_RELEASED)) return -ESRCH;
    if (oom_protected(task) || oom_vfork(task)) return 0;
    int64_t badness = oom_badness(task, address_space_user_resident_kib(sched_task_as(task)) / 4);
    if (badness <= 0) return 0;
    uint64_t total = oom_total_pages();
    if ((uint64_t)badness >= total) return 1000;
    return (int)((uint64_t)badness * 1000 / total);
}

/** @brief Compute min(value * 1000 / total, 2000) without wide division.
 * @param value Unsigned magnitude. @param total Nonzero page capacity.
 * @return Truncated ratio, capped at 2000. No allocation or state changes.
 */
static uint32_t oom_scaled_ratio(uint64_t value, uint64_t total)
{
    uint64_t whole = value / total, add = value % total, remainder = 0;
    if (whole >= 2) return 2000;
    uint32_t fraction = 0;
    for (unsigned bit = 512; bit; bit >>= 1) {
        fraction *= 2;
        if (remainder >= total - remainder) { remainder -= total - remainder; ++fraction; }
        else remainder += remainder;
        if (1000u & bit) {
            if (remainder >= total - add) { remainder -= total - add; ++fraction; }
            else remainder += add;
        }
    }
    return (uint32_t)(whole * 1000) + fraction;
}

/** @brief Compute the proc presentation without changing victim selection.
 * @param task Live user task and root pinned by execution ownership, nullable.
 * @return Standard 0..2000 presentation, or -ESRCH for an invalid/released
 * task. Synchronously walks PTEs; never runs under scheduler_lock or in the
 * bounded selector. DEVICE leaves are excluded and swap counts slot owners.
 */
int oom_proc_score_for_task(struct task *task)
{
    if (!task || task->kind != TASK_KIND_USER || !task->pid ||
        (task->flags & TASK_FLAG_RESOURCES_RELEASED)) return -ESRCH;
    if (oom_protected(task) || oom_vfork(task)) return 0;
    struct swap_stats swap; swap_get_stats(&swap);
    uint64_t total = mm_total_memory_kib() / 4;
    total = swap.total > UINT64_MAX - total ? UINT64_MAX : total + swap.total;
    if (!total) return 0;
    struct address_space *as = sched_task_as(task);
    uint64_t pages = address_space_user_resident_kib(as) / 4;
    uint64_t slots = mm_swap_count_existing(as);
    pages = slots > UINT64_MAX - pages ? UINT64_MAX : pages + slots;
    int adjustment; sched_oom_policy_get(task, &adjustment, NULL);
    uint64_t bias = (total / 1000) * (uint32_t)(adjustment < 0 ? -adjustment : adjustment);
    bool negative = adjustment < 0 && bias > pages;
    uint64_t magnitude = negative ? bias - pages : adjustment < 0 ? pages - bias :
                         bias > UINT64_MAX - pages ? UINT64_MAX : pages + bias;
    int32_t ratio = (int32_t)oom_scaled_ratio(magnitude, total);
    int32_t score = 1000 + (negative ? -ratio : ratio);
    if (score <= 0) return 0;
    if (score >= 3000) return 2000;
    return score * 2 / 3;
}

/** @brief Read the target process's inherited OOM adjustment.
 * @param task User task pinned by execution ownership, nullable.
 * @return Adjustment -1000..1000 or -ESRCH for an invalid target.
 */
int oom_adj_get(struct task *task)
{
    if (!task || !task->pid || task->kind != TASK_KIND_USER) return -ESRCH;
    int adjustment; sched_oom_policy_get(task, &adjustment, NULL);
    return adjustment;
}

/** @brief Update an authorized target and applicable MM/group sharers.
 * @param task Target live user task. @param value Adjustment -1000..1000.
 * @param caller Capability-bearing task; both tasks remain execution-pinned.
 * @return Zero or -EINVAL/-ESRCH/-EDEADLK/-EACCES. Sole outer writer required;
 * no task reference transfers and no memory is allocated.
 */
int oom_adj_set(struct task *task, int value, struct task *caller)
{
    if (value < -1000 || value > 1000) return -EINVAL;
    if (!task || !task->pid || task->kind != TASK_KIND_USER || !caller) return -ESRCH;
    if (!kernel_execution_is_outermost()) return -EDEADLK;
    int minimum; sched_oom_policy_get(task, NULL, &minimum);
    bool privileged = (caller->cap_effective & (1ULL << CAP_SYS_RESOURCE)) != 0;
    if (value < minimum && !privileged) return -EACCES;
    uint32_t group = sched_task_tgid(task);
    uint64_t cr3 = sched_task_as(task)->cr3;
    bool share = !task->vfork_parent && cr3 != 0;
    for (uint32_t i = 0;; ++i) {
        struct task *member = sched_task_at(i);
        if (!member) break;
        bool same_group = sched_task_tgid(member) == group;
        bool same_mm = share && member->kind == TASK_KIND_USER && member->pid != 1 &&
                       !member->vfork_parent && sched_task_as(member)->cr3 == cr3;
        if (member != task && !same_group && !same_mm) continue;
        int old_min; sched_oom_policy_get(member, NULL, &old_min);
        sched_oom_policy_set(member, value, privileged ? value : old_min);
    }
    return 0;
}

enum oom_phase { OOM_PICK, OOM_COUNT, OOM_VERIFY, OOM_FINAL, OOM_SIGNAL, OOM_WAIT };
/* Repeated snapshot invalidation may end one fault with standard ENOMEM.
 * Advancing an unchanged cursor, however large the MM, consumes no retries. */
#define OOM_SNAPSHOT_RESTART_LIMIT 8u
struct oom_cursor {
    enum oom_phase phase;
    uint32_t next, member, membership_restarts, device_restarts;
    struct task_identity *candidate, *best;
    uint64_t candidate_cr3, candidate_generation, best_cr3, best_generation;
    uint64_t count_cursor, device_pages, device_revision;
    int64_t candidate_score, best_score;
    uint64_t victim_epoch, membership_revision;
    bool denied, sent, reported, wait_live, wait_new, abandon, revision_valid;
};
/* Every reference survives scheduling as a PID identity, never a task pointer. */
static struct oom_cursor oom;
static uint64_t oom_epoch;

/** @brief Test that a task still owns the same incarnation of a root.
 * @param task Execution-pinned task, nullable. @param cr3 Physical root identity.
 * @param generation Expected private incarnation. @return True on a live match.
 */
static bool oom_same_root(const struct task *task, uint64_t cr3, uint64_t generation)
{
    return task && !(task->flags & TASK_FLAG_RESOURCES_RELEASED) &&
           sched_task_as(task)->cr3 == cr3 &&
           mm_context_generation_existing(sched_task_as(task)) == generation;
}

/** @brief Release a discarded candidate and continue at the next task slot. */
static void oom_candidate_done(void)
{
    sched_task_identity_put(oom.candidate); oom.candidate = NULL;
    ++oom.next; oom.member = 0; oom.denied = false; oom.phase = OOM_PICK;
}

/** @brief Drop retained identities and start a fresh selection. */
static void oom_reset(void)
{
    sched_task_identity_put(oom.candidate);
    sched_task_identity_put(oom.best);
    oom = (struct oom_cursor){0};
}

/** @brief Begin a bounded revalidation of every holder of the selected MM.
 * The best PID identity is retained, while the root incarnation is recorded
 * as scalars. After publication, the representative may leave while peers
 * retain the MM; the cursor does not own a task or address-space pointer.
 */
static void oom_begin_final(void)
{
    oom.phase = OOM_FINAL; oom.member = 0; oom.denied = false;
    oom.membership_revision = sched_oom_membership_revision();
}

/** @brief Stop signalling an unsafe MM and wait for already published victims.
 * The existing nonzero victim epoch and retained best identity remain owned.
 */
static void oom_abandon_published(void)
{
    oom.abandon = true; oom.phase = OOM_WAIT; oom.member = 0;
    oom.wait_live = oom.wait_new = false;
    oom.membership_revision = sched_oom_membership_revision();
}

/** @brief Advance OOM selection, publication, or real resource-retirement wait.
 * @param budget Maximum task slots or bounded PTE steps, capped at 32.
 * @return -EAGAIN for another batch, -EINPROGRESS for a live victim, -ENOMEM
 * for no victim or repeated snapshot invalidation, or another terminal errno.
 * Eight membership or per-candidate DEVICE restarts bound concurrent churn;
 * ordinary cursor progress does not consume this allowance. After publication,
 * bounded failure retains the root/epoch for safe revalidation and retirement.
 * Caller holds the sole outer writer; only PID and scalar MM identities persist.
 */
int memory_oom_advance(uint32_t budget)
{
    if (!kernel_execution_is_outermost()) return -EDEADLK;
    if (budget > 32) budget = 32;
    while (budget) {
        uint64_t membership = sched_oom_membership_revision();
        if (!oom.revision_valid) {
            oom.membership_revision = membership; oom.revision_valid = true;
        } else if (oom.membership_revision != membership) {
            if (++oom.membership_restarts >= OOM_SNAPSHOT_RESTART_LIMIT) {
                if (oom.sent) {
                    /* The caller may fail, but unmarked peers still belong to
                     * this published root. Resume from a fresh snapshot; only
                     * an actually unsafe holder warrants abandonment. */
                    if (oom.phase == OOM_WAIT) {
                        oom.member = 0; oom.wait_live = oom.wait_new = false;
                        oom.membership_revision = membership;
                    } else oom_begin_final();
                    oom.membership_restarts = 0;
                } else oom_reset();
                return -ENOMEM;
            }
            if (!oom.sent) {
                uint32_t restarts = oom.membership_restarts;
                oom_reset(); oom.membership_restarts = restarts;
                oom.membership_revision = membership; oom.revision_valid = true;
            } else if (oom.phase == OOM_WAIT) {
                oom.member = 0; oom.wait_live = oom.wait_new = false;
                oom.membership_revision = membership;
            } else oom_begin_final();
        }
        if (oom.phase == OOM_PICK) {
            struct task *task = sched_task_at(oom.next); --budget;
            if (!task) {
                if (!oom.best) { oom_reset(); return -ENOMEM; }
                oom_begin_final(); continue;
            }
            if (oom_protected(task) || oom_vfork(task) || task->state == TASK_EXITED ||
                (task->flags & TASK_FLAG_RESOURCES_RELEASED) || !sched_task_as(task)->cr3) {
                ++oom.next; continue;
            }
            uint64_t generation = mm_context_generation_existing(sched_task_as(task));
            if (!generation || sched_task_identity_get_existing(task, &oom.candidate)) {
                ++oom.next; continue;
            }
            oom.candidate_cr3 = sched_task_as(task)->cr3;
            oom.candidate_generation = generation;
            oom.count_cursor = RELIEFNT_USER_BASE; oom.device_pages = 0;
            oom.device_restarts = 0;
            oom.device_revision = mm_context_device_revision(sched_task_as(task));
            oom.member = 0; oom.denied = false; oom.phase = OOM_COUNT;
            continue;
        }
        if (oom.phase == OOM_COUNT) {
            struct task *candidate = sched_task_identity_task(oom.candidate);
            if (!oom_same_root(candidate, oom.candidate_cr3, oom.candidate_generation) ||
                oom_protected(candidate) || oom_vfork(candidate) || candidate->state == TASK_EXITED) {
                oom_candidate_done(); continue;
            }
            struct address_space *as = sched_task_as(candidate);
            uint64_t revision = mm_context_device_revision(as);
            if (revision != oom.device_revision) {
                if (++oom.device_restarts >= OOM_SNAPSHOT_RESTART_LIMIT) {
                    oom_reset(); return -ENOMEM;
                }
                oom.count_cursor = RELIEFNT_USER_BASE; oom.device_pages = 0;
                oom.device_revision = revision;
            }
            uint64_t page = 0;
            int state = paging_user_pressure_step(as, &oom.count_cursor, &page);
            --budget;
            if (!state) {
                uint64_t total = as->user_page_count;
                uint64_t resident = total >= oom.device_pages ? total - oom.device_pages : 0;
                oom.candidate_score = oom_badness(candidate, resident);
                oom.phase = OOM_VERIFY;
            } else if (state == 2 &&
                       (address_space_user_page_flags(as, page) & RELIEFNT_PAGE_DEVICE)) {
                ++oom.device_pages;
            }
            continue;
        }
        if (oom.phase == OOM_VERIFY || oom.phase == OOM_FINAL) {
            bool final = oom.phase == OOM_FINAL;
            struct task_identity *identity = final ? oom.best : oom.candidate;
            uint64_t cr3 = final ? oom.best_cr3 : oom.candidate_cr3;
            uint64_t generation = final ? oom.best_generation : oom.candidate_generation;
            struct task *candidate = sched_task_identity_task(identity);
            /* Before publication, the selected task proves eligibility. Once
             * published, root/generation plus a fresh holder scan do so: the
             * representative may already have released its MM reference. */
            if ((!final || !oom.sent) &&
                (!oom_same_root(candidate, cr3, generation) || oom_protected(candidate) ||
                 oom_vfork(candidate) || candidate->state == TASK_EXITED)) {
                if (final) { oom_reset(); return -EAGAIN; }
                oom_candidate_done(); continue;
            }
            struct task *member = sched_task_at(oom.member); --budget;
            if (member) {
                ++oom.member;
                if (oom_same_root(member, cr3, generation) &&
                    (oom_vfork(member) || oom_protected(member)))
                    oom.denied = true;
                continue;
            }
            if (final) {
                if (oom.denied && oom.sent) { oom_abandon_published(); continue; }
                if (oom.denied) { oom_reset(); return -EAGAIN; }
                if (!oom.victim_epoch) {
                    oom.victim_epoch = ++oom_epoch;
                    if (!oom.victim_epoch) oom.victim_epoch = ++oom_epoch;
                }
                oom.phase = OOM_SIGNAL; oom.member = 0; continue;
            }
            if (!oom.denied && (!oom.best || oom.candidate_score > oom.best_score)) {
                sched_task_identity_put(oom.best); oom.best = oom.candidate; oom.candidate = NULL;
                oom.best_cr3 = cr3; oom.best_generation = generation; oom.best_score = oom.candidate_score;
            }
            oom_candidate_done();
            continue;
        }
        struct task *member = sched_task_at(oom.member); --budget;
        if (oom.phase == OOM_SIGNAL) {
            if (!member) {
                if (!oom.sent) { oom_reset(); return -EAGAIN; }
                oom.phase = OOM_WAIT; oom.member = 0;
                oom.wait_live = oom.wait_new = false; continue;
            }
            ++oom.member;
            if (!oom_same_root(member, oom.best_cr3, oom.best_generation)) continue;
            if (oom_vfork(member) || oom_protected(member)) {
                if (!oom.sent) { oom_reset(); return -EAGAIN; }
                oom_abandon_published(); continue;
            }
            if (member->state == TASK_EXITED ||
                sched_oom_victim_marked(member, oom.victim_epoch)) continue;
            uint64_t *pending = sched_task_process_pending(member);
            if (!(__atomic_load_n(pending, __ATOMIC_SEQ_CST) & (1ULL << 8))) {
                int rc = kernel_signal_enqueue(member, true, 9, NULL);
                if (rc) {
                    if (!oom.sent) { oom_reset(); return rc; }
                    oom_abandon_published(); return -EAGAIN;
                }
                if (!oom.reported) {
                    if (memory_event_add) memory_event_add(MEMORY_EVENT_OOM_KILL, 1);
                    console_warn("[memory] oom kill signal published pid=%u\n", member->pid);
                    oom.reported = true;
                }
            }
            sched_oom_victim_mark(member, oom.victim_epoch);
            oom.sent = true;
            sched_wake_fatal(member);
            continue;
        }
        /* The signal itself never counts as reclaimed RAM. */
        if (member) {
            ++oom.member;
            if (!oom_same_root(member, oom.best_cr3, oom.best_generation)) continue;
            if (oom_vfork(member) || oom_protected(member)) oom.abandon = true;
            /* A process SIGKILL can exit same-group siblings before this scan
             * marked them. Even on abandonment, their CPU/MM must retire. */
            if (sched_oom_victim_marked(member, oom.victim_epoch) ||
                member->state == TASK_EXITED) oom.wait_live = true;
            else if (!oom.abandon) oom.wait_new = true;
            continue;
        }
        if (!oom.abandon && oom.wait_new) {
            oom.wait_live = oom.wait_new = false; oom_begin_final(); continue;
        }
        if (!oom.wait_live) { oom_reset(); return -EAGAIN; }
        oom.member = 0; oom.wait_live = oom.wait_new = false; return -EINPROGRESS;
    }
    return -EAGAIN;
}
