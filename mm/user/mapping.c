#include "memory_internal.h"

/**
 * Task mmap top.
 * @param task Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint64_t task_mmap_top(const struct task *task);

/** @brief Check whether an interval is free of VMA reservations and backed PTEs.
 * @param task User task whose VMA store and address space are inspected.
 * @param start Inclusive first user virtual byte.
 * @param end Exclusive interval end.
 * @return 1 when valid, outside the reserved kernel hole, and unoccupied; 0 otherwise.
 * Page-table occupancy is checked through sparse owned-table traversal.
 */
static int task_user_pages_free(const struct task *task, uint64_t start, uint64_t end);

/** @brief Find a low-to-high free anonymous mmap interval without scanning VMA spans.
 * @param task User task receiving the reservation.
 * @param len Nonzero page-rounded interval length.
 * @return First free interval base or zero when none fits below the stack.
 */
static uint64_t task_find_mmap_region(struct task *task, uint64_t len);

/** @brief Find a high-to-low free file mmap interval without scanning VMA spans.
 * @param task User task receiving the reservation.
 * @param len Nonzero page-rounded interval length.
 * @return Highest free interval base or zero when none fits below the stack.
 * File mappings remain near the stack end to leave contiguous room for heap growth.
 */
static uint64_t task_find_file_mmap_region(struct task *task, uint64_t len);


/**
 * Align up page.
 * @param value Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint64_t align_up_page(uint64_t value)
{
    return (value + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

/**
 * Align down page.
 * @param value Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint64_t align_down_page(uint64_t value)
{
    return value & ~(PAGE_SIZE - 1ULL);
}

/**
 * Align user len.
 * @param len Maximum number of elements available in the related buffer.
 * @param out Output storage updated by the function.
 * @return The value or status produced by the operation.
 */
int align_user_len(uint64_t len, uint64_t *out)
{
    if (!len || len > RELIEFNT_USER_TOP - RELIEFNT_USER_BASE) {
        return -RELIEFOS_EINVAL;
    }
    if (len > UINT64_MAX - (PAGE_SIZE - 1ULL)) {
        return -RELIEFOS_EINVAL;
    }
    *out = align_up_page(len);
    return *out ? 0 : -RELIEFOS_EINVAL;
}

/**
 * Task mmap top.
 * @param task Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint64_t task_mmap_top(const struct task *task)
{
    uint64_t stack_top = (task && task->stack_top) ? task->stack_top : RELIEFNT_USER_TOP - PAGE_SIZE;
    if (task && sched_task_mm(task)->initial_stack_top)
        stack_top = sched_task_mm(task)->initial_stack_top;
    uint64_t stack_low = stack_top - (uint64_t)RELIEFNT_USER_STACK_MAX_PAGES * PAGE_SIZE;
    if (stack_low > RELIEFNT_USER_BASE + PAGE_SIZE) {
        return stack_low - PAGE_SIZE;
    }
    return stack_low;
}

/**
 * Task user pages free.
 * @param task Value supplied by the caller.
 * @param start Value supplied by the caller.
 * @param end Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int task_user_pages_free(const struct task *task, uint64_t start, uint64_t end)
{
    if (!task || start < RELIEFNT_USER_BASE || start >= end || end > RELIEFNT_USER_TOP) {
        return 0;
    }
    if (start < RELIEFNT_KERNEL_HOLE_END && end > RELIEFNT_KERNEL_HOLE_START) {
        return 0;
    }
    if (task_vma_overlaps(task, start, end)) {
        return 0;
    }
    return address_space_user_range_free(sched_task_as(task), start, end) ? 1 : 0;
}

/**
 * Task find mmap region.
 * @param task Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
static uint64_t task_find_mmap_region(struct task *task, uint64_t len)
{
    uint64_t top = task_mmap_top(task);
    if (top <= RELIEFNT_USER_MMAP_BASE || len > top - RELIEFNT_USER_MMAP_BASE) {
        return 0;
    }
    for (uint64_t start = RELIEFNT_USER_MMAP_BASE; start <= top - len;) {
        uint64_t end = start + len;
        uint64_t occupied_end = start;
        if (start < RELIEFNT_KERNEL_HOLE_END && end > RELIEFNT_KERNEL_HOLE_START) {
            start = RELIEFNT_KERNEL_HOLE_END;
            continue;
        }
        /* VMA metadata is sparse too: skip an entire overlapping reservation
         * rather than retrying each page inside a multi-terabyte VMA. */
        for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
            const struct task_vma *vma = sched_task_vma_at(task, i);
            if (vma && vma->used && vma->start < end && vma->end > start &&
                vma->end > occupied_end)
                occupied_end = vma->end;
        }
        if (occupied_end > start) {
            start = align_up_page(occupied_end);
            continue;
        }
        if (task_user_pages_free(task, start, start + len)) {
            return start;
        }
        uint64_t occupied_page;
        if (!address_space_user_page_next(sched_task_as(task), start, end,
                                          &occupied_page, NULL, NULL)) return 0;
        start = occupied_page + PAGE_SIZE;
    }
    return 0;
}

/**
 * @brief Keep read-only file mappings at the opposite end of the mmap interval so a large font, dictionary, or other resource cannot occupy the process break's next contiguous extension.
 */
static uint64_t task_find_file_mmap_region(struct task *task, uint64_t len)
{
    uint64_t top = task_mmap_top(task);
    uint64_t start;
    if (top <= RELIEFNT_USER_MMAP_BASE || len > top - RELIEFNT_USER_MMAP_BASE) {
        return 0;
    }
    start = align_down_page(top - len);
    for (;;) {
        uint64_t end = start + len;
        uint64_t earliest_overlap = start + len;
        if (start < RELIEFNT_KERNEL_HOLE_END && end > RELIEFNT_KERNEL_HOLE_START) {
            if (RELIEFNT_KERNEL_HOLE_START < len + RELIEFNT_USER_MMAP_BASE) return 0;
            start = align_down_page(RELIEFNT_KERNEL_HOLE_START - len);
            continue;
        }
        for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
            const struct task_vma *vma = sched_task_vma_at(task, i);
            if (vma && vma->used && vma->start < end && vma->end > start &&
                vma->start < earliest_overlap)
                earliest_overlap = vma->start;
        }
        if (earliest_overlap < end) {
            if (earliest_overlap < len + RELIEFNT_USER_MMAP_BASE) return 0;
            start = align_down_page(earliest_overlap - len);
            continue;
        }
        if (task_user_pages_free(task, start, start + len)) {
            return start;
        }
        uint64_t cursor = start;
        uint64_t last_page = 0;
        uint64_t page;
        while (address_space_user_page_next(sched_task_as(task), cursor, end,
                                            &page, NULL, NULL)) {
            last_page = page;
            cursor = page + PAGE_SIZE;
        }
        if (!last_page || last_page < len + RELIEFNT_USER_MMAP_BASE) return 0;
        uint64_t next_start = align_down_page(last_page - len);
        if (next_start >= start) return 0;
        start = next_start;
    }
}

/** @brief Map anonymous/provider bytes or an independently pinned device backing.
 * @param addr Fixed address or placement hint, checked against the user aperture.
 * @param len Nonzero byte length, rounded to complete pages without overflow.
 * @param prot Linux PROT bits, constrained by descriptor/provider access and W^X.
 * @param flags Linux mmap flags, including validated fixed/no-replace semantics.
 * @param fd Original OFD admitted by the syscall, ignored for anonymous mappings.
 * @param offset Page-aligned file offset or provider mapping selector.
 * @return User address or negative errno. Execution-owned current MM required;
 * no graph/module lock spans callbacks, allocation or unmap. On success one
 * provider/device reference transfers to the VMA; unpublished failures drain
 * installed PTEs and release each temporary owner before returning.
 */
int64_t syscall_mm_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset)
{
    struct task *task = sched_current_task();
    struct task_file *file = NULL;
    uint64_t mapped_len;
    uint64_t start;
    uint64_t end;
    uint64_t page_flags = 0;
    uint32_t vma_flags;
    uint32_t max_prot = LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC;
    int anonymous;
    int device_mapping = 0;
    int character_file_mapping = 0;
    uint64_t device_phys = 0;
    struct device_mapping *device_backing=NULL;
    int ret;

    if (!task || task->kind != TASK_KIND_USER) {
        return -RELIEFOS_EINVAL;
    }
    if (align_user_len(len, &mapped_len) < 0) {
        return -RELIEFOS_EINVAL;
    }
    if ((prot & ~(uint64_t)(LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC)) != 0) {
        return -RELIEFOS_EINVAL;
    }
    if ((prot & (LINUX_PROT_WRITE | LINUX_PROT_EXEC)) ==
        (LINUX_PROT_WRITE | LINUX_PROT_EXEC)) {
        return -RELIEFOS_EACCES;
    }
    if ((flags & ~((uint64_t)LINUX_MAP_SUPPORTED)) != 0) {
        return -RELIEFOS_EINVAL;
    }
    anonymous = (flags & LINUX_MAP_ANONYMOUS) != 0;
    if (!(flags & (LINUX_MAP_PRIVATE | LINUX_MAP_SHARED)) ||
        (anonymous && (flags & (LINUX_MAP_PRIVATE | LINUX_MAP_SHARED)) ==
                       (LINUX_MAP_PRIVATE | LINUX_MAP_SHARED)) ||
        (offset & (PAGE_SIZE - 1ULL))) {
        return -RELIEFOS_EINVAL;
    }
    vma_flags = anonymous ? TASK_VMA_FLAG_ANON :
        (TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY | TASK_VMA_FLAG_MMAP |
         ((flags & LINUX_MAP_SHARED) ? TASK_VMA_FLAG_SHARED_FILE : TASK_VMA_FLAG_PRIVATE));
    if (flags & LINUX_MAP_SHARED) vma_flags |= TASK_VMA_FLAG_SHARED;
    if (!anonymous) {
        if (offset > UINT64_MAX - mapped_len) {
            return -RELIEFOS_EINVAL;
        }
        if (fd > INT32_MAX) {
            return -RELIEFOS_EBADF;
        }
        file = task_file_for_io(task, (int)fd);
        if (!file) {
            return -RELIEFOS_EBADF;
        }
        if (file->character_file) {
            if (!file_can_read(file)) return -RELIEFOS_EACCES;
            if ((flags&LINUX_MAP_SHARED) && !file_can_write(file))max_prot&=~LINUX_PROT_WRITE;
            max_prot&=~LINUX_PROT_EXEC;
            character_file_mapping=1;
            vma_flags=TASK_VMA_FLAG_DEVICE|TASK_VMA_FLAG_SHARED|TASK_VMA_FLAG_MMAP;
        } else {
            if (!file_can_read(file)) return -RELIEFOS_EACCES;
            if(!(file->flags&TASK_FILE_FLAG_DEV_NODE)) {
                if(file->node.type!=RELIEFOS_FS_TYPE_FILE)return -RELIEFOS_EINVAL;
                /* Common syscall admission pins this exact OFD through
                 * metadata waits and fd reuse. The provider owns identity;
                 * refresh no copied bare node or diagnostic pathname. */
                storage_set_io_async_context(false);
                struct linux_stat_abi status;
                int refresh=vfs_path_stat(&file->path_ref,&status);
                if(refresh)return refresh;
                if(status.st_size<0)return -LINUX_EIO;
                file->node.size=(uint64_t)status.st_size;
            }
            if ((flags & LINUX_MAP_SHARED) && !file_can_write(file)) {
                max_prot &= ~LINUX_PROT_WRITE;
                if (prot & LINUX_PROT_WRITE) return -RELIEFOS_EACCES;
            }
        }
        if (!character_file_mapping && !(file->flags&TASK_FILE_FLAG_DEV_NODE)) {
            uint64_t mount_flags=vfs_path_mount_flags(&file->path_ref);
            if ((mount_flags & MS_RDONLY) && (flags & LINUX_MAP_SHARED)) {
                if (prot & LINUX_PROT_WRITE) return -RELIEFOS_EROFS;
                max_prot &= ~LINUX_PROT_WRITE;
            }
            if (mount_flags & MS_NOEXEC) {
                if (prot & LINUX_PROT_EXEC) return -RELIEFOS_EPERM;
                max_prot &= ~LINUX_PROT_EXEC;
            }
        }
        if (file->flags & TASK_FILE_FLAG_DEV_SHM) {
            int shm_ret = task_shm_mmap_check(file, (uint32_t)flags, (uint32_t)prot, &max_prot);
            if (shm_ret < 0) return shm_ret;
        }
        if (!character_file_mapping &&
            (file->flags & TASK_FILE_FLAG_DEV_NODE)) {
            uint64_t bytes;
            if (file->node.first_cluster == STORAGE_DEV_KIND_ZERO) {
                anonymous = 1;
                vma_flags = TASK_VMA_FLAG_ANON |
                    ((flags & LINUX_MAP_SHARED) ? TASK_VMA_FLAG_SHARED : 0);
            } else if (file->node.first_cluster == STORAGE_DEV_KIND_FB0) {
                const struct framebuffer *fb = framebuffer_get();
                if (!fb || !fb->available || !fb->pixels) {
                    return -RELIEFOS_EINVAL;
                }
                /* The reserved VRAM includes the final partial page. */
                bytes = align_up_page(fb->max_bytes ? fb->max_bytes
                                                    : (uint64_t)fb->pitch * fb->height);
                if (offset > bytes || mapped_len > bytes - offset) {
                    return -RELIEFOS_EINVAL;
                }
                device_phys = mm_virt_to_phys(fb->pixels);
                if (!device_phys || offset > UINT64_MAX - device_phys) {
                    return -RELIEFOS_EINVAL;
                }
                device_phys += offset;
                if (!paging_kernel_direct_map_range(device_phys, mapped_len) ||
                    (device_phys & (PAGE_SIZE - 1ULL))) {
                    return -RELIEFOS_EINVAL;
                }
                device_mapping = 1;
            } else {
                return -RELIEFOS_EINVAL;
            }
            if (device_mapping && !character_file_mapping) vma_flags = TASK_VMA_FLAG_DEVICE;
        } else if (!character_file_mapping && file->node.type != RELIEFOS_FS_TYPE_FILE) {
            return -RELIEFOS_EINVAL;
        }
    }

    if (flags & LINUX_MAP_FIXED_NOREPLACE) {
        flags |= LINUX_MAP_FIXED;
    }
    if (flags & LINUX_MAP_FIXED) {
        if ((addr & (PAGE_SIZE - 1ULL)) != 0) {
            return -RELIEFOS_EINVAL;
        }
        start = addr;
    } else {
        start = anonymous ? task_find_mmap_region(task, mapped_len)
                          : task_find_file_mmap_region(task, mapped_len);
        if (!start) {
            console_warn("[reliefnt] mmap virtual range unavailable pid=%u bytes=%llu "
                           "top=0x%llx vmas=%u\n",
                           task->pid,
                           (unsigned long long)mapped_len,
                           (unsigned long long)task_mmap_top(task),
                           task_vma_count(task));
            return -RELIEFOS_ENOMEM;
        }
    }
    if (start < RELIEFNT_USER_BASE || start >= RELIEFNT_USER_TOP || mapped_len > RELIEFNT_USER_TOP - start) {
        return -RELIEFOS_EINVAL;
    }
    end = start + mapped_len;
    if (end > task_mmap_top(task)) {
        return (flags & LINUX_MAP_FIXED) ? -RELIEFOS_EINVAL : -RELIEFOS_ENOMEM;
    }
    if (character_file_mapping) {
        ret=character_file_mmap(file->character_file,task,file,offset,mapped_len,(uint32_t)prot,flags,&device_backing);
        if(ret<0)return ret;
        if(device_mapping_info(device_backing)->length<mapped_len) {
            device_mapping_put(device_backing);return -RELIEFOS_EINVAL;
        }
        max_prot &= device_mapping_info(device_backing)->max_prot;
        device_phys=device_mapping_physical(device_backing,0);
        if(!device_phys || (prot&~max_prot)) {
            device_mapping_put(device_backing);return !device_phys ? -RELIEFOS_EINVAL : -RELIEFOS_EACCES;
        }
        device_mapping=1;
    }

    if (flags & LINUX_MAP_FIXED) {
        if ((flags & LINUX_MAP_FIXED_NOREPLACE) && !task_user_pages_free(task, start, end)) {
            device_mapping_put(device_backing);
            return -RELIEFOS_EEXIST;
        }
        if (!task_address_space_can_map(task, start, end)) {
            device_mapping_put(device_backing);
            return -RELIEFOS_ENOMEM;
        }
        int unmap_ret = syscall_mm_munmap(start, mapped_len);
        if (!(flags & LINUX_MAP_FIXED_NOREPLACE) && unmap_ret < 0 && unmap_ret != -RELIEFOS_EINVAL) {
            device_mapping_put(device_backing);
            return unmap_ret;
        }
    } else if (!task_user_pages_free(task, start, end)) {
        device_mapping_put(device_backing);
        return -RELIEFOS_ENOMEM;
    }
    if (!(flags & LINUX_MAP_FIXED) && !task_address_space_can_map(task, start, end)) {
        device_mapping_put(device_backing);
        return -RELIEFOS_ENOMEM;
    }
    if (!task_vma_can_record_mapping(task, start, end, prot, vma_flags,
                                     file ? &file->node : NULL, offset, max_prot,
                                     (vma_flags&TASK_VMA_FLAG_FILE) ? file : NULL)) {
        console_warn("[reliefnt] mmap VMA slots exhausted pid=%u bytes=%llu vmas=%u\n",
                       task->pid,
                       (unsigned long long)mapped_len,
                       task_vma_count(task));
        device_mapping_put(device_backing);
        return -RELIEFOS_ENOMEM;
    }


    if (prot & LINUX_PROT_WRITE) {
        page_flags |= RELIEFNT_PAGE_WRITABLE;
    }
    if (!(prot & LINUX_PROT_EXEC)) {
        page_flags |= RELIEFNT_PAGE_NOEXEC;
    }
    if (prot == LINUX_PROT_NONE) page_flags |= RELIEFNT_PAGE_PROTNONE;
    if (vma_flags & TASK_VMA_FLAG_SHARED) page_flags |= RELIEFNT_PAGE_SHARED;
    if (device_mapping) {
        uint64_t phys = device_phys;
        for (uint64_t page = start; page < end; page += PAGE_SIZE, phys += PAGE_SIZE) {
            uint64_t actual=device_backing ? device_mapping_physical(device_backing,page-start) : phys;
            if (!actual || !address_space_map_user_page(sched_task_as(task), page, actual,
                                             page_flags | RELIEFNT_PAGE_DEVICE)) {
                task_unmap_pages(task, start, page);
                device_mapping_put(device_backing);
                return -RELIEFOS_ENOMEM;
            }
        }
        ret = 0;
    } else if (anonymous && (flags & LINUX_MAP_NORESERVE) == 0 &&
               (task->mlockall_flags & LINUX_MCL_FUTURE)) {
        /* MCL_FUTURE promises resident pages for future mappings. */
        ret = task_map_anonymous_pages(task, start, end, page_flags);
    } else if (anonymous) {
        ret = 0;
    } else {
        ret = 0;
    }
    if (ret < 0) {
        device_mapping_put(device_backing);
        return ret;
    }

    if (task_vma_record_mapping(task, start, end, prot, vma_flags,
                                character_file_mapping ? NULL :
                                    (file ? &file->node : NULL),
                                offset, max_prot,
                                (vma_flags&TASK_VMA_FLAG_FILE) ? file : NULL) < 0) {
        task_unmap_pages(task, start, end);
        device_mapping_put(device_backing);
        return -RELIEFOS_ENOMEM;
    }
    if (device_mapping) {
        struct task_vma *device_vma = task_vma_containing(task, start, end);
        if (!device_vma) {
            task_unmap_pages(task, start, end);
            device_mapping_put(device_backing);
            return -RELIEFOS_ENOMEM;
        }
        device_vma->device_physical = device_phys;

    }
    if (device_backing) {
        struct task_vma *vma=task_vma_containing(task,start,end);
        if(!vma){task_unmap_pages(task,start,end);device_mapping_put(device_backing);return -RELIEFOS_ENOMEM;}
        vma->device_mapping=device_backing;vma->device_map_offset=0;device_backing=NULL;
    }
    if (task->mlockall_flags & LINUX_MCL_FUTURE) {
        struct task_vma *new_vma = task_vma_containing(task, start, end);
        if (new_vma) new_vma->flags |= TASK_VMA_FLAG_LOCKED;
    }
    return (int64_t)start;
}

int64_t syscall_mm_brk(uint64_t requested)
{
    struct task *task = sched_current_task();
    struct task_address_space_state *mm;
    uint64_t base;
    uint64_t current;
    uint64_t old_page;
    uint64_t new_page;
    int64_t mapped;

    if (!task || task->kind != TASK_KIND_USER) return -RELIEFOS_EINVAL;
    mm = sched_task_mm(task);
    base = mm->program_break_base;
    current = mm->program_break;
    if (!base) return current;
    if (!requested) return (int64_t)current;
    /* Linux reports the unchanged break when the requested extension cannot
     * be committed; callers distinguish this from a negative errno return. */
    if (requested < base || requested >= task_mmap_top(task)) return (int64_t)current;
    if (requested == current) return (int64_t)current;

    old_page = align_up_page(current);
    new_page = align_up_page(requested);
    if (new_page > old_page) {
        /* MAP_FIXED intentionally replaces mappings. brk(2) must first reject
         * collisions so an adjacent mmap cannot be destroyed by heap growth. */
        if (!task_user_pages_free(task, old_page, new_page)) {
            return (int64_t)current;
        }
        mapped = syscall_mm_mmap(old_page, new_page - old_page,
                                 LINUX_PROT_READ | LINUX_PROT_WRITE,
                                 LINUX_MAP_PRIVATE | LINUX_MAP_FIXED |
                                 LINUX_MAP_ANONYMOUS, UINT64_MAX, 0);
        if (mapped != (int64_t)old_page) return (int64_t)current;
    } else if (new_page < old_page) {
        if (syscall_mm_munmap(new_page, old_page - new_page) < 0) {
            return (int64_t)current;
        }
    }
    mm->program_break = requested;
    return (int64_t)requested;
}

/** @brief Resize anonymous memory or shrink/move a movable retained device VMA.
 * @param old_addr Page-aligned source address in the current execution-owned MM.
 * @param old_len Nonzero source bytes. @param new_len Nonzero destination bytes.
 * @param flags Linux MAYMOVE/FIXED flags; FIXED requires MAYMOVE.
 * @param new_addr Aligned destination for FIXED, ignored otherwise.
 * @return Resulting user address or negative errno. Device mappings never grow
 * beyond their initial PFN mapping (EFAULT); fixed moves require the complete
 * VMA and resolve each actual provider page. A failed destination leaves source
 * PTEs/backing retained. No graph/module lock spans allocation or final puts.
 */
int64_t syscall_mm_mremap(uint64_t old_addr, uint64_t old_len, uint64_t new_len,
                          uint64_t flags, uint64_t new_addr)
{
    struct task *task = sched_current_task();
    struct task_vma *vma;
    uint64_t old_mapped, new_mapped, old_end, new_end;
    uint32_t map_flags;
    int64_t moved;

    if (!task || task->kind != TASK_KIND_USER ||
        (flags & ~(uint64_t)(LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED)) ||
        !old_len || !new_len || (old_addr & (PAGE_SIZE - 1ULL))) {
        return -RELIEFOS_EINVAL;
    }
    if ((flags & LINUX_MREMAP_FIXED) && !(flags & LINUX_MREMAP_MAYMOVE)) {
        return -RELIEFOS_EINVAL;
    }
    if (align_user_len(old_len, &old_mapped) < 0 ||
        align_user_len(new_len, &new_mapped) < 0 ||
        old_addr < RELIEFNT_USER_BASE || old_addr >= RELIEFNT_USER_TOP ||
        old_mapped > RELIEFNT_USER_TOP - old_addr) {
        return -RELIEFOS_EINVAL;
    }
    old_end = old_addr + old_mapped;
    vma = task_vma_containing(task, old_addr, old_end);
    if (!vma) return -RELIEFOS_EFAULT;
    if (vma->device_mapping && device_mapping_info(vma->device_mapping)->can_move) {
        if (new_mapped == old_mapped && !(flags & LINUX_MREMAP_FIXED))
            return (int64_t)old_addr;
        if (new_mapped > old_mapped)
            return -RELIEFOS_EFAULT;
        if (!(flags & LINUX_MREMAP_FIXED)) {
            return syscall_mm_munmap(old_addr + new_mapped,
                                     old_mapped - new_mapped) < 0
                       ? -RELIEFOS_ENOMEM : (int64_t)old_addr;
        }

        /* Linux TTM marks these VM_PFNMAP | VM_DONTEXPAND. It permits a
         * shrink and a fixed move, but rejects growth with EFAULT. Keep the
         * initial implementation's move atomic at the VMA level: a fixed
         * move must name the complete device VMA so no segment loses its object
         * offset or independent backing reference. */
        uint64_t source_start = vma->start;
        uint64_t source_end = vma->end;
        uint64_t source_bytes = source_end - source_start;
        uint64_t destination_end;
        uint32_t source_index = UINT32_MAX;
        uint32_t destination_index;
        if (old_addr != source_start || old_mapped != source_bytes)
            return -RELIEFOS_EFAULT;
        if (!new_addr || (new_addr & (PAGE_SIZE - 1ULL)) ||
            new_addr < RELIEFNT_USER_BASE || new_addr >= task_mmap_top(task) ||
            new_mapped > task_mmap_top(task) - new_addr)
            return -RELIEFOS_EINVAL;
        destination_end = new_addr + new_mapped;
        if (new_addr < source_end && source_start < destination_end)
            return -RELIEFOS_EINVAL;
        for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
            struct task_vma *entry = sched_task_vma_at(task, i);
            if (entry == vma) {
                source_index = i;
                break;
            }
        }
        if (source_index == UINT32_MAX) return -RELIEFOS_EFAULT;

        /* MREMAP_FIXED replaces its destination. Do that before checking the
         * final address-space accounting, while keeping the source pinned. */
        int unmap_ret = syscall_mm_munmap(new_addr, new_mapped);
        if (unmap_ret < 0) return unmap_ret;
        if (!task_user_pages_free(task, new_addr, destination_end))
            return -RELIEFOS_ENOMEM;
        uint64_t used = task_vma_total_bytes(task);
        uint64_t limit = sched_task_limits(task)->as.rlim_cur &
                         ~(PAGE_SIZE - 1ULL);
        if (sched_task_limits(task)->as.rlim_cur != LINUX_RLIM_INFINITY &&
            (used < source_bytes ||
             used - source_bytes > limit ||
             new_mapped > limit - (used - source_bytes)))
            return -RELIEFOS_ENOMEM;
        if (task_vma_free_slot_index(task, &destination_index) < 0)
            return -RELIEFOS_ENOMEM;

        vma = sched_task_vma_at(task, source_index);
        if (!vma || !vma->device_mapping)
            return -RELIEFOS_EFAULT;
        struct task_vma moved_vma = *vma;
        uint64_t physical = vma->device_physical;
        uint64_t page_flags = RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_SHARED;
        if (vma->prot & LINUX_PROT_WRITE) page_flags |= RELIEFNT_PAGE_WRITABLE;
        if (!(vma->prot & LINUX_PROT_EXEC)) page_flags |= RELIEFNT_PAGE_NOEXEC;
        if (vma->prot == LINUX_PROT_NONE) page_flags |= RELIEFNT_PAGE_PROTNONE;
        for (uint64_t page = 0; page < new_mapped; page += PAGE_SIZE) {
            uint64_t actual=device_mapping_physical(vma->device_mapping,vma->device_map_offset+page);
            if (!actual || !address_space_map_user_page(sched_task_as(task), new_addr + page,
                                             actual, page_flags)) {
                task_unmap_pages(task, new_addr, new_addr + page);
                return -RELIEFOS_ENOMEM;
            }
        }

        int retain=device_mapping_get(vma->device_mapping);
        if(retain) {
            task_unmap_pages(task,new_addr,destination_end);
            return retain;
        }
        moved_vma.used = 1;
        moved_vma.start = new_addr;
        moved_vma.end = destination_end;
        moved_vma.device_physical = physical;
        struct task_vma *destination = sched_task_vma_at(task, destination_index);
        if (!destination || destination->used) {
            task_unmap_pages(task, new_addr, destination_end);
            device_mapping_put(moved_vma.device_mapping);
            return -RELIEFOS_ENOMEM;
        }
        *destination = moved_vma;
        task_unmap_pages(task, source_start, source_end);
        vma = sched_task_vma_at(task, source_index);
        if (vma) task_vma_clear(vma);
        return (int64_t)new_addr;
    }
    /* File/device mappings still need Linux backing-file readahead/writeback. */
    if (!(vma->flags & TASK_VMA_FLAG_ANON)) return -RELIEFOS_ENOMEM;
    if (new_mapped == old_mapped) return (int64_t)old_addr;
    if (new_mapped < old_mapped) {
        return syscall_mm_munmap(old_addr + new_mapped, old_mapped - new_mapped) < 0
                   ? -RELIEFOS_ENOMEM : (int64_t)old_addr;
    }

    new_end = old_addr + new_mapped;
    if (new_end <= task_mmap_top(task) &&
        task_user_pages_free(task, old_end, new_end)) {
        map_flags = (vma->flags & TASK_VMA_FLAG_SHARED) ? LINUX_MAP_SHARED : LINUX_MAP_PRIVATE;
        map_flags |= LINUX_MAP_ANONYMOUS | LINUX_MAP_FIXED;
        moved = syscall_mm_mmap(old_end, new_mapped - old_mapped, vma->prot,
                                map_flags, UINT64_MAX, 0);
        if (moved == (int64_t)old_end) return (int64_t)old_addr;
    }
    if (!(flags & LINUX_MREMAP_MAYMOVE)) return -RELIEFOS_ENOMEM;
    if (flags & LINUX_MREMAP_FIXED) {
        if (!new_addr || (new_addr & (PAGE_SIZE - 1ULL)) ||
            new_addr < RELIEFNT_USER_BASE || new_addr >= RELIEFNT_USER_TOP ||
            new_mapped > RELIEFNT_USER_TOP - new_addr ||
            (new_addr < old_end && old_addr < new_addr + new_mapped)) {
            return -RELIEFOS_EINVAL;
        }
    } else {
        new_addr = 0;
    }
    map_flags = (vma->flags & TASK_VMA_FLAG_SHARED) ? LINUX_MAP_SHARED : LINUX_MAP_PRIVATE;
    map_flags |= LINUX_MAP_ANONYMOUS;
    if (flags & LINUX_MREMAP_FIXED) map_flags |= LINUX_MAP_FIXED;
    moved = syscall_mm_mmap(new_addr, new_mapped, vma->prot, map_flags, UINT64_MAX, 0);
    if (moved < 0) return moved;
    __builtin_memcpy((void *)(uintptr_t)moved, (const void *)(uintptr_t)old_addr,
                     old_mapped < new_mapped ? old_mapped : new_mapped);
    if (syscall_mm_munmap(old_addr, old_mapped) < 0) {
        (void)syscall_mm_munmap((uint64_t)moved, new_mapped);
        return -RELIEFOS_EFAULT;
    }
    return moved;
}

/** @brief Publish one SysV attachment using the existing VMA and PTE allocator.
 * @param task Current user MM, pinned by execution ownership.
 * @param address Exact aligned address, or zero to find a free range.
 * @param length Page-rounded bytes. @param prot Initial protection.
 * @param max_prot Ceiling for later mprotect. @param remap Replace existing range.
 * @param attachment Prepared identity, borrowed until successful publication.
 * @param pages Registry-owned backing; each PTE obtains a physical page reference.
 * @return Address or negative errno. All PTE refs acquired here are rolled back
 * on failure; registry backing and attachment ownership stay with the caller.
 */
int64_t syscall_mm_map_sysv_shm(struct task *task, uint64_t address, uint64_t length,
    uint32_t prot, uint32_t max_prot, bool remap,
    struct sysv_shm_attachment *attachment, const uint64_t *pages)
{
    if (!task || task != sched_current_task() || task->kind != TASK_KIND_USER ||
        !length || (length & (PAGE_SIZE - 1)) || !attachment || !pages)
        return -RELIEFOS_EINVAL;
    uint64_t start = address ? address : task_find_mmap_region(task, length);
    if (!start) return -RELIEFOS_ENOMEM;
    if ((start & (PAGE_SIZE - 1)) || start < RELIEFNT_USER_BASE ||
        start >= task_mmap_top(task) || length > task_mmap_top(task) - start ||
        (start < RELIEFNT_KERNEL_HOLE_END && start + length > RELIEFNT_KERNEL_HOLE_START))
        return -RELIEFOS_EINVAL;
    uint64_t end = start + length;
    if (!remap && !task_user_pages_free(task, start, end)) return -RELIEFOS_EINVAL;
    if (!task_address_space_can_map(task, start, end)) return -RELIEFOS_ENOMEM;
    /* Reserve the metadata slot by index; SHM_REMAP may grow vma_extra. */
    uint32_t slot_index;
    if (task_vma_free_slot_index(task, &slot_index) < 0) return -RELIEFOS_ENOMEM;
    struct task_vma *slot = sched_task_vma_at(task, slot_index);
    if (!slot) return -RELIEFOS_ENOMEM;
    slot->used = 0xffffffffu;
    if (!address_space_prepare_user_range(sched_task_as(task), start, end)) {
        slot = sched_task_vma_at(task, slot_index);
        if (slot) *slot = (struct task_vma){0};
        return -RELIEFOS_ENOMEM;
    }
    if (remap) {
        /* The reservation is outside the range, so munmap never clears it. */
        int ret = (int)syscall_mm_munmap(start, length);
        slot = sched_task_vma_at(task, slot_index);
        if (ret) {
            if (slot) *slot = (struct task_vma){0};
            return ret;
        }
        if (!slot) return -RELIEFOS_ENOMEM;
    }
    uint64_t page_flags = RELIEFNT_PAGE_SHARED;
    if (prot & LINUX_PROT_WRITE) page_flags |= RELIEFNT_PAGE_WRITABLE;
    if (!(prot & LINUX_PROT_EXEC)) page_flags |= RELIEFNT_PAGE_NOEXEC;
    uint64_t mapped = 0;
    for (; mapped < length; mapped += PAGE_SIZE) {
        uint64_t physical = pages[mapped / PAGE_SIZE];
        mm_retain_page(physical);
        if (!address_space_map_user_page(sched_task_as(task), start + mapped,
                                         physical, page_flags)) {
            mm_free_page(physical);
            task_unmap_pages(task, start, start + mapped);
            slot = sched_task_vma_at(task, slot_index);
            if (slot) *slot = (struct task_vma){0};
            return -RELIEFOS_ENOMEM;
        }
    }
    slot = sched_task_vma_at(task, slot_index);
    if (!slot) {
        task_unmap_pages(task, start, end);
        return -RELIEFOS_ENOMEM;
    }
    *slot = (struct task_vma){.used = 1, .start = start, .end = end,
        .prot = prot, .max_prot = max_prot,
        .flags = TASK_VMA_FLAG_SHARED | TASK_VMA_FLAG_SYSV_SHM,
        .sysv_shm_attachment = attachment};
    return (int64_t)start;
}
