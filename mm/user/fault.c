#include "memory_internal.h"

/**
 * @brief Clear one allocator page through the kernel direct map.
 * @param phys Physical page address returned by the allocator.
 */
static void zero_phys_page(uint64_t phys);

static int task_map_anonymous_page(struct task *task, struct task_vma *vma,
                                   uint64_t page);

static int load_file_cache_page(uint64_t phys, void *opaque);

static int task_map_file_vma_page(struct task *task, const struct task_vma *vma,
                                  uint64_t page);

/**
 * @brief RLIMIT_STACK/RLIMIT_AS admission for a new lowest stack page.
 *
 * Mirrors Linux mm/mmap.c:acct_stack_growth(): @size is the whole growable
 * stack VMA span (top - candidate start) and the stack test is
 * `size > rlimit(RLIMIT_STACK)`; RLIM_INFINITY disables only that test.  The
 * address-space limit stays a cumulative virtual-memory check.
 */
static bool task_stack_fault_candidate(const struct task *task, uint64_t page,
                                       uint64_t error);

static bool task_stack_growth_allowed(const struct task *task, uint64_t page);


/**
 * Zero phys page.
 * @param phys Value supplied by the caller.
 */
static void zero_phys_page(uint64_t phys)
{
    uint8_t *ptr = (uint8_t *)mm_phys_to_virt(phys);
    if (!ptr) return;
    for (uint64_t i = 0; i < PAGE_SIZE; ++i) {
        ptr[i] = 0;
    }
}

/**
 * Task map anonymous pages.
 * @param task Value supplied by the caller.
 * @param start Value supplied by the caller.
 * @param end Value supplied by the caller.
 * @param page_flags Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int task_map_anonymous_pages(struct task *task, uint64_t start, uint64_t end,
                                    uint64_t page_flags)
{
    /* Anonymous mappings have no executable provenance.  An application that
     * needs JIT code must explicitly map RX after producing it; W+X is never
     * accepted by the syscall. */
    page_flags |= RELIEFNT_PAGE_NOEXEC;
    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        uint64_t phys = mm_alloc_page();
        if (!phys || !address_space_map_user_page(sched_task_as(task), page, phys, page_flags)) {
            uint64_t mapped_kib = (page - start) / 1024ULL;
            console_warn("[reliefnt] anonymous mmap failed pid=%u range=0x%llx-0x%llx "
                           "at=0x%llx mapped=%llu KiB free=%llu KiB vmas=%u cause=%s\n",
                           task ? task->pid : 0,
                           (unsigned long long)start,
                           (unsigned long long)end,
                           (unsigned long long)page,
                           (unsigned long long)mapped_kib,
                           (unsigned long long)mm_free_memory_kib(),
                           task_vma_count(task),
                           phys ? "page-table" : "physical-memory");
            if (phys) {
                mm_free_page(phys);
            }
            task_unmap_pages(task, start, page);
            return -RELIEFOS_ENOMEM;
        }
    }
    return 0;
}

static int task_map_anonymous_page(struct task *task, struct task_vma *vma,
                                   uint64_t page)
{
    uint64_t phys;
    uint64_t flags = 0;
    if (!task || !vma || !(vma->flags & TASK_VMA_FLAG_ANON) ||
        vma->prot == LINUX_PROT_NONE) {
        return -RELIEFOS_EFAULT;
    }
    phys = mm_alloc_page();
    if (!phys) return -RELIEFOS_ENOMEM;
    zero_phys_page(phys);
    if (vma->prot & LINUX_PROT_WRITE) flags |= RELIEFNT_PAGE_WRITABLE;
    if (!(vma->prot & LINUX_PROT_EXEC)) flags |= RELIEFNT_PAGE_NOEXEC;
    if (vma->flags & TASK_VMA_FLAG_SHARED) flags |= RELIEFNT_PAGE_SHARED;
    if (!address_space_map_user_page(sched_task_as(task), page, phys, flags)) {
        mm_free_page(phys);
        return -RELIEFOS_ENOMEM;
    }
    return 0;
}

/** @brief Fill a page-cache frame from the requested file range.
 * @param phys Physical cache frame to fill.
 * @param opaque File, offset, and segment-limit context.
 * @return Zero on success or a negative ReliefOS errno.
 */
static int load_file_cache_page(uint64_t phys, void *opaque)
{
    struct file_page_load_context *context = (struct file_page_load_context *)opaque;
    uint64_t offset;
    uint32_t want;
    uint32_t got = 0;
    if (!context || !context->node) {
        return -RELIEFOS_EINVAL;
    }
    zero_phys_page(phys);
    offset = context->offset;
    if (offset >= context->limit) {
        return 0;
    }
    want = context->limit - offset < PAGE_SIZE
               ? (uint32_t)(context->limit - offset)
               : (uint32_t)PAGE_SIZE;
    /* usercopy can fault here inside an async filesystem syscall. A page
     * fault cannot replay that syscall, and its cache page must stay alive
     * until DMA completes. Match the uncached fault path's synchronous I/O. */
    storage_set_io_async_context(false);
    void *page = mm_phys_to_virt(phys);
    if (!page) return -RELIEFOS_ENOMEM;
    if (storage_read_node(context->node, offset, page,
                          want, &got) < 0 || got != want) {
        return -RELIEFOS_EIO;
    }
    return 0;
}

/** @brief Resolve and install one lazy file-backed VMA page.
 * @param task Owning task and address space.
 * @param vma File mapping metadata and protection policy.
 * @param page Aligned virtual page address to materialize.
 * @return Zero after mapping, otherwise a negative ReliefOS errno.
 */
static int task_map_file_vma_page(struct task *task, const struct task_vma *vma,
                                  uint64_t page)
{
    if (!task || !vma || !(vma->flags & TASK_VMA_FLAG_FILE)) {
        return -RELIEFOS_EINVAL;
    }
    uint64_t phys = 0;
    uint64_t file_offset = vma->file_offset + (page - vma->start);
    bool tmpfs = (vma->file_node.flags & STORAGE_NODE_FLAG_TMPFS) &&
                  (vma->flags & TASK_VMA_FLAG_MMAP);
    uint64_t cache_end = file_offset + PAGE_SIZE;
    if (cache_end > vma->file_node.size) cache_end = vma->file_node.size;
    /* Cache keys describe file pages, not ELF segment limits. A segment's
     * private zero-filled tail must never replace bytes of another LOAD. */
    int cached = !tmpfs && (vma->flags & TASK_VMA_FLAG_SHARED_FILE) &&
                 file_offset < vma->file_node.size && vma->file_limit >= cache_end;
    struct file_page_load_context context;
    if (tmpfs) {
        int ret = storage_tmpfs_get_page(&vma->file_node, file_offset, &phys);
        if (ret < 0) return ret;
    } else if (cached) {
        context.node = &vma->file_node;
        context.offset = vma->file_offset + (page - vma->start);
        context.limit = vma->file_limit;
        int result = page_cache_load(&vma->file_node, context.offset,
                                     load_file_cache_page, &context, &phys);
        /* Cache capacity must not limit the resident executable working set.
         * Private pages follow the normal paging ownership and fork paths. */
        if (result == PAGE_CACHE_FULL && !(vma->flags & TASK_VMA_FLAG_SHARED)) {
            cached = 0;
            phys = mm_alloc_page();
        } else if (result < 0) {
            return -RELIEFOS_EIO;
        }
    } else {
        phys = mm_alloc_page();
    }
    if (!phys) {
        return -RELIEFOS_ENOMEM;
    }
    if (!cached && !tmpfs) {
        zero_phys_page(phys);
    }

    if (!cached && !tmpfs && file_offset < vma->file_limit) {
        uint64_t available = vma->file_limit - file_offset;
        uint32_t want = available < PAGE_SIZE ? (uint32_t)available : (uint32_t)PAGE_SIZE;
        uint32_t got = 0;
        int ret;
        /**
 * @brief A user page fault is not a restartable filesystem syscall. Force this backing-page read through the synchronous AHCI path so an in-flight async storage operation cannot surface as EAGAIN and turn a valid lazy mapping into a process-killing fault.
 */
        storage_set_io_async_context(false);
        void *frame = mm_phys_to_virt(phys);
        if (!frame) {
            if (cached) page_cache_release(phys);
            else mm_free_page(phys);
            return -RELIEFOS_ENOMEM;
        }
        ret = storage_read_node(&vma->file_node, file_offset,
                                frame, want, &got);
        storage_set_io_async_context(false);
        if (ret < 0 || got != want) {
            if (cached) page_cache_release(phys);
            else mm_free_page(phys);
            return ret < 0 ? storage_errno(ret) : -RELIEFOS_EINVAL;
        }
    }

    uint64_t page_flags = (vma->prot & LINUX_PROT_WRITE) ? RELIEFNT_PAGE_WRITABLE : 0;
    if (tmpfs && !(vma->flags & TASK_VMA_FLAG_SHARED) && (vma->prot & LINUX_PROT_WRITE))
        page_flags = RELIEFNT_PAGE_COW;
    if (vma->flags & TASK_VMA_FLAG_SHARED) page_flags |= RELIEFNT_PAGE_SHARED;
    if (!(vma->prot & LINUX_PROT_EXEC)) {
        page_flags |= RELIEFNT_PAGE_NOEXEC;
    }
    if (!address_space_map_user_page(sched_task_as(task), page, phys, page_flags)) {
        if (cached) page_cache_release(phys);
        else mm_free_page(phys);
        return -RELIEFOS_ENOMEM;
    }
    return 0;
}

/**
 * @brief RLIMIT_STACK/RLIMIT_AS admission for a new lowest stack page.
 *
 * Mirrors Linux mm/mmap.c:acct_stack_growth(): @size is the whole growable
 * stack VMA span (top - candidate start) and the stack test is
 * `size > rlimit(RLIMIT_STACK)`; RLIM_INFINITY disables only that test.  The
 * address-space limit stays a cumulative virtual-memory check.
 */
static bool task_stack_fault_candidate(const struct task *task, uint64_t page,
                                       uint64_t error)
{
    /* A missing user stack PTE can appear as a present protection fault when
     * the inherited supervisor-only 2 MiB identity mapping covers the next
     * page-directory slot.  Accept user data reads/writes in the stack window;
     * existing user PTEs were already handled above the stack path. */
    return task && task->stack_top && task->stack_low &&
           (error & 0x4ULL) && !(error & 0x18ULL) &&
           page < task->stack_top &&
           page >= task->stack_top - (uint64_t)RELIEFNT_USER_STACK_MAX_PAGES * PAGE_SIZE &&
           page >= RELIEFNT_USER_BASE + PAGE_SIZE;
}

static bool task_stack_growth_allowed(const struct task *task, uint64_t page)
{
    if (!task || !task->stack_top || page >= task->stack_top) {
        return false;
    }
    uint64_t stack_size = task->stack_top - page;
    const struct task_address_space_state *mm = sched_task_mm(task);
    uint64_t old_low = mm->initial_stack_low ? mm->initial_stack_low : task->stack_low;
    if (page >= old_low) return true;
    const struct task_rlimit_state *limits = sched_task_limits(task);
    if (limits->stack.rlim_cur != LINUX_RLIM_INFINITY &&
        stack_size > limits->stack.rlim_cur) {
        return false;
    }
    if (page < old_low && limits->as.rlim_cur != LINUX_RLIM_INFINITY) {
        uint64_t limit = limits->as.rlim_cur & ~(PAGE_SIZE - 1ULL);
        uint64_t used = task_vma_total_bytes(task);
        if (used > limit || old_low - page > limit - used) {
            return false;
        }
    }
    return true;
}

/** @brief Grow a prepared exec stack using the same accounting as stack faults.
 * @param task Prepared task owning the not-yet-committed user mappings.
 * @param low Lowest required page, aligned to 4 KiB.
 * @return Zero, -E2BIG for RLIMIT_STACK/window exhaustion, or -ENOMEM.
 */
int syscall_mm_prepare_exec_stack(struct task *task, uint64_t low)
{
    if (!task || (low & (PAGE_SIZE - 1)) || low >= task->stack_top ||
        low < task->stack_top - (uint64_t)RELIEFNT_USER_STACK_MAX_PAGES * PAGE_SIZE)
        return -RELIEFOS_E2BIG;
    if (task->stack_top - low > sched_task_limits(task)->stack.rlim_cur)
        return -RELIEFOS_E2BIG;
    uint64_t as_limit = sched_task_limits(task)->as.rlim_cur;
    if (as_limit != LINUX_RLIM_INFINITY &&
        task_vma_total_bytes(task) > (as_limit & ~(PAGE_SIZE - 1ULL)))
        return -RELIEFOS_ENOMEM;
    if (low >= task->stack_low) return 0;
    if (!task_stack_growth_allowed(task, low)) return -RELIEFOS_ENOMEM;
    for (uint64_t page = low; page < task->stack_low; page += PAGE_SIZE)
        if (!address_space_map_user_stack_page(sched_task_as(task), page))
            return -RELIEFOS_ENOMEM;
    task->stack_low = low;
    sched_task_mm(task)->initial_stack_low = low;
    return 0;
}


/** @brief Distinguish a missing mapping from denied access to an existing VMA. */
int syscall_page_fault_signal_code(struct task *task, uint64_t address)
{
    if (task->page_fault_signal == LINUX_SIGBUS) return LINUX_BUS_ADRERR;
    struct task_address_space_state *mm = sched_task_mm(task);
    uint64_t page = align_down_page(address);
    if (task_vma_containing(task, page, page + PAGE_SIZE) ||
        (mm->initial_stack_low && page >= mm->initial_stack_low && page < mm->initial_stack_top) ||
        address_space_user_page_phys(sched_task_as(task), page)) return LINUX_SEGV_ACCERR;
    return LINUX_SEGV_MAPERR;
}

int syscall_handle_private_anon_fault(struct task *task, uint64_t fault_addr, uint64_t error)
{
    /* The execution read gate keeps fork/exec/munmap and remote memory
     * operations out. An unshared MM has exactly one running owner, so its
     * VMA/page tables need no additional lock here. CLONE_VM (including
     * vfork) and all file/shared mappings retain the exclusive slow path.
     * The physical allocator has its own lock. No usercopy or VFS calls are
     * permitted here: they could recursively acquire the exclusive gate. */
    if (!task || task->kind != TASK_KIND_USER || task->shared_mm || (error & 0x8ULL))
        return 0;
    uint64_t page = align_down_page(fault_addr);
    if (page < RELIEFNT_USER_BASE || page >= RELIEFNT_USER_TOP ||
        address_space_user_page_phys(sched_task_as(task), page)) return 0;
    struct task_vma *vma = task_vma_containing(task, page, page + PAGE_SIZE);
    if (!vma || !(vma->flags & TASK_VMA_FLAG_ANON) ||
        (vma->flags & (TASK_VMA_FLAG_SHARED | TASK_VMA_FLAG_FILE)) ||
        vma->prot == LINUX_PROT_NONE ||
        ((error & 0x2ULL) && !(vma->prot & LINUX_PROT_WRITE)) ||
        ((error & 0x10ULL) && !(vma->prot & LINUX_PROT_EXEC))) return 0;
    return task_map_anonymous_page(task, vma, page) == 0;
}

int syscall_handle_task_page_fault(struct task *task, uint64_t fault_addr, uint64_t error)
{
    if (!task || task->kind != TASK_KIND_USER) {
        return 0;
    }
    task->page_fault_signal = LINUX_SIGSEGV;
    uint64_t page = align_down_page(fault_addr);
    if (page < RELIEFNT_USER_BASE || page >= RELIEFNT_USER_TOP) {
        return 0;
    }
    /**
 * @brief A write through either Ring-3 code or a kernel syscall helper can touch the calling process's present, read-only COW PTE. Resolve only that precise protection fault; reserved-bit and instruction-fetch faults remain fatal and ordinary kernel faults never reach this path.
 */
    if ((error & 0x3ULL) == 0x3ULL && !(error & 0x18ULL) &&
        address_space_handle_cow_fault(sched_task_as(task), page)) {
        return 1;
    }
    /* A user address can still report the PRESENT bit when the bootstrap
     * address space contributes a supervisor-only 2 MiB identity PDE.  That
     * is not a valid user mapping, but it is a recoverable first fault when a
     * recorded anonymous or file-backed VMA owns the address: the VMA path
     * below replaces the inherited PDE with a user PTE.  Reserved-bit faults
     * remain fatal before any VMA handling. */
    if (error & 0x8ULL) {
        return 0;
    }
    if (address_space_retry_mapped_user_page(sched_task_as(task), page, error)) {
        return 1;
    }
    if (address_space_user_page_phys(sched_task_as(task), page)) {
        return 0;
    }

    {
        struct task_vma *anon = task_vma_containing(task, page, page + PAGE_SIZE);
        if (anon && (anon->flags & TASK_VMA_FLAG_ANON)) {
            if (anon->prot == LINUX_PROT_NONE ||
                ((error & 0x2ULL) && !(anon->prot & LINUX_PROT_WRITE)) ||
                ((error & 0x10ULL) && !(anon->prot & LINUX_PROT_EXEC))) {
                return 0;
            }
            return task_map_anonymous_page(task, anon, page) == 0;
        }
    }

    /**
 * @brief Grow the anonymous user stack on demand. The initial image maps a
 * small working set; accesses below it consume pages while RLIMIT_STACK
 * permits, bounded by the native user stack window, and leave one unmapped
 * guard page below the stack.
 *
 * A single user instruction (notably a forward-running memset) can cross
 * upward from a newly grown page into the originally unmapped gap between
 * task->stack_low and the eager stack mapping. Therefore every missing page
 * inside the legal stack range is mapped, and stack_low only moves when the
 * fault is below the current lowest mapped page.
 */
    if (task_stack_fault_candidate(task, page, error) &&
        (!sched_task_mm(task)->initial_stack_top ||
         task->stack_top == sched_task_mm(task)->initial_stack_top)) {
        uint64_t guard = task->stack_top -
                         (uint64_t)RELIEFNT_USER_STACK_MAX_PAGES * PAGE_SIZE - PAGE_SIZE;
        struct task_address_space_state *mm = sched_task_mm(task);
        if (!task_stack_growth_allowed(task, page)) return 0;
        if (page > guard && address_space_map_user_stack_page(sched_task_as(task), page)) {
            if (page < task->stack_low) {
                task->stack_low = page;
            }
            if (!mm->initial_stack_low || page < mm->initial_stack_low) mm->initial_stack_low = page;
            return 1;
        }
        return 0;
    }
    struct task_vma *vma = task_vma_containing(task, page, page + PAGE_SIZE);
    if (!vma || !(vma->flags & TASK_VMA_FLAG_LAZY) || !(vma->flags & TASK_VMA_FLAG_FILE)) {
        return 0;
    }
    if (vma->prot == LINUX_PROT_NONE) return 0;
    if ((error & 0x2ULL) && !(vma->prot & LINUX_PROT_WRITE)) {
        return 0;
    }
    if ((error & 0x10ULL) && !(vma->prot & LINUX_PROT_EXEC)) {
        return 0;
    }
    {
        uint64_t loader_flags;
        userland_loader_lock(&loader_flags);
        int ret = task_map_file_vma_page(task, vma, page);
        userland_loader_unlock(loader_flags);
        if (ret < 0) {
            if ((vma->file_node.flags & STORAGE_NODE_FLAG_TMPFS) &&
                (vma->flags & TASK_VMA_FLAG_MMAP) &&
                (ret == -RELIEFOS_EINVAL || ret == -RELIEFOS_ENOSPC))
                task->page_fault_signal = LINUX_SIGBUS;
            console_error("[reliefnt] lazy file map failed pid=%u page=0x%llx "
                           "fault=0x%llx error=0x%llx vma=0x%llx-0x%llx "
                           "flags=0x%x file_off=0x%llx file_limit=0x%llx ret=%d\n",
                           task->pid, (unsigned long long)page,
                           (unsigned long long)fault_addr,
                           (unsigned long long)error,
                           (unsigned long long)vma->start,
                           (unsigned long long)vma->end, vma->flags,
                           (unsigned long long)vma->file_offset,
                           (unsigned long long)vma->file_limit, ret);
        }
        return ret == 0;
    }
}

/** @brief Resolve a user fault in the current task's address space. */
int syscall_handle_user_page_fault(uint64_t fault_addr, uint64_t error)
{
    return syscall_handle_task_page_fault(sched_current_task(), fault_addr, error);
}
