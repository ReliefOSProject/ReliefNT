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
        if (!address_space_user_entry_next(sched_task_as(task), start, end,
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
        while (address_space_user_entry_next(sched_task_as(task), cursor, end,
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

/** @brief Admit a future-locked mapping against the final net MM quota.
 * @param task Execution-owned user MM. @param start First page. @param end Exclusive end.
 * @param vma_flags Mapping flags updated with lock policy on success.
 * @return Zero or negative EAGAIN before any existing mapping is removed.
 */
static int task_future_lock_mapping(struct task *task, uint64_t start, uint64_t end,
                                    uint32_t *vma_flags)
{
    uint32_t mode = mm_lock_flags(sched_task_as(task));
    if (!(mode & LINUX_MCL_FUTURE)) return 0;
    if (!(task->cap_effective & (1ULL << CAP_IPC_LOCK))) {
        uint64_t held = mm_locked_bytes(task), replaced = 0;
        uint64_t limit = sched_task_limits(task)->memlock.rlim_cur & ~(PAGE_SIZE - 1ULL);
        for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
            struct task_vma *old = sched_task_vma_at(task, i);
            if (!old || !old->used || !(old->flags & TASK_VMA_FLAG_LOCKED)) continue;
            uint64_t first = old->start > start ? old->start : start;
            uint64_t last = old->end < end ? old->end : end;
            if (last > first) replaced += last - first;
        }
        if (held < replaced || held - replaced > limit || end - start > limit - (held - replaced))
            return -RELIEFOS_EAGAIN;
    }
    *vma_flags |= TASK_VMA_FLAG_LOCKED;
    if (mode & LINUX_MCL_ONFAULT) *vma_flags |= TASK_VMA_FLAG_LOCKONFAULT;
    return 0;
}

/** @brief Map anonymous/provider bytes or an independently pinned device backing.
 * @param addr Fixed address or placement hint, checked against the user aperture.
 * @param len Nonzero byte length, rounded to complete pages without overflow.
 * @param prot Linux PROT bits, constrained by descriptor/provider access and W^X.
 * @param flags Linux mmap flags, including validated fixed/no-replace semantics.
 * @param fd Original OFD admitted by the syscall, ignored for anonymous mappings.
 * @param offset Page-aligned file offset or provider mapping selector.
 * @param populate Attempt best-effort eager population after publication.
 * @param tag_io_retry Return the private marker for retryable metadata I/O.
 * @return User address, negative errno, or the private metadata retry marker.
 * Execution-owned current MM required;
 * no graph/module lock spans callbacks, allocation or unmap. On success one
 * provider/device reference transfers to the VMA; typed DEVICE backing owns
 * matching physical cache policies through every VMA and staged-I/O reference.
 * Unpublished failures drain installed PTEs and release each temporary owner.
 */
static int64_t mm_mmap_internal(uint64_t addr, uint64_t len, uint64_t prot,
                                uint64_t flags, uint64_t fd, uint64_t offset,
                                bool populate, bool tag_io_retry)
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
    /* Linux permits explicit W|X for ordinary mappings. File/mount/device
     * restrictions below still determine this VMA's maximum permissions. */
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
                if(refresh) return tag_io_retry && refresh == -RELIEFOS_EAGAIN
                    ? KERNEL_MMAP_FILE_IO_RETRY : refresh;
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
#ifdef CONFIG_SWAP
            /* A claimed swap inode cannot admit a shared cache frame, even
             * through a read-only or inaccessible mapping. */
            if ((flags&LINUX_MAP_SHARED) &&
                storage_inode_swap_active(file->inode)) return -LINUX_ETXTBSY;
#endif
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

    uint32_t lock_mode = mm_lock_flags(sched_task_as(task));
    if (task_future_lock_mapping(task, start, end, &vma_flags) < 0) {
        device_mapping_put(device_backing);
        return -RELIEFOS_EAGAIN;
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
    } else if (!task_user_pages_free(task, start, end)) {
        device_mapping_put(device_backing);
        return -RELIEFOS_ENOMEM;
    }
    if (!(flags & LINUX_MAP_FIXED) && !task_address_space_can_map(task, start, end)) {
        device_mapping_put(device_backing);
        return -RELIEFOS_ENOMEM;
    }
    /* The provider may return an already-owned backing without allocating.
     * Reserve every new view and physical-policy owner before MAP_FIXED can
     * destroy the old target. The execution writer keeps the chosen policies
     * stable until PTE installation or exact admission rollback. */
    if (device_backing) {
        ret=device_mapping_admit_view(&device_backing);
        if(ret<0){device_mapping_put(device_backing);return ret;}
        for(uint64_t offset_in_view=0;offset_in_view<mapped_len;offset_in_view+=PAGE_SIZE) {
            uint64_t actual=device_mapping_physical(device_backing,offset_in_view),cache;
            if(!actual) { device_mapping_put(device_backing);return -RELIEFOS_ENOMEM; }
            ret=device_mapping_cache_page(device_backing,offset_in_view,actual,&cache);
            if(ret<0){device_mapping_put(device_backing);return ret;}
        }
    }
    if (flags & LINUX_MAP_FIXED) {
        int unmap_ret = syscall_mm_munmap(start, mapped_len);
        if (!(flags & LINUX_MAP_FIXED_NOREPLACE) && unmap_ret < 0 && unmap_ret != -RELIEFOS_EINVAL) {
            device_mapping_put(device_backing);
            return unmap_ret;
        }
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
            uint64_t cache=0;
            /* Typed backing owns the physical policy through all VMA/stage
             * references. FB0's boot/display policy and backing are permanent. */
            if(actual && device_backing) {
                ret=device_mapping_cache_page(device_backing,page-start,actual,&cache);
                if(ret<0) {
                    task_unmap_pages(task,start,page);
                    device_mapping_put(device_backing);
                    return ret;
                }
            } else if(actual) (void)paging_kernel_direct_cache_flags(actual,&cache);
            if (!actual || !address_space_map_user_page(sched_task_as(task), page, actual,
                                             page_flags | RELIEFNT_PAGE_DEVICE | cache)) {
                task_unmap_pages(task, start, page);
                device_mapping_put(device_backing);
                return -RELIEFOS_ENOMEM;
            }
        }
        ret = 0;
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
    if (anonymous && (flags & LINUX_MAP_SHARED) && !device_mapping &&
        mm_shared_create(sched_task_as(task), start, end) < 0) {
        (void)syscall_mm_munmap(start, end - start);
        return -RELIEFOS_ENOMEM;
    }
    if (populate && (lock_mode & LINUX_MCL_FUTURE) && !(lock_mode & LINUX_MCL_ONFAULT))
        (void)mm_populate_locked_range(start, end, false);
    return (int64_t)start;
}

/** @brief Map bytes and opportunistically populate any FUTURE-locked range.
 * @param addr Address hint. @param len Bytes. @param prot Protection bits.
 * @param flags Mapping flags. @param fd File descriptor. @param offset File offset.
 * @return Mapped address or negative errno. All publication precedes a possible
 * execution suspension; no borrowed VMA or movable MM pointer survives it.
 */
int64_t syscall_mm_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset)
{
    return mm_mmap_internal(addr, len, prot, flags, fd, offset, true, false);
}

/** @brief Map for the syscall frame with a distinguishable metadata I/O wait.
 * @param addr Address hint. @param len Bytes. @param prot Protection bits.
 * @param flags Mapping flags. @param fd File descriptor. @param offset File offset.
 * @return Address, negative errno, or the private file-I/O marker consumed by
 * syscall entry. Quota and provider errors remain ordinary final errnos.
 */
int64_t syscall_mm_mmap_dispatch(uint64_t addr, uint64_t len, uint64_t prot,
                                uint64_t flags, uint64_t fd, uint64_t offset)
{
    return mm_mmap_internal(addr, len, prot, flags, fd, offset, true, true);
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
    bool populate = false;

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
        uint32_t mode = mm_lock_flags(sched_task_as(task));
        populate = (mode & LINUX_MCL_FUTURE) && !(mode & LINUX_MCL_ONFAULT);
        mapped = mm_mmap_internal(old_page, new_page - old_page,
                                 LINUX_PROT_READ | LINUX_PROT_WRITE,
                                 LINUX_MAP_PRIVATE | LINUX_MAP_FIXED |
                                 LINUX_MAP_ANONYMOUS, UINT64_MAX, 0, false, false);
        if (mapped != (int64_t)old_page) return (int64_t)current;
    } else if (new_page < old_page) {
        if (syscall_mm_munmap(new_page, old_page - new_page) < 0) {
            return (int64_t)current;
        }
    }
    mm->program_break = requested;
    mm = NULL; /* No pointer into movable inline MM crosses population. */
    if (populate) (void)mm_populate_locked_range(old_page, new_page, false);
    return (int64_t)requested;
}

/** @brief Check net locked-byte cost before a locked VMA is remapped.
 * @param task Execution-owned MM. @param old_len Locked bytes removed at source.
 * @param new_len Locked bytes installed at destination. @param target Optional fixed target.
 * @return True when capability or memlock quota admits the final intervals.
 */
static bool task_remap_lock_allowed(struct task *task, uint64_t old_len,
                                    uint64_t new_len, uint64_t target)
{
    if (task->cap_effective & (1ULL << CAP_IPC_LOCK)) return true;
    uint64_t held = mm_locked_bytes(task), replaced = 0;
    if (target) {
        uint64_t target_end = target + new_len;
        for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
            struct task_vma *vma = sched_task_vma_at(task, i);
            if (!vma || !vma->used || !(vma->flags & TASK_VMA_FLAG_LOCKED)) continue;
            uint64_t first = vma->start > target ? vma->start : target;
            uint64_t last = vma->end < target_end ? vma->end : target_end;
            if (last > first) replaced += last - first;
        }
    }
    if (held < old_len || held - old_len < replaced) return false;
    held -= old_len + replaced;
    uint64_t limit = sched_task_limits(task)->memlock.rlim_cur & ~(PAGE_SIZE - 1ULL);
    return held <= limit && new_len <= limit - held;
}

/** @brief Reserve an anonymous remap without applying FUTURE to an old VMA.
 * @param task Execution-owned MM. @param address Destination or zero for search.
 * @param length Aligned new bytes. @param prot Original VMA permissions.
 * @param flags Original anonymous mapping mode. @return Address or negative errno.
 */
static int64_t task_remap_reserve(struct task *task, uint64_t address, uint64_t length,
                                  uint32_t prot, uint32_t flags)
{
    uint32_t policy = mm_lock_flags(sched_task_as(task));
    if (policy & LINUX_MCL_FUTURE) mm_context_set_lock_flags(sched_task_as(task), 0);
    int64_t result = syscall_mm_mmap(address, length, prot, flags, UINT64_MAX, 0);
    if (policy & LINUX_MCL_FUTURE) mm_context_set_lock_flags(sched_task_as(task), policy);
    return result;
}

/** @brief Stage resident anonymous leaves at a remap destination without user access.
 * @param task Execution-owned MM. @param source First source page.
 * @param target First reserved destination page. @param length Bytes to preserve.
 * @return Zero or negative ENOMEM; the caller removes staged destination leaves
 * on failure. Source leaves stay untouched, and each destination owns a PMM pin.
 * Absent leaves remain lazy; hardware permissions, PROT_NONE and COW are retained.
 */
static int task_remap_stage_leaves(struct task *task, uint64_t source,
                                   uint64_t target, uint64_t length)
{
    const uint64_t mask = RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC |
                          RELIEFNT_PAGE_PROTNONE | RELIEFNT_PAGE_SHARED | RELIEFNT_PAGE_WRITE_NOTIFY |
                          RELIEFNT_PAGE_COW | RELIEFNT_PAGE_PWT |
                          RELIEFNT_PAGE_PCD | RELIEFNT_PAGE_PAT;
    uint64_t cursor = source;
    while (cursor < source + length) {
        uint64_t page, phys, flags;
        if (!address_space_user_entry_next(sched_task_as(task), cursor,
                source + length, &page, &phys, &flags)) break;
        uint64_t destination = target + page - source;
        if (paging_user_entry_kind(sched_task_as(task), page) == PAGING_USER_ENTRY_SWAP) {
            int ret = mm_swap_copy_record(sched_task_as(task), sched_task_as(task),
                                          page, destination);
            if (ret < 0) return ret;
            mm_context_vma_changed(sched_task_as(task));
            if (!address_space_map_user_swap(sched_task_as(task), destination, flags & mask)) {
                mm_swap_record_remove(sched_task_as(task), destination);
                return -RELIEFOS_ENOMEM;
            }
        } else {
            mm_retain_page(phys);
            if (!address_space_map_user_page(sched_task_as(task), destination,
                                             phys, flags & mask)) {
                mm_free_page(phys);
                return -RELIEFOS_ENOMEM;
            }
        }
        cursor = page + PAGE_SIZE;
    }
    return 0;
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

        /* No ownership-count failure may follow replacement of an existing
         * destination. The execution writer keeps this preflight stable. */
        int retain_check=device_mapping_check_range(vma->device_mapping,
                                                     vma->device_map_offset,new_mapped);
        if(retain_check)return retain_check;
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
            uint64_t cache=address_space_user_page_flags(sched_task_as(task),old_addr+page) &
                (RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_PAT);
            if (!actual || !address_space_map_user_page(sched_task_as(task), new_addr + page,
                                             actual, page_flags | cache)) {
                task_unmap_pages(task, new_addr, new_addr + page);
                return -RELIEFOS_ENOMEM;
            }
        }

        int retain=device_mapping_get(vma->device_mapping);
        if(retain) {
            task_unmap_pages(task,new_addr,destination_end);
            return retain;
        }
        retain=device_mapping_retain_range(vma->device_mapping,vma->device_map_offset,new_mapped);
        if(retain) {
            task_unmap_pages(task,new_addr,destination_end);
            device_mapping_put(moved_vma.device_mapping);
            return retain;
        }
        moved_vma.used = 1;
        moved_vma.start = new_addr;
        moved_vma.end = destination_end;
        moved_vma.device_physical = physical;
        struct task_vma *destination = sched_task_vma_at(task, destination_index);
        if (!destination || destination->used) {
            task_unmap_pages(task, new_addr, destination_end);
            device_mapping_release_range(moved_vma.device_mapping,moved_vma.device_map_offset,
                                         new_mapped,false);
            device_mapping_put(moved_vma.device_mapping);
            return -RELIEFOS_ENOMEM;
        }
        mm_context_vma_changed(sched_task_as(task));
        *destination = moved_vma;
        task_unmap_pages(task, source_start, source_end);
        vma = sched_task_vma_at(task, source_index);
        if (vma) task_vma_clear(vma);
        return (int64_t)new_addr;
    }
    /* File/device mappings still need Linux backing-file readahead/writeback. */
    if (!(vma->flags & TASK_VMA_FLAG_ANON)) return -RELIEFOS_ENOMEM;
    uint32_t source_flags = vma->flags;
    uint32_t source_prot = vma->prot;
    bool source_locked = (source_flags & TASK_VMA_FLAG_LOCKED) != 0;
    bool source_onfault = (source_flags & TASK_VMA_FLAG_LOCKONFAULT) != 0;
    if (new_mapped == old_mapped && !(flags & LINUX_MREMAP_FIXED)) return (int64_t)old_addr;
    if (new_mapped < old_mapped && !(flags & LINUX_MREMAP_FIXED)) {
        return syscall_mm_munmap(old_addr + new_mapped, old_mapped - new_mapped) < 0
                   ? -RELIEFOS_ENOMEM : (int64_t)old_addr;
    }

    new_end = old_addr + new_mapped;
    if (!(flags & LINUX_MREMAP_FIXED) && new_end <= task_mmap_top(task) &&
        task_user_pages_free(task, old_end, new_end)) {
        if (source_locked && !task_remap_lock_allowed(task, old_mapped, new_mapped, 0))
            return -RELIEFOS_EAGAIN;
        map_flags = (vma->flags & TASK_VMA_FLAG_SHARED) ? LINUX_MAP_SHARED : LINUX_MAP_PRIVATE;
        map_flags |= LINUX_MAP_ANONYMOUS | LINUX_MAP_FIXED;
        moved = task_remap_reserve(task, old_end, new_mapped - old_mapped, source_prot, map_flags);
        if (moved == (int64_t)old_end) {
            if (source_locked && mm_lock_remap(task, old_end, new_end, source_onfault) < 0) {
                (void)syscall_mm_munmap(old_end, new_mapped - old_mapped);
                return -RELIEFOS_EAGAIN;
            }
            vma = NULL;
            if (source_locked && !source_onfault)
                (void)mm_populate_locked_range(old_end, new_end, false);
            return (int64_t)old_addr;
        }
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
    if (source_locked && !task_remap_lock_allowed(task, old_mapped, new_mapped, new_addr))
        return -RELIEFOS_EAGAIN;
    map_flags = (source_flags & TASK_VMA_FLAG_SHARED) ? LINUX_MAP_SHARED : LINUX_MAP_PRIVATE;
    map_flags |= LINUX_MAP_ANONYMOUS;
    if (flags & LINUX_MREMAP_FIXED) map_flags |= LINUX_MAP_FIXED;
    moved = task_remap_reserve(task, new_addr, new_mapped, source_prot, map_flags);
    if (moved < 0) return moved;
    uint64_t copy_len = old_mapped < new_mapped ? old_mapped : new_mapped;
    if (map_flags & LINUX_MAP_SHARED) {
        if (mm_shared_alias(sched_task_as(task), old_addr, (uint64_t)moved, copy_len) < 0) {
            (void)syscall_mm_munmap((uint64_t)moved, new_mapped);
            return -RELIEFOS_ENOMEM;
        }
    }
    if (task_remap_stage_leaves(task, old_addr, (uint64_t)moved, copy_len) < 0) {
        (void)syscall_mm_munmap((uint64_t)moved, new_mapped);
        return -RELIEFOS_ENOMEM;
    }
    if (source_locked && mm_lock_remap(task, (uint64_t)moved,
                                       (uint64_t)moved + new_mapped, source_onfault) < 0) {
        (void)syscall_mm_munmap((uint64_t)moved, new_mapped);
        return -RELIEFOS_EAGAIN;
    }
    if (syscall_mm_munmap(old_addr, old_mapped) < 0) {
        (void)syscall_mm_munmap((uint64_t)moved, new_mapped);
        return -RELIEFOS_ENOMEM;
    }
    vma = NULL;
    if (source_locked && !source_onfault)
        (void)mm_populate_locked_range((uint64_t)moved, (uint64_t)moved + new_mapped, false);
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
    uint32_t vma_flags = TASK_VMA_FLAG_SHARED | TASK_VMA_FLAG_SYSV_SHM;
    if (task_future_lock_mapping(task, start, end, &vma_flags) < 0) return -RELIEFOS_EAGAIN;
    /* Reserve the metadata slot by index; SHM_REMAP may grow vma_extra. */
    uint32_t slot_index;
    if (task_vma_free_slot_index(task, &slot_index) < 0) return -RELIEFOS_ENOMEM;
    struct task_vma *slot = sched_task_vma_at(task, slot_index);
    if (!slot) return -RELIEFOS_ENOMEM;
    /* Failed table preparation clears this provisional slot under the writer,
     * before any mapping is published. Later unmaps and slot publication mark
     * their own real changes. */
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
    mm_context_vma_changed(sched_task_as(task));
    *slot = (struct task_vma){.used = 1, .start = start, .end = end,
        .prot = prot, .max_prot = max_prot,
        .flags = vma_flags,
        .sysv_shm_attachment = attachment};
    return (int64_t)start;
}
