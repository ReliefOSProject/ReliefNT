/* Memory locking is virtual-range policy, shared by threads using one MM. */
#include "memory_internal.h"
#include <reliefnt/swap.h>

/* Bound slot inspections and page attempts per IRQ-masked execution batch. */
#define MM_LOCK_POPULATE_BATCH 32u

/** @brief Translate a checked prefault error to the Linux mlock result.
 * @param error Negative fault/provider status. @return Negative syscall errno.
 */
static int mm_lock_population_error(int error)
{
    if (error == SWAP_STORAGE_CONTENDED) return KERNEL_SYSCALL_SWAP_CONTENDED;
    return error == -RELIEFOS_ENOMEM ? -RELIEFOS_EAGAIN : -RELIEFOS_ENOMEM;
}

/** @brief Resolve a swap leaf at the checked walker's outer-writer boundary.
 * @param task Current execution-owned task. @param page Aligned page.
 * @return Zero for resident/empty or resolved swap, otherwise a terminal error
 * or SWAP_STORAGE_CONTENDED; no spin/loader lock is held and nothing queues.
 */
static int mm_lock_prepare_swap(struct task *task, uint64_t page)
{
    if (paging_user_entry_kind(sched_task_as(task), page) != PAGING_USER_ENTRY_SWAP) return 0;
    int ret = swap_resolve_fault(task, page, 4u);
    return ret > 0 ? 0 : ret ? ret : -RELIEFOS_EIO;
}

/** @brief Fault pages and inspect VMA slots in bounded transactions.
 * @param start First page. @param end Exclusive end. @param checked Require
 * every visited page to succeed; otherwise population is best effort.
 * @param require_locked Only visit locked eager-policy VMAs when true.
 * @return Zero or an mlock errno. The running CPU reserves its current task
 * while in ring 0. Only its numeric PID and the root/generation identity are
 * carried over a suspension: on resume the current task and its VMA/PTE are
 * resolved afresh. Best-effort iteration uses slot order; checked iteration
 * searches for the next required address and errors on a hole. If an active
 * slot changes its bounds, best effort restarts its scan and checked mode
 * searches the current page again. No spinlock spans suspend/resume.
 */
static int mm_populate_lock_walk(uint64_t start, uint64_t end, bool checked,
                                 bool require_locked)
{
    struct task *task = sched_current_task();
    struct mm_identity identity;
    if (!task || mm_context_identity(sched_task_as(task), &identity) < 0)
        return checked ? -RELIEFOS_EAGAIN : 0;
    uint32_t pid = task->pid;
    uint64_t revision = mm_context_vma_revision(sched_task_as(task));
    uint64_t cursor = start, active_start = 0, active_end = 0;
    uint32_t slot = 0;
    bool active = false;
    unsigned batch = 0;
    for (;;) {
        if (checked && cursor >= end) return 0;
        if (batch >= MM_LOCK_POPULATE_BATCH) {
            batch = 0;
            struct kernel_execution_suspend_token token = {0};
            task = NULL;
            if (kernel_execution_suspend_irqrestore(&token) < 0)
                return checked ? -RELIEFOS_EAGAIN : 0;
            if (kernel_execution_resume_irqsave(&token) < 0)
                panic("memory lock population could not resume execution");
            task = sched_current_task();
            struct mm_identity resumed;
            if (!task || task->pid != pid || task->state == TASK_EXITED ||
                sched_task_as(task)->cr3 != identity.cr3 ||
                mm_context_identity(sched_task_as(task), &resumed) < 0 ||
                resumed.generation != identity.generation)
                return checked ? -RELIEFOS_ENOMEM : 0;
            uint64_t current_revision = mm_context_vma_revision(sched_task_as(task));
            if (current_revision != revision) {
                /* A split may have published an unvisited slice behind us. */
                revision = current_revision;
                active = false;
                slot = 0;
                /* Before admission, a peer can also discard a visited PTE. */
                if (!require_locked) cursor = start;
            }
        }
        if (slot >= sched_task_vma_capacity(task)) {
            if (checked && !require_locked) {
                /* The full slot search ruled out an ordinary VMA. */
                ++batch;
                int ret = mm_lock_prepare_swap(task, cursor);
                if (!ret) {
                    revision = mm_context_vma_revision(sched_task_as(task));
                    ret = task_populate_lock_stack_page(task, cursor);
                }
                if (ret < 0) return mm_lock_population_error(ret);
                cursor += PAGE_SIZE;
                slot = 0;
                continue;
            }
            return checked ? -RELIEFOS_ENOMEM : 0;
        }
        struct task_vma *vma = sched_task_vma_at(task, slot);
        ++batch;
        if (active && (!vma || !vma->used || vma->start != active_start ||
                       vma->end != active_end)) {
            /* Splits may move an unvisited slice to an earlier slot. */
            active = false;
            slot = 0;
            if (!require_locked) cursor = start;
            continue;
        }
        if (!active) {
            if (!vma || !vma->used ||
                (checked ? (cursor < vma->start || cursor >= vma->end) :
                           (vma->end <= start || vma->start >= end))) {
                ++slot;
                continue;
            }
            if (!checked) cursor = vma->start > start ? vma->start : start;
            active_start = vma->start;
            active_end = vma->end;
            active = true;
        }
        if ((require_locked && (!(vma->flags & TASK_VMA_FLAG_LOCKED) ||
            (vma->flags & TASK_VMA_FLAG_LOCKONFAULT))) || vma->prot == LINUX_PROT_NONE) {
            if (checked) return -RELIEFOS_ENOMEM;
            active = false;
            ++slot;
            continue;
        }
        int ret = mm_lock_prepare_swap(task, cursor);
        if (!ret) {
            revision = mm_context_vma_revision(sched_task_as(task));
            ret = task_populate_lock_vma_page(task, vma, cursor);
        }
        if (ret < 0) {
            if (checked) return mm_lock_population_error(ret);
            /* A local failure does not suppress other independent VMAs. */
            active = false;
            ++slot;
            continue;
        }
        cursor += PAGE_SIZE;
        if (cursor >= end || cursor >= active_end) {
            active = false;
            if (checked) slot = 0;
            else ++slot;
        }
    }
}

/** @brief Populate an admitted locked range in checked or best-effort mode.
 * @param start First page. @param end Exclusive end. @param checked Require
 * every page to succeed. @return Zero or a translated checked mlock error.
 */
int mm_populate_locked_range(uint64_t start, uint64_t end, bool checked)
{
    return mm_populate_lock_walk(start, end, checked, true);
}

/** @brief Count lock-marked VMA bytes in an execution-owned MM.
 * @param task Borrowed user task; NULL has no locked bytes.
 * @return Number of distinct locked virtual bytes.
 */
uint64_t mm_locked_bytes(struct task *task)
{
    uint64_t total = 0;
    if (!task) return 0;
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        const struct task_vma *vma = sched_task_vma_at(task, i);
        if (vma && vma->used && (vma->flags & TASK_VMA_FLAG_LOCKED))
            total += vma->end - vma->start;
    }
    return total;
}

/** @brief Admit Linux lock operations by effective capability and memlock limit.
 * @param task Borrowed current user task. @return True when locking is permitted.
 */
static bool mm_can_lock(const struct task *task)
{
    return task && ((task->cap_effective & (1ULL << CAP_IPC_LOCK)) ||
                    sched_task_limits(task)->memlock.rlim_cur != 0);
}

/** @brief Retain a copied VMA slice's independent provider and backing owners.
 * @param dst Copied unowned VMA. @param src Original execution-owned VMA.
 * @return Zero or negative errno; failed destination owns no references.
 */
static int mm_lock_retain_slice(struct task_vma *dst, const struct task_vma *src)
{
    dst->mapping_file = NULL;
    dst->exec_source = NULL;
    dst->inode = NULL;
    dst->device_mapping = NULL;
    dst->sysv_shm_attachment = NULL;
    int ret = sched_task_vma_retain_file(dst, src);
    if (ret < 0) return ret;
    if (src->device_mapping) {
        ret = device_mapping_get(src->device_mapping);
        if (ret < 0) { task_vma_clear(dst); return ret; }
        dst->device_mapping = src->device_mapping;
    }
    dst->inode = src->inode;
    storage_inode_retain(dst->inode);
    return 0;
}

/** @brief Adjust one copied slice's offset to its new first address.
 * @param slice Copied VMA to adjust. @param original Original VMA bounds.
 */
static void mm_lock_slice_offset(struct task_vma *slice, const struct task_vma *original)
{
    uint64_t delta = slice->start - original->start;
    if (slice->flags & TASK_VMA_FLAG_FILE) slice->file_offset += delta;
    if (slice->device_mapping) {
        slice->device_map_offset = original->device_map_offset + delta;
        slice->device_physical = device_mapping_physical(slice->device_mapping,
                                                          slice->device_map_offset);
    }
}

/** @brief Release any retained but unpublished endpoint slices.
 * @param slices Two staged slots. @param count Number with owned references.
 */
static void mm_lock_drop_slices(struct task_vma slices[2], unsigned count)
{
    for (unsigned i = 0; i < count; ++i) task_vma_clear(&slices[i]);
}

/** @brief Retain every fallible owner for the two possible boundary splits.
 * @param task Execution-owned MM. @param start Inclusive covered page.
 * @param end Exclusive covered page. @param slices Two caller-owned slots.
 * @param count Number of retained slices, returned even on failure.
 * @return Zero or negative ENOMEM without changing any live VMA policy.
 * An interval can split at most its first and last VMA. SysV attachment pins
 * are infallible and are taken only when each staged slice is published.
 */
static int mm_lock_prepare_slices(struct task *task, uint64_t start, uint64_t end,
                                  struct task_vma slices[2], unsigned *count)
{
    *count = 0;
    struct task_vma *first = task_vma_containing(task, start, start + PAGE_SIZE);
    struct task_vma *last = task_vma_containing(task, end - PAGE_SIZE, end);
    if (first && first->start < start) {
        slices[*count] = *first;
        if (mm_lock_retain_slice(&slices[*count], first) < 0) return -RELIEFOS_ENOMEM;
        slices[*count].end = start;
        ++*count;
    }
    if (last && last->end > end) {
        slices[*count] = *last;
        if (mm_lock_retain_slice(&slices[*count], last) < 0) return -RELIEFOS_ENOMEM;
        slices[*count].start = end;
        mm_lock_slice_offset(&slices[*count], last);
        ++*count;
    }
    return 0;
}

/** @brief Lock new implicit-stack growth before publishing its lower bound.
 * @param task Execution-owned user task. @param low Faulting new low page.
 * @param old_low Previously admitted low page. @param mode Future policy bits.
 * @return Zero or negative ENOMEM; new metadata and pages roll back on failure.
 */
int mm_lock_future_stack(struct task *task, uint64_t low, uint64_t old_low, uint32_t mode)
{
    uint64_t length = old_low - low;
    if (!(task->cap_effective & (1ULL << CAP_IPC_LOCK))) {
        uint64_t limit = sched_task_limits(task)->memlock.rlim_cur & ~(PAGE_SIZE - 1ULL);
        uint64_t held = mm_locked_bytes(task);
        if (held > limit || length > limit - held) return -RELIEFOS_ENOMEM;
    }
    if (task_vma_overlaps(task, low, old_low)) return -RELIEFOS_ENOMEM;
    struct task_vma *vma = task_vma_free_slot(task);
    if (!vma) return -RELIEFOS_ENOMEM;
    mm_context_vma_changed(sched_task_as(task));
    *vma = (struct task_vma){.used = 1, .start = low, .end = old_low,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .max_prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_LOCKED |
                 ((mode & LINUX_MCL_ONFAULT) ? TASK_VMA_FLAG_LOCKONFAULT : 0)};
    uint64_t populate_end = (mode & LINUX_MCL_ONFAULT) ? low + PAGE_SIZE : old_low;
    for (uint64_t page = low; page < populate_end; page += PAGE_SIZE) {
        if (task_populate_lock_page(task, page) < 0) {
            task_unmap_pages(task, low, page);
            task_vma_clear(vma);
            return -RELIEFOS_ENOMEM;
        }
    }
    return 0;
}

/** @brief Normalize, reserve and apply a partial lock or unlock transaction.
 * @param task Current execution-owned MM. @param addr First requested byte.
 * @param len Requested byte count. @param lock True to lock, false to unlock.
 * @param onfault Defer population while still charging locked virtual bytes.
 * @param check_quota False only for a remap whose final net quota was admitted.
 * @param populate Perform checked eager population for an ordinary mlock.
 * @return Zero or a negative Linux errno. Capacity and quota are checked before
 *         VMA mutation; checked eager population precedes policy publication.
 */
static int mm_lock_range(struct task *task, uint64_t addr, uint64_t len,
                         bool lock, bool onfault, bool check_quota, bool populate)
{
    uint64_t offset = addr & (PAGE_SIZE - 1ULL);
    uint64_t start = addr - offset;
    if (len > UINT64_MAX - offset || len + offset > UINT64_MAX - (PAGE_SIZE - 1ULL))
        return -RELIEFOS_ENOMEM;
    uint64_t length = align_up_page(len + offset);
    if (!length) return 0;
    if (start < RELIEFNT_USER_BASE || start >= RELIEFNT_USER_TOP ||
        length > RELIEFNT_USER_TOP - start) return -RELIEFOS_ENOMEM;
    uint64_t end = start + length;
    uint64_t current = mm_locked_bytes(task), already = 0;
    uint32_t splits = 0;
    uint64_t cursor = start;
    bool hole = false, policy_changed = false;
    const uint32_t lock_mask = TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT;
    const uint32_t requested = lock ? TASK_VMA_FLAG_LOCKED |
                                      (onfault ? TASK_VMA_FLAG_LOCKONFAULT : 0) : 0;
    const struct task_address_space_state *mm = sched_task_mm(task);
    uint64_t stack_low = mm->initial_stack_low ? mm->initial_stack_low : task->stack_low;
    uint64_t stack_top = mm->initial_stack_top ? mm->initial_stack_top : task->stack_top;
    while (cursor < end) {
        struct task_vma *vma = task_vma_containing(task, cursor, cursor + PAGE_SIZE);
        if (!vma && stack_low && cursor >= stack_low && cursor < stack_top) {
            uint64_t piece_end = stack_top < end ? stack_top : end;
            for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
                struct task_vma *next = sched_task_vma_at(task, i);
                if (next && next->used && next->start > cursor && next->start < piece_end)
                    piece_end = next->start;
            }
            if (lock) { ++splits; policy_changed = true; }
            cursor = piece_end;
            continue;
        }
        if (!vma) {
            hole = true;
            uint64_t next_start = end;
            if (stack_low > cursor && stack_low < next_start) next_start = stack_low;
            for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
                struct task_vma *next = sched_task_vma_at(task, i);
                if (next && next->used && next->start > cursor && next->start < next_start)
                    next_start = next->start;
            }
            cursor = next_start;
            continue;
        }
        uint64_t piece_end = vma->end < end ? vma->end : end;
        if ((vma->flags & TASK_VMA_FLAG_DEVICE) &&
            (!vma->device_mapping || !device_mapping_info(vma->device_mapping)->can_split) &&
            (cursor != vma->start || piece_end != vma->end))
            return -RELIEFOS_EOPNOTSUPP;
        if (vma->flags & TASK_VMA_FLAG_LOCKED) already += piece_end - cursor;
        if ((vma->flags & lock_mask) != requested) policy_changed = true;
        if (cursor > vma->start) ++splits;
        if (piece_end < vma->end) ++splits;
        cursor = piece_end;
    }
    if (lock && check_quota && !(task->cap_effective & (1ULL << CAP_IPC_LOCK))) {
        uint64_t limit = sched_task_limits(task)->memlock.rlim_cur & ~(PAGE_SIZE - 1ULL);
        if (current > limit || length - already > limit - current) return -RELIEFOS_ENOMEM;
    }
    if (hole) return -RELIEFOS_ENOMEM;
    /* Eager mlock still has to fault missing pages even when its policy is
     * already equal. Its final admission will repeat these checks. */
    if (!policy_changed && !(lock && !onfault && populate)) return 0;
    if (!policy_changed) splits = 0;
    uint32_t free_slots = 0;
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *slot = sched_task_vma_at(task, i);
        if (slot && !slot->used) ++free_slots;
    }
    while (free_slots < splits) {
        if (!sched_task_vma_at(task, sched_task_vma_capacity(task))) return -RELIEFOS_ENOMEM;
        ++free_slots;
    }
    if (lock && !onfault && populate) {
        struct mm_lock_intent intent = {0};
        int ret = mm_context_lock_intent_begin(sched_task_as(task), start, end, &intent);
        if (ret < 0) return ret;
        /* No policy changes before checked faults finish. The walker can
         * suspend, so discard movable pointers and re-admit the whole range. */
        task = NULL;
        mm = NULL;
        ret = mm_populate_lock_walk(start, end, true, false);
        if (!ret) {
            task = sched_current_task();
            ret = mm_lock_range(task, start, length, true, false, check_quota, false);
        }
        mm_context_lock_intent_end(&intent);
        return ret;
    }
    struct task_vma staged[2] = {{0}, {0}};
    unsigned staged_count = 0, staged_next = 0;
    int prepare_ret = mm_lock_prepare_slices(task, start, end, staged, &staged_count);
    if (prepare_ret < 0) {
        mm_lock_drop_slices(staged, staged_count);
        return prepare_ret;
    }
    mm_context_vma_changed(sched_task_as(task));
    cursor = start;
    while (cursor < end) {
        struct task_vma *vma = task_vma_containing(task, cursor, cursor + PAGE_SIZE);
        if (!vma) {
            uint64_t piece_end = stack_top < end ? stack_top : end;
            for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
                struct task_vma *next = sched_task_vma_at(task, i);
                if (next && next->used && next->start > cursor && next->start < piece_end)
                    piece_end = next->start;
            }
            if (lock) {
                vma = task_vma_free_slot(task);
                *vma = (struct task_vma){.used = 1, .start = cursor, .end = piece_end,
                    .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
                    .max_prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
                    .flags = TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_ANON |
                             TASK_VMA_FLAG_LOCKED |
                             (onfault ? TASK_VMA_FLAG_LOCKONFAULT : 0)};
            }
            cursor = piece_end;
            continue;
        }
        struct task_vma original = *vma;
        uint64_t piece_end = original.end < end ? original.end : end;
        struct task_vma *left = NULL;
        if (cursor > original.start) {
            left = task_vma_free_slot(task);
            if (!left || staged_next >= staged_count) panic("mlock split reservation lost");
            *left = staged[staged_next];
            staged[staged_next++] = (struct task_vma){0};
            left->sysv_shm_attachment = original.sysv_shm_attachment;
            sysv_shm_vma_split(left);
        }
        if (piece_end < original.end) {
            struct task_vma *right = task_vma_free_slot(task);
            if (!right || staged_next >= staged_count) panic("mlock split reservation lost");
            *right = staged[staged_next];
            staged[staged_next++] = (struct task_vma){0};
            right->sysv_shm_attachment = original.sysv_shm_attachment;
            sysv_shm_vma_split(right);
        }
        *vma = original;
        vma->start = cursor;
        vma->end = piece_end;
        mm_lock_slice_offset(vma, &original);
        vma->flags &= ~(TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT);
        if (lock) vma->flags |= TASK_VMA_FLAG_LOCKED |
                                (onfault ? TASK_VMA_FLAG_LOCKONFAULT : 0);
        cursor = piece_end;
    }
    mm_lock_drop_slices(staged, staged_count);
    return 0;
}

/** @brief Mark an admitted remap destination before its caller commits the move.
 * @param task Execution-owned MM. @param start First page. @param end Exclusive end.
 * @param onfault Preserve deferred population. @return Zero or metadata errno;
 *         the caller performs best-effort population after the move commits.
 */
int mm_lock_remap(struct task *task, uint64_t start, uint64_t end, bool onfault)
{
    return mm_lock_range(task, start, end - start, true, onfault, false, false);
}

/** @brief Lock Linux's page-rounded range under the caller's memlock policy.
 * @param addr First byte. @param len Requested bytes. @param flags Low 32-bit mlock2 flags.
 * @return Zero or negative EINVAL, EPERM, ENOMEM or EAGAIN.
 */
int64_t syscall_mm_mlock(uint64_t addr, uint64_t len, uint64_t flags)
{
    uint32_t mode = (uint32_t)flags;
    if (mode & ~LINUX_MLOCK_ONFAULT) return -RELIEFOS_EINVAL;
    struct task *task = sched_current_task();
    if (!mm_can_lock(task)) return -RELIEFOS_EPERM;
    return mm_lock_range(task, addr, len, true, mode & LINUX_MLOCK_ONFAULT, true, true);
}

/** @brief Unlock a Linux page-rounded range without a lock privilege check.
 * @param addr First byte. @param len Requested bytes. @return Zero or negative errno.
 */
int64_t syscall_mm_munlock(uint64_t addr, uint64_t len)
{
    struct task *task = sched_current_task();
    if (!task) return -RELIEFOS_EINVAL;
    return mm_lock_range(task, addr, len, false, false, true, false);
}

/** @brief Set current and future lock policy for the current task.
 * @param flags Linux mlockall mode. @return Zero or a negative errno.
 */
int64_t syscall_mm_mlockall(uint64_t flags)
{
    struct task *task = sched_current_task();
    uint32_t mode = (uint32_t)flags;
    if ((mode & ~(LINUX_MCL_CURRENT | LINUX_MCL_FUTURE | LINUX_MCL_ONFAULT)) ||
        !(mode & (LINUX_MCL_CURRENT | LINUX_MCL_FUTURE))) return -RELIEFOS_EINVAL;
    if (!mm_can_lock(task)) return -RELIEFOS_EPERM;
    if ((mode & LINUX_MCL_CURRENT) && !(task->cap_effective & (1ULL << CAP_IPC_LOCK)) &&
        task_vma_total_bytes(task) > (sched_task_limits(task)->memlock.rlim_cur & ~(PAGE_SIZE - 1ULL)))
        return -RELIEFOS_ENOMEM;
    if (mm_context_ensure(sched_task_as(task)) < 0) return -RELIEFOS_ENOMEM;
    if (mode & LINUX_MCL_CURRENT) {
        const struct task_address_space_state *mm = sched_task_mm(task);
        uint64_t low = mm->initial_stack_low ? mm->initial_stack_low : task->stack_low;
        uint64_t top = mm->initial_stack_top ? mm->initial_stack_top : task->stack_top;
        if (low && top > low) {
            /* Publish the requested policy here; eager population remains
             * best effort after all CURRENT and FUTURE policy is set. */
            int ret = mm_lock_range(task, low, top - low, true,
                                    mode & LINUX_MCL_ONFAULT, true, false);
            if (ret < 0) return ret;
        }
        bool changed = false;
        for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
            struct task_vma *vma = sched_task_vma_at(task, i);
            if (vma && vma->used) {
                uint32_t requested = TASK_VMA_FLAG_LOCKED |
                    ((mode & LINUX_MCL_ONFAULT) ? TASK_VMA_FLAG_LOCKONFAULT : 0);
                if (!changed && (vma->flags & (TASK_VMA_FLAG_LOCKED |
                                               TASK_VMA_FLAG_LOCKONFAULT)) != requested) {
                    mm_context_vma_changed(sched_task_as(task));
                    changed = true;
                }
                vma->flags |= TASK_VMA_FLAG_LOCKED;
                if (mode & LINUX_MCL_ONFAULT) vma->flags |= TASK_VMA_FLAG_LOCKONFAULT;
                else vma->flags &= ~TASK_VMA_FLAG_LOCKONFAULT;
            }
        }
    }
    uint32_t future = mode & (LINUX_MCL_FUTURE | LINUX_MCL_ONFAULT);
    if (mm_lock_flags(sched_task_as(task)) != future) {
        mm_context_vma_changed(sched_task_as(task));
        mm_context_set_lock_flags(sched_task_as(task), future);
    }
    if ((mode & LINUX_MCL_CURRENT) && !(mode & LINUX_MCL_ONFAULT))
        (void)mm_populate_locked_range(RELIEFNT_USER_BASE, RELIEFNT_USER_TOP, false);
    return 0;
}

/** @brief Clear all current and future lock policy of the current task.
 * @return Zero or negative EINVAL when no task exists.
 */
int64_t syscall_mm_munlockall(void)
{
    struct task *task = sched_current_task();
    if (!task) return -RELIEFOS_EINVAL;
    bool changed = false;
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (vma && vma->used && (vma->flags & (TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT))) {
            if (!changed) {
                mm_context_vma_changed(sched_task_as(task));
                changed = true;
            }
            vma->flags &= ~(TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_LOCKONFAULT);
        }
    }
    if (mm_lock_flags(sched_task_as(task))) {
        if (!changed) mm_context_vma_changed(sched_task_as(task));
        mm_context_set_lock_flags(sched_task_as(task), 0);
    }
    return 0;
}
