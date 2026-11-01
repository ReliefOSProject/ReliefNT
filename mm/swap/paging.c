/* SPDX-License-Identifier: Apache-2.0 */
#include "../user/memory_internal.h"
#include <reliefnt/swap.h>
#include <reliefnt/futex.h>
#include <reliefnt/swap_internal.h>
#include <reliefnt/export.h>
#include <linux/errno.h>

/** @brief Find a live task whose current root owns the candidate leaf.
 * @param as Root protected by the execution writer. @return Borrowed task or NULL.
 * No task or movable address-space pointer survives this transaction.
 */
static struct task *swap_find_owner(const struct address_space *as)
{
    if (!as || !as->cr3) return NULL;
    for (uint32_t i = 0;; ++i) {
        struct task *task = sched_task_at(i);
        if (!task) break;
        if (task->kind == TASK_KIND_USER && task->state != TASK_EXITED &&
            sched_task_as(task)->cr3 == as->cr3) return task;
    }
    return NULL;
}

/** @brief Validate a private candidate against its VMA or implicit stack.
 * @param task Live owner under the execution writer. @param page Aligned page.
 * @return True only for a private, unlocked mapping whose provenance can be
 * checked further by the physical/cache ownership checks at the call site.
 */
static bool swap_private_candidate(struct task *task, uint64_t page)
{
    struct task_vma *vma = task_vma_containing(task, page, page + PAGE_SIZE);
    if (vma) {
        if (vma->flags & (TASK_VMA_FLAG_SHARED | TASK_VMA_FLAG_SHARED_FILE | TASK_VMA_FLAG_DEVICE |
                          TASK_VMA_FLAG_SYSV_SHM | TASK_VMA_FLAG_VDSO |
                          TASK_VMA_FLAG_VVAR | TASK_VMA_FLAG_LOCKED)) return false;
        return (vma->flags & TASK_VMA_FLAG_ANON) ||
               ((vma->flags & (TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_PRIVATE)) ==
                (TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_PRIVATE));
    }
    const struct task_address_space_state *mm = sched_task_mm(task);
    uint64_t low = mm->initial_stack_low ? mm->initial_stack_low : task->stack_low;
    uint64_t top = mm->initial_stack_top ? mm->initial_stack_top : task->stack_top;
    return low && page >= low && page < top;
}

/** @brief Check fault access before reading a private nonresident page.
 * @param task Live task under the writer. @param page Aligned user page.
 * @param error x86 fault flags. @return True when current policy allows access.
 */
static bool swap_fault_allowed(struct task *task, uint64_t page, uint64_t error)
{
    if (error & 8ULL) return false;
    struct task_vma *vma = task_vma_containing(task, page, page + PAGE_SIZE);
    if (vma) {
        if (vma->prot == LINUX_PROT_NONE ||
            ((error & 2ULL) && !(vma->prot & LINUX_PROT_WRITE)) ||
            ((error & 16ULL) && !(vma->prot & LINUX_PROT_EXEC))) return false;
        return true;
    }
    const struct task_address_space_state *mm = sched_task_mm(task);
    uint64_t low = mm->initial_stack_low ? mm->initial_stack_low : task->stack_low;
    uint64_t top = mm->initial_stack_top ? mm->initial_stack_top : task->stack_top;
    return low && page >= low && page < top && !(error & 16ULL);
}

/** @brief Write one exclusive private RAM page to a new swap slot.
 * @param as Live root protected by the outer writer. @param va User address.
 * @return One after actually freeing its frame, zero if ineligible, or terminal
 * negative errno. The caller proves no other spin/reader/loader/usercopy lock;
 * no transaction release or deferred I/O occurs during this one-page transfer.
 */
int swap_try_evict(struct address_space *as, uint64_t va)
{
    if (!kernel_execution_is_outermost()) return -EDEADLK;
    uint64_t page = va & ~(PAGE_SIZE - 1ULL);
    if (!as || page < RELIEFNT_USER_BASE || page >= RELIEFNT_USER_TOP ||
        mm_context_lock_intent_intersects(as, page, page + PAGE_SIZE)) return 0;
    struct task *task = swap_find_owner(as);
    if (!task || !swap_private_candidate(task, page)) return 0;
    enum paging_user_entry_kind kind = paging_user_entry_kind(as, page);
    if (kind != PAGING_USER_ENTRY_RESIDENT && kind != PAGING_USER_ENTRY_PROTNONE) return 0;
    uint64_t flags = address_space_user_page_flags(as, page);
    uint64_t physical = address_space_user_page_phys(as, page);
    if (!physical || (flags & (RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_SHARED)) ||
        page_cache_owns(physical) || mm_page_refcount(physical) != 1 ||
        futex_swap_pinned(physical)) return 0;
    uint64_t token = swap_slot_alloc();
    if (!token) return -ENOSPC;
    int ret = mm_swap_record_insert_reclaim(as, page, token);
    if (ret < 0) { swap_slot_put(token); return ret; }
    uint64_t original = paging_user_swap_freeze(as, page);
    if (!original) { mm_swap_record_remove(as, page); return 0; }
    ret = swap_slot_submit_write(token, physical);
    if (ret < 0) {
        bool restored = paging_user_swap_restore(as, page, original);
        mm_swap_record_remove(as, page);
        return restored ? (ret == SWAP_STORAGE_CONTENDED ? -EBUSY : ret) : -EIO;
    }
    /* An owner acquired during the transfer invalidates the exclusivity proof. */
    if (mm_page_refcount(physical) != 1 || page_cache_owns(physical)) {
        bool restored = paging_user_swap_restore(as, page, original);
        mm_swap_record_remove(as, page);
        return restored ? 0 : -EIO;
    }
    if (!paging_user_swap_commit(as, page, original)) {
        (void)paging_user_swap_restore(as, page, original);
        mm_swap_record_remove(as, page);
        return -EIO;
    }
    mm_context_vma_changed(as);
    return mm_free_page_reclaimed(physical) ? 1 : 0;
}

/** @brief Read one swap leaf into a fresh private RAM page.
 * @param task Execution-owned live task. @param address User fault address.
 * @param error x86 fault flags. @return One on success, zero for a non-swap
 * leaf, or terminal negative errno; read failure preserves the slot and bytes.
 */
int swap_resolve_fault(struct task *task, uint64_t address, uint64_t error)
{
    if (!task || task->kind != TASK_KIND_USER) return 0;
    uint64_t page = address & ~(PAGE_SIZE - 1ULL);
    struct address_space *as = sched_task_as(task);
    if (paging_user_entry_kind(as, page) != PAGING_USER_ENTRY_SWAP) return 0;
    if (!kernel_execution_is_outermost()) return -EDEADLK;
    if (!swap_fault_allowed(task, page, error) ||
        (address_space_user_page_flags(as, page) & RELIEFNT_PAGE_PROTNONE)) return -EACCES;
    uint64_t token = mm_swap_record_lookup(as, page);
    if (!token) return -EIO;
    uint64_t physical = mm_alloc_page();
    if (!physical) return -ENOMEM;
    if (!mm_phys_to_virt(physical)) { mm_free_page(physical); return -ENOMEM; }
    int ret = swap_slot_submit_read(token, physical);
    if (ret < 0) { mm_free_page(physical); return ret; }
    if (!paging_user_swap_in(as, page, physical, (error & 2ULL) != 0)) {
        mm_free_page(physical);
        return -EIO;
    }
    mm_context_vma_changed(as);
    mm_swap_record_remove(as, page);
    return 1;
}

/** @brief Restore one durable leaf for a draining area, preserving permissions.
 * @param task Unreleased user MM owner under the execution writer.
 * @param address User address. @return One on transfer, zero for a non-swap
 * leaf, or negative errno. Failed reads leave the marker and durable slot intact.
 */
int swap_restore_leaf(struct task *task, uint64_t address)
{
    if (!task || task->kind != TASK_KIND_USER) return 0;
    if (!kernel_execution_is_outermost()) return -EDEADLK;
    uint64_t page = address & ~(PAGE_SIZE - 1ULL);
    struct address_space *as = sched_task_as(task);
    if (paging_user_entry_kind(as, page) != PAGING_USER_ENTRY_SWAP) return 0;
    uint64_t token = mm_swap_record_lookup(as, page);
    if (!token) return -EIO;
    uint64_t physical = mm_alloc_page();
    if (!physical) return -ENOMEM;
    if (!mm_phys_to_virt(physical)) { mm_free_page(physical); return -ENOMEM; }
    int ret = swap_slot_submit_read(token, physical);
    if (ret < 0) { mm_free_page(physical); return ret; }
    if (!paging_user_swap_materialize(as, page, physical)) {
        mm_free_page(physical);
        return -EIO;
    }
    mm_context_vma_changed(as);
    mm_swap_record_remove(as, page);
    return 1;
}

/** @brief Resolve swapped leaves only in one bounded outer-writer user slice.
 * @param task Live user task. @param address First byte. @param length Bytes,
 * at most 32768. @param write_access User page will receive output bytes.
 * @return Zero, SWAP_STORAGE_CONTENDED before submission, or a terminal errno.
 * No lazy/COW population or suspension occurs.
 */
int swap_prepare_user_range(struct task *task, uint64_t address, uint64_t length,
                            bool write_access)
{
    if (!length) return 0;
    if (length > 32768u) return -EINVAL;
    if (!task || task->kind != TASK_KIND_USER || address < RELIEFNT_USER_BASE ||
        address >= RELIEFNT_USER_TOP || length > RELIEFNT_USER_TOP - address)
        return -EFAULT;
    uint64_t end = address + length;
    for (uint64_t page = address & ~(PAGE_SIZE - 1ULL); page < end; page += PAGE_SIZE) {
        if (paging_user_entry_kind(sched_task_as(task), page) != PAGING_USER_ENTRY_SWAP) continue;
        int ret = swap_resolve_fault(task, page, 4u | (write_access ? 2u : 0u));
        if (ret < 0) return ret;
        if (!ret) return -EIO;
    }
    return 0;
}
EXPORT_SYMBOL(swap_prepare_user_range);

/** @brief Prepare an exiting task's bounded operand with real CPU ownership.
 * @param task Unreleased target. @param address User start. @param length At
 * most 32 KiB. @param write_access Select an output. @return Zero, prerequisite
 * contention, or terminal errno; cleanup admission is released before return.
 */
int swap_prepare_user_cleanup(struct task *task, uint64_t address, uint64_t length,
                              bool write_access)
{
    int ret = swap_storage_cleanup_enter();
    if (ret) return ret;
    ret = swap_prepare_user_range(task, address, length, write_access);
    swap_storage_cleanup_leave();
    return ret;
}
