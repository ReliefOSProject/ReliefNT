/* SPDX-License-Identifier: Apache-2.0 */
#include "../user/memory_internal.h"
#include "internal.h"
#include <linux/errno.h>

/** @brief Evacuate a bounded number of candidate leaves for one area.
 * @param area Retained exclusively draining area. @param cursor Scalar cursor.
 * @param budget Maximum candidate and completion units, between 2 and 32.
 * @param work Receives actual charged units. @return One when unpublished,
 * zero for another batch, or terminal errno with the area still published.
 */
int swap_drain_step(struct swap_area *area, struct swap_drain_cursor *cursor,
                    uint32_t budget, uint32_t *work)
{
    if (!work) return -EINVAL;
    *work = 0;
    if (!area || !cursor || !area->published || !area->draining ||
        budget < 2 || budget > 32) return -EINVAL;
    if (!kernel_execution_is_outermost()) return -EDEADLK;
    while (*work < budget) {
        struct task *task = sched_task_at(cursor->task_index);
        if (!task) {
            ++*work;
            int rc = swap_area_finish_drain(area);
            if (!rc) return 1;
            if (rc != -EBUSY || !cursor->pass_progress) return rc;
            uint64_t restored = cursor->restored;
            *cursor = (struct swap_drain_cursor){.restored = restored};
            continue;
        }
        if (!task->pid || task->kind != TASK_KIND_USER ||
            (task->flags & TASK_FLAG_RESOURCES_RELEASED)) {
            ++*work; ++cursor->task_index; cursor->pid = 0;
            continue;
        }
        struct address_space *as = sched_task_as(task);
        uint64_t generation = mm_context_generation_existing(as);
        if (cursor->pid != task->pid || cursor->cr3 != as->cr3 ||
            cursor->generation != generation) {
            cursor->pid = task->pid; cursor->cr3 = as->cr3;
            cursor->generation = generation; cursor->next_page = RELIEFNT_USER_BASE;
        }
        uint64_t page, token;
        ++*work;
        if (!generation || cursor->next_page >= RELIEFNT_USER_TOP ||
            !mm_swap_record_next(as, cursor->next_page, &page, &token)) {
            ++cursor->task_index; cursor->pid = 0;
            continue;
        }
        if ((uint32_t)(token >> 32) != area->serial) {
            cursor->next_page = page + PAGE_SIZE;
            continue;
        }
        /* Charge the synchronous completion separately from inspection. */
        if (*work == budget) return 0;
        ++*work;
        int rc = swap_restore_leaf(task, page);
        if (rc <= 0) return rc ? rc : -EIO;
        if (cursor->restored != UINT64_MAX) ++cursor->restored;
        cursor->pass_progress = true;
        cursor->next_page = page + PAGE_SIZE;
    }
    return 0;
}
