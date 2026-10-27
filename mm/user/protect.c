#include "memory_internal.h"

static int syscall_mm_lock_range(uint64_t addr, uint64_t len, bool locked);


/** @brief Remove only backed user leaves in a virtual interval and release their owners.
 * @param task User task whose address space is modified; NULL is ignored.
 * @param start Inclusive first virtual byte to unmap.
 * @param end Exclusive virtual range end.
 * @return None. Backed leaves are traversed sparsely and remote user TLBs are
 *         synchronized once after any mapping was removed.
 */
void task_unmap_pages(struct task *task, uint64_t start, uint64_t end)
{
    uint64_t cursor = start;
    bool changed = false;
    if (!task || start >= end) return;
    while (cursor < end) {
        uint64_t page;
        uint64_t flags;
        if (!address_space_user_page_next(sched_task_as(task), cursor, end,
                                          &page, NULL, &flags)) break;
        if (page >= end) break;
        struct task_vma *vma = task_vma_containing(task, page, page + PAGE_SIZE);
        uint64_t phys = address_space_unmap_user_page(sched_task_as(task), page);
        if (phys) {
            changed = true;
            if ((vma && (vma->flags & TASK_VMA_FLAG_DEVICE)) ||
                (flags & RELIEFNT_PAGE_DEVICE)) {
                /* Framebuffer mappings borrow reserved VRAM. */
            } else if (page_cache_owns(phys)) {
                page_cache_release(phys);
            } else {
                mm_free_page(phys);
            }
        }
        cursor = page + PAGE_SIZE;
    }
    if (changed) smp_flush_user_tlb();
}

static int syscall_mm_lock_range(uint64_t addr, uint64_t len, bool locked)
{
    struct task *task = sched_current_task();
    uint64_t mapped_len, end;
    bool found = false;
    if (!task || task->kind != TASK_KIND_USER || !len ||
        align_user_len(len, &mapped_len) < 0 || (addr & (PAGE_SIZE - 1ULL)) ||
        addr < RELIEFNT_USER_BASE || addr >= RELIEFNT_USER_TOP ||
        mapped_len > RELIEFNT_USER_TOP - addr) return -RELIEFOS_EINVAL;
    end = addr + mapped_len;
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (!vma || !vma->used || vma->end <= addr || vma->start >= end) continue;
        if (vma->start > addr || vma->end < end) return -RELIEFOS_ENOMEM;
        vma->flags = locked ? (vma->flags | TASK_VMA_FLAG_LOCKED) :
                              (vma->flags & ~TASK_VMA_FLAG_LOCKED);
        found = true;
    }
    return found ? 0 : -RELIEFOS_ENOMEM;
}

int64_t syscall_mm_mlock(uint64_t addr, uint64_t len, uint64_t flags)
{
    if (flags & ~LINUX_MLOCK_ONFAULT) return -RELIEFOS_EINVAL;
    return syscall_mm_lock_range(addr, len, true);
}

int64_t syscall_mm_munlock(uint64_t addr, uint64_t len)
{
    return syscall_mm_lock_range(addr, len, false);
}

int64_t syscall_mm_mlockall(uint64_t flags)
{
    struct task *task = sched_current_task();
    if (!task || (flags & ~(uint64_t)(LINUX_MCL_CURRENT | LINUX_MCL_FUTURE | LINUX_MCL_ONFAULT)) ||
        !(flags & (LINUX_MCL_CURRENT | LINUX_MCL_FUTURE))) return -RELIEFOS_EINVAL;
    if (flags & LINUX_MCL_CURRENT) {
        for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
            struct task_vma *vma = sched_task_vma_at(task, i);
            if (vma && vma->used) vma->flags |= TASK_VMA_FLAG_LOCKED;
        }
    }
    task->mlockall_flags = (uint32_t)flags;
    return 0;
}

int64_t syscall_mm_munlockall(void)
{
    struct task *task = sched_current_task();
    if (!task) return -RELIEFOS_EINVAL;
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (vma && vma->used) vma->flags &= ~TASK_VMA_FLAG_LOCKED;
    }
    task->mlockall_flags = 0;
    return 0;
}

/** @brief Protect a covered user range while preserving split backing references.
 * @param addr Page-aligned start in the execution-owned current user MM.
 * @param len Byte length rounded to pages; zero is a no-op.
 * @param prot Linux PROT bits bounded by every affected VMA's maximum access.
 * @return Zero or negative EINVAL/ENOMEM/EACCES/EOPNOTSUPP. Metadata slots are
 * reserved before PTE changes; device permission changes synchronously flush
 * remote user TLBs. No graph/module lock spans heap growth or backing release;
 * successful splits acquire independent core references to the original lease.
 */
int64_t syscall_mm_mprotect(uint64_t addr, uint64_t len, uint64_t prot)
{
    struct task *task = sched_current_task();
    uint64_t mapped_len;
    uint64_t end;
    if (!task || task->kind != TASK_KIND_USER || (addr & (PAGE_SIZE - 1ULL))) {
        return -RELIEFOS_EINVAL;
    }
    if (!len) return 0;
    if (align_user_len(len, &mapped_len) < 0 || addr >= RELIEFNT_USER_TOP ||
        mapped_len > RELIEFNT_USER_TOP - addr) return -RELIEFOS_ENOMEM;
    if ((prot & ~(uint64_t)(LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC)) ||
        (prot & (LINUX_PROT_WRITE | LINUX_PROT_EXEC)) ==
                     (LINUX_PROT_WRITE | LINUX_PROT_EXEC)) {
        return -RELIEFOS_EINVAL;
    }
    end = addr + mapped_len;
    /* Validate every page first. Linux permits mprotect to cover adjacent
     * VMAs with different attributes, but rejects a range containing a hole. */
    uint64_t cursor = addr;
    uint32_t split_count = 0;
    bool device_tlb = false;
    while (cursor < end) {
        struct task_vma *vma = task_vma_containing(task, cursor, cursor + PAGE_SIZE);
        if (!vma) return -RELIEFOS_ENOMEM;
        if (vma->device_mapping) device_tlb = true;
        if (prot & ~vma->max_prot) return -RELIEFOS_EACCES;
        uint64_t piece_end = vma->end < end ? vma->end : end;
        /* Device mappings without split capability accept only a whole-VMA no-op change. */
        if ((vma->flags & TASK_VMA_FLAG_DEVICE) && (!vma->device_mapping || !device_mapping_info(vma->device_mapping)->can_split) &&
            (cursor != vma->start || piece_end != vma->end || prot != vma->prot)) {
            return -RELIEFOS_EOPNOTSUPP;
        }
        if (piece_end <= cursor) return -RELIEFOS_ENOMEM;
        if (cursor > vma->start) ++split_count;
        if (piece_end < vma->end) ++split_count;
        cursor = piece_end;
    }

    /* Secure every split slot before changing PTEs. Adding slots may grow and
     * relocate the VMA store, so keep no table pointers across this phase.
     * If allocation fails, no page permissions or VMA metadata have changed. */
    uint32_t free_slots = 0;
    for (uint32_t i = 0; split_count && i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *slot = sched_task_vma_at(task, i);
        if (slot && !slot->used) ++free_slots;
    }
    while (free_slots < split_count) {
        if (!sched_task_vma_at(task, sched_task_vma_capacity(task))) {
            return -RELIEFOS_ENOMEM;
        }
        ++free_slots;
    }

    /* Apply the PTE change with each mapping's own device/cache attributes. */
    cursor = addr;
    bool pte_changed = false;
    while (cursor < end) {
        struct task_vma *vma = task_vma_containing(task, cursor, cursor + PAGE_SIZE);
        if (!vma) return -RELIEFOS_ENOMEM;
        uint64_t piece_end = vma->end < end ? vma->end : end;
        if ((vma->flags & TASK_VMA_FLAG_DEVICE) && (!vma->device_mapping || !device_mapping_info(vma->device_mapping)->can_split)) {
            /* Validated as a whole-VMA no-op: leave device PTEs untouched. */
            cursor = piece_end;
            continue;
        }
        uint64_t page_flags = 0;
        if (prot & LINUX_PROT_WRITE) page_flags |= RELIEFNT_PAGE_WRITABLE;
        if (!(prot & LINUX_PROT_EXEC)) page_flags |= RELIEFNT_PAGE_NOEXEC;
        if (prot == LINUX_PROT_NONE) page_flags |= RELIEFNT_PAGE_PROTNONE;
        if (vma->flags & TASK_VMA_FLAG_DEVICE) page_flags |= RELIEFNT_PAGE_DEVICE;
        uint64_t page_cursor = cursor;
        while (page_cursor < piece_end) {
            uint64_t page;
            if (!address_space_user_page_next(sched_task_as(task), page_cursor,
                                              piece_end, &page, NULL, NULL)) break;
            if (!address_space_protect_user_page(sched_task_as(task), page,
                                                 page_flags)) {
                /* Earlier pages may already have changed. Revoke stale remote
                 * DRM translations even though the range operation fails. */
                if (device_tlb && pte_changed) smp_flush_user_tlb();
                return -RELIEFOS_EACCES;
            }
            pte_changed = true;
            page_cursor = page + PAGE_SIZE;
        }
        cursor = piece_end;
    }
    /* The page-table helper invalidates only the current CPU. Device protection
     * changes must not return while a CLONE_VM peer can still write through a
     * stale translation, so synchronously flush remote user TLBs first. */
    if (device_tlb && pte_changed) smp_flush_user_tlb();

    /**
 * @brief A partial protection change needs independent VMA metadata. In particular, PT_GNU_RELRO protects only the GOT page while BSS in the same original load segment must remain writable and lazily mappable.
 */
    cursor = addr;
    while (cursor < end) {
        struct task_vma *vma = task_vma_containing(task, cursor, cursor + PAGE_SIZE);
        if (!vma) return -RELIEFOS_ENOMEM;
        struct task_vma original = *vma;
        uint64_t piece_end = original.end < end ? original.end : end;
        if (cursor > original.start) {
            struct task_vma *left = task_vma_free_slot(task);
            if (!left) return -RELIEFOS_ENOMEM;
            *left = original;
            task_vma_retain_file(left, &original);
            sysv_shm_vma_split(left);
            storage_inode_retain(left->inode);
            if (left->device_mapping) (void)device_mapping_get(left->device_mapping);
            left->end = cursor;
            if (left->device_mapping)
                left->device_physical = device_mapping_physical(left->device_mapping,left->device_map_offset);
        }
        if (piece_end < original.end) {
            struct task_vma *right = task_vma_free_slot(task);
            if (!right) return -RELIEFOS_ENOMEM;
            *right = original;
            task_vma_retain_file(right, &original);
            sysv_shm_vma_split(right);
            storage_inode_retain(right->inode);
            if (right->device_mapping) (void)device_mapping_get(right->device_mapping);
            right->start = piece_end;
            if(right->device_mapping)right->device_map_offset=original.device_map_offset+piece_end-original.start;
            if (right->device_mapping)
                right->device_physical = device_mapping_physical(right->device_mapping,right->device_map_offset);
            if (right->flags & TASK_VMA_FLAG_FILE) {
                right->file_offset += piece_end - original.start;
            }
        }
        *vma = original;
        vma->start = cursor;
        vma->end = piece_end;
        if (vma->device_mapping) {
            vma->device_map_offset=original.device_map_offset+cursor-original.start;
            vma->device_physical=device_mapping_physical(vma->device_mapping,vma->device_map_offset);
        }
        if (vma->flags & TASK_VMA_FLAG_FILE) {
            vma->file_offset += cursor - original.start;
        }
        vma->prot = (uint32_t)prot;
        cursor = piece_end;
    }
    return 0;
}

/** @brief Remove user PTEs and release fully retired provider/device references.
 * @param addr Page-aligned start in the execution-owned current user MM.
 * @param len Nonzero byte length rounded to pages without aperture overflow.
 * @return Zero or negative errno. Split-capability and free metadata are checked
 * before modifying the VMA store; retained slices preserve provider offsets.
 * Final backing/OFD puts may perform I/O and run without graph/module locks.
 */
int64_t syscall_mm_munmap(uint64_t addr, uint64_t len)
{
    struct task *task = sched_current_task();
    uint64_t mapped_len;
    uint64_t start = addr;
    uint64_t end;
    uint32_t split_count = 0;
    uint32_t free_slots = 0;

    if (!task || task->kind != TASK_KIND_USER || (start & (PAGE_SIZE - 1ULL)) != 0 ||
        start < RELIEFNT_USER_BASE || start >= RELIEFNT_USER_TOP) {
        return -RELIEFOS_EINVAL;
    }
    if (align_user_len(len, &mapped_len) < 0 || mapped_len > RELIEFNT_USER_TOP - start) {
        return -RELIEFOS_EINVAL;
    }
    end = start + mapped_len;
    /* Unmapping a hole is a successful no-op on Linux. Count only VMA
     * interiors that need a right-hand metadata split. */
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (vma && vma->used && (vma->flags & TASK_VMA_FLAG_DEVICE) &&
            (!vma->device_mapping || !device_mapping_info(vma->device_mapping)->can_split) &&
            start < vma->end && end > vma->start &&
            (start > vma->start || end < vma->end))
            return -RELIEFOS_EOPNOTSUPP;
        if (vma && vma->used && start > vma->start && start < vma->end &&
            end < vma->end) ++split_count;
    }
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (vma && !vma->used) {
            ++free_slots;
        }
    }
    /* Splits need a spare VMA entry each. The growable store extends on
     * demand, so secure the slots before mutating anything instead of
     * refusing whenever the fixed array happens to be full. */
    while (split_count > free_slots) {
        if (!sched_task_vma_at(task, sched_task_vma_capacity(task))) {
            return -RELIEFOS_ENOMEM;
        }
        ++free_slots;
    }
    task_unmap_pages(task, start, end);
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (!vma) continue;
        struct task_vma original;
        uint64_t remove_start;
        uint64_t remove_end;
        if (!vma->used || vma->end <= start || vma->start >= end) {
            continue;
        }
        original = *vma;
        remove_start = original.start > start ? original.start : start;
        remove_end = original.end < end ? original.end : end;
        if (remove_start == original.start && remove_end == original.end) {
            task_vma_clear(vma);
        } else if (remove_start == original.start) {
            vma->start = remove_end;
            if(vma->device_mapping)vma->device_map_offset=original.device_map_offset+remove_end-original.start;
            if (vma->device_mapping)
                vma->device_physical = device_mapping_physical(vma->device_mapping,vma->device_map_offset);
            if (vma->flags & TASK_VMA_FLAG_FILE) {
                vma->file_offset += remove_end - original.start;
            }
        } else if (remove_end == original.end) {
            vma->end = remove_start;
        } else {
            struct task_vma *right = task_vma_free_slot(task);
            if (!right) {
                return -RELIEFOS_ENOMEM;
            }
            *right = original;
            task_vma_retain_file(right, &original);
            sysv_shm_vma_split(right);
            storage_inode_retain(right->inode);
            if (right->device_mapping) (void)device_mapping_get(right->device_mapping);
            right->start = remove_end;
            if(right->device_mapping)right->device_map_offset=original.device_map_offset+remove_end-original.start;
            if (right->device_mapping)
                right->device_physical = device_mapping_physical(right->device_mapping,right->device_map_offset);
            if (right->flags & TASK_VMA_FLAG_FILE) {
                right->file_offset += remove_end - original.start;
            }
            vma->end = remove_start;
        }
    }
    return 0;
}
