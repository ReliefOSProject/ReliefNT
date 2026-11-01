#include "memory_internal.h"
#include <reliefnt/exec_file.h>
#include <reliefnt/memory_stats.h>

extern void memory_event_add(enum memory_event, uint64_t) __attribute__((weak));
extern bool sched_fault_event_major(struct task *) __attribute__((weak));

/** @brief Read a lazy file mapping through its retained executable/OFD source.
 * @param vma Stable file VMA whose source owns the provider callback lifetime.
 * @param offset Nonnegative file byte offset.
 * @param buffer Writable kernel destination for capacity bytes.
 * @param capacity Requested bytes, bounded to one allocator page by callers.
 * @param read Required output for completed bytes.
 * @return Zero or backing errno; absent source fails EBADF. Caller owns the
 * execution/loader transaction and forces synchronous fault I/O. No displayed
 * name, unpinned filesystem callback or graph/module spinlock participates.
 */
static int file_vma_read(const struct task_vma *vma,uint64_t offset,void *buffer,
                            uint32_t capacity,uint32_t *read)
{
    if(vma->exec_source)return vfs_exec_source_read(vma->exec_source,offset,buffer,capacity,read);
    const struct task_file *file=task_file_description(vma->mapping_file);
    if(file && vfs_path_node(&file->path_ref))return vfs_path_read(&file->path_ref,offset,buffer,capacity,read);
    *read=0;return -RELIEFOS_EBADF;
}

/**
 * @brief Clear one allocator page through the kernel direct map.
 * @param phys Physical page address returned by the allocator.
 */
static void zero_phys_page(uint64_t phys);

/** @brief Fault one private or shared anonymous page with the correct owner.
 * @param task Execution-owned user task. @param vma Covering anonymous mapping.
 * @param page Aligned user address. @return Zero or a negative errno.
 */
static int task_map_anonymous_page(struct task *task, struct task_vma *vma,
                                   uint64_t page);

static int load_file_cache_page(uint64_t phys, void *opaque);

static int task_map_file_vma_page(struct task *task, const struct task_vma *vma,
                                  uint64_t page);
/** @brief Populate a file page and optionally observe completed provider I/O.
 * @param task Execution-owned user task. @param vma File VMA under loader lock.
 * @param page Aligned user address. @param did_read Optional boolean output.
 * @param retry_fault True only when the outer ring-3 dispatcher can retry
 * after releasing the fault lock; false preserves ordinary cache reclaim.
 * @return Zero or negative errno; no ownership escapes on failure.
 */
static int task_map_file_vma_page_observed(struct task *task, const struct task_vma *vma,
                                           uint64_t page, bool *did_read, bool retry_fault);

/**
 * @brief Check a fault against the shared initial-stack address window.
 * @param task Borrowed task with an execution-owned shared MM.
 * @param page Aligned candidate address. @param error User page-fault flags.
 * @return True when the page lies inside the implicit initial stack window.
 */
static bool task_stack_fault_candidate(const struct task *task, uint64_t page,
                                       uint64_t error);

/** @brief Check shared initial-stack limits before growth.
 * @param task Borrowed execution-owned task. @param page Aligned candidate page.
 * @return True for an existing stack page or admitted new low bound.
 */
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
 * @brief Populate an admitted anonymous interval with its requested permissions.
 * @param task User task owning the mapping and its page tables.
 * @param start Page-aligned inclusive first user virtual byte.
 * @param end Page-aligned exclusive interval end.
 * @param page_flags VMA-validated PTE flags, including NX unless EXEC was requested.
 * @return Zero on success or negative ENOMEM after releasing a partial mapping.
 */
int task_map_anonymous_pages(struct task *task, uint64_t start, uint64_t end,
                                    uint64_t page_flags)
{
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

/** @brief Fault one private or shared anonymous page with the correct owner.
 * @param task Execution-owned user task. @param vma Covering anonymous mapping.
 * @param page Aligned user address. @return Zero or a negative errno.
 */
static int task_map_anonymous_page(struct task *task, struct task_vma *vma,
                                   uint64_t page)
{
    uint64_t phys;
    uint64_t flags = 0;
    if (!task || !vma || !(vma->flags & TASK_VMA_FLAG_ANON) ||
        vma->prot == LINUX_PROT_NONE) {
        return -RELIEFOS_EFAULT;
    }
    if (vma->prot & LINUX_PROT_WRITE) flags |= RELIEFNT_PAGE_WRITABLE;
    if (!(vma->prot & LINUX_PROT_EXEC)) flags |= RELIEFNT_PAGE_NOEXEC;
    if (vma->flags & TASK_VMA_FLAG_SHARED)
        return mm_shared_fault(sched_task_as(task), page, flags);
    phys = mm_alloc_page();
    if (!phys) return -RELIEFOS_ENOMEM;
    zero_phys_page(phys);
    if (!address_space_map_user_page(sched_task_as(task), page, phys, flags)) {
        mm_free_page(phys);
        return -RELIEFOS_ENOMEM;
    }
    return 0;
}

/** @brief Fill a page-cache frame from the requested file range.
 * @param phys Physical cache frame to fill.
 * @param opaque File, offset, and segment-limit context.
 * @return Zero on success, -EINVAL for an invalid context, -ENOMEM for a
 * missing direct map or provider allocation failure, or -EIO for other
 * provider errors and short reads. The cache owns the frame.
 */
static int load_file_cache_page(uint64_t phys, void *opaque)
{
    struct file_page_load_context *context = (struct file_page_load_context *)opaque;
    uint64_t offset;
    uint32_t want;
    uint32_t got = 0;
    if (!context || !context->vma) {
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
    int ret = file_vma_read(context->vma, offset, page, want, &got);
    if (ret < 0) return ret == -RELIEFOS_ENOMEM ? ret : -RELIEFOS_EIO;
    if (got != want) return -RELIEFOS_EIO;
    context->did_read = true;
    return 0;
}

/** @brief Resolve and install one lazy file-backed VMA page.
 * @param task Owning task and address space.
 * @param vma File mapping metadata and protection policy.
 * @param page Aligned virtual page address to materialize.
 * @param did_read Optional output set after completed provider bytes, including
 * when later mapping fails; loader/execution ownership pins the source.
 * @param retry_fault True only when the outer hardware-fault dispatcher can
 * reclaim and retry after this call returns, with the fault lock released.
 * @return Zero after mapping, -ENOMEM for an allocation failure, otherwise
 * a negative ReliefOS errno. Failure retains no newly acquired frame ownership.
 */
static int task_map_file_vma_page_observed(struct task *task, const struct task_vma *vma,
                                           uint64_t page, bool *did_read, bool retry_fault)
{
    if (did_read) *did_read = false;
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
    struct file_page_load_context context = {0};
    if (tmpfs) {
        int ret = storage_tmpfs_get_page(&vma->file_node, file_offset, &phys);
        if (ret < 0) return ret;
    } else if (cached) {
        context.vma = vma;
        context.offset = vma->file_offset + (page - vma->start);
        context.limit = vma->file_limit;
        int result = retry_fault ?
            page_cache_load_fault(&vma->file_node, context.offset, load_file_cache_page, &context, &phys) :
            page_cache_load(&vma->file_node, context.offset, load_file_cache_page, &context, &phys);
        if (did_read) *did_read = context.did_read;
        /* Cache capacity must not limit the resident executable working set.
         * Private pages follow the normal paging ownership and fork paths. */
        if (result == PAGE_CACHE_FULL && !(vma->flags & TASK_VMA_FLAG_SHARED)) {
            cached = 0;
            phys = mm_alloc_page();
        } else if (result < 0) {
            return result == -RELIEFOS_ENOMEM ? result : -RELIEFOS_EIO;
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
        ret = file_vma_read(vma, file_offset,
                                frame, want, &got);
        storage_set_io_async_context(false);
        if (ret < 0 || got != want) {
            if (cached) page_cache_release(phys);
            else mm_free_page(phys);
            return ret < 0 ? storage_errno(ret) : -RELIEFOS_EINVAL;
        }
        if (did_read) *did_read = true;
    }

    uint64_t page_flags = (vma->prot & LINUX_PROT_WRITE) ? RELIEFNT_PAGE_WRITABLE : 0;
    if (tmpfs && !(vma->flags & TASK_VMA_FLAG_SHARED) && (vma->prot & LINUX_PROT_WRITE))
        page_flags = RELIEFNT_PAGE_COW;
    if (cached && (vma->prot & LINUX_PROT_WRITE)) {
        if (vma->flags & TASK_VMA_FLAG_SHARED)
        {
            page_cache_mark_shared_writable(phys);
            page_flags = RELIEFNT_PAGE_WRITE_NOTIFY;
        }
        else
            page_flags = RELIEFNT_PAGE_COW;
    }
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

/** @brief Populate a file page without collecting user-fault accounting.
 * @param task Execution-owned task. @param vma File VMA. @param page Address.
 * @return Zero or negative errno; lock-prefault callers retain prior semantics.
 */
static int task_map_file_vma_page(struct task *task, const struct task_vma *vma,
                                  uint64_t page)
{ return task_map_file_vma_page_observed(task,vma,page,NULL,false); }

/** @brief Populate one lockable VMA page, preserving private COW semantics.
 * @param task Execution-owned user task. @param vma Live covering VMA.
 * @param page Aligned user address; no VMA-store search is performed.
 * @return Zero on success, negative EFAULT for inaccessible or unsupported
 *         mappings, or an allocator/provider errno. Shared writable file
 *         mappings use the normal cache-grant marking before publication.
 */
int task_populate_lock_vma_page(struct task *task, struct task_vma *vma, uint64_t page)
{
    if (!task || !vma || !vma->used || page < vma->start ||
        page >= vma->end || vma->end - page < PAGE_SIZE || vma->prot == LINUX_PROT_NONE)
        return -RELIEFOS_EFAULT;
    uint64_t phys = address_space_user_page_phys(sched_task_as(task), page);
    if (phys) {
        if ((vma->prot & LINUX_PROT_WRITE) &&
            !(vma->flags & TASK_VMA_FLAG_SHARED) &&
            (address_space_user_page_flags(sched_task_as(task), page) & RELIEFNT_PAGE_COW) &&
            !address_space_handle_cow_fault(sched_task_as(task), page))
            return -RELIEFOS_ENOMEM;
        return 0;
    }
    if (vma->flags & TASK_VMA_FLAG_SYSV_SHM) return sysv_shm_fault_page(task, vma, page);
    if (vma->flags & TASK_VMA_FLAG_ANON) return task_map_anonymous_page(task, vma, page);
    if ((vma->flags & (TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY)) ==
        (TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY)) {
        uint64_t flags;
        userland_loader_lock(&flags);
        int ret = task_map_file_vma_page(task, vma, page);
        userland_loader_unlock(flags);
        if (!ret && (vma->prot & LINUX_PROT_WRITE) &&
            !(vma->flags & TASK_VMA_FLAG_SHARED) &&
            (address_space_user_page_flags(sched_task_as(task), page) & RELIEFNT_PAGE_COW) &&
            !address_space_handle_cow_fault(sched_task_as(task), page))
            return -RELIEFOS_ENOMEM;
        return ret;
    }
    return -RELIEFOS_EFAULT;
}

/** @brief Populate a page already verified to have no covering VMA.
 * @param task Execution-owned user task. @param page Aligned user address.
 * @return Zero or negative errno. Only the implicit initial stack is accepted;
 * a resident private COW page is made exclusive.
 */
int task_populate_lock_stack_page(struct task *task, uint64_t page)
{
    if (!task) return -RELIEFOS_EFAULT;
    const struct task_address_space_state *mm = sched_task_mm(task);
    uint64_t low = mm->initial_stack_low ? mm->initial_stack_low : task->stack_low;
    uint64_t top = mm->initial_stack_top ? mm->initial_stack_top : task->stack_top;
    if (!low || page < low || page >= top) return -RELIEFOS_EFAULT;
    uint64_t phys = address_space_user_page_phys(sched_task_as(task), page);
    if (phys) {
        if ((address_space_user_page_flags(sched_task_as(task), page) & RELIEFNT_PAGE_COW) &&
            !address_space_handle_cow_fault(sched_task_as(task), page))
            return -RELIEFOS_ENOMEM;
        return 0;
    }
    return address_space_map_user_stack_page(sched_task_as(task), page) ? 0 : -RELIEFOS_ENOMEM;
}

/** @brief Populate a lockable page, including an implicit initial-stack page.
 * @param task Execution-owned user task. @param page Aligned user address.
 * @return Zero or a negative fault/provider errno; private COW is made exclusive.
 * This convenience entry searches the VMA store; bounded walkers use the
 * prelocated-VMA and checked implicit-stack entries instead.
 */
int task_populate_lock_page(struct task *task, uint64_t page)
{
    struct task_vma *vma = task_vma_containing(task, page, page + PAGE_SIZE);
    if (vma) return task_populate_lock_vma_page(task, vma, page);
    return task_populate_lock_stack_page(task, page);
}

/**
 * @brief Check a fault against the shared initial-stack address window.
 * @param task Borrowed task with an execution-owned shared MM.
 * @param page Aligned candidate address. @param error User page-fault flags.
 * @return True when the page lies inside the implicit initial stack window.
 */
static bool task_stack_fault_candidate(const struct task *task, uint64_t page,
                                       uint64_t error)
{
    /* A missing user stack PTE can appear as a present protection fault when
     * the inherited supervisor-only 2 MiB identity mapping covers the next
     * page-directory slot.  Accept user data reads/writes in the stack window;
     * existing user PTEs were already handled above the stack path. */
    if (!task) return false;
    const struct task_address_space_state *mm = sched_task_mm(task);
    uint64_t top = mm->initial_stack_top ? mm->initial_stack_top : task->stack_top;
    uint64_t low = mm->initial_stack_low ? mm->initial_stack_low : task->stack_low;
    return top && low &&
           (error & 0x4ULL) && !(error & 0x18ULL) &&
           page < top &&
           page >= top - (uint64_t)RELIEFNT_USER_STACK_MAX_PAGES * PAGE_SIZE &&
           page >= RELIEFNT_USER_BASE + PAGE_SIZE;
}

/** @brief Check shared initial-stack limits before growth.
 * @param task Borrowed execution-owned task. @param page Aligned candidate page.
 * @return True for an existing stack page or admitted new low bound.
 */
static bool task_stack_growth_allowed(const struct task *task, uint64_t page)
{
    if (!task) return false;
    const struct task_address_space_state *mm = sched_task_mm(task);
    uint64_t top = mm->initial_stack_top ? mm->initial_stack_top : task->stack_top;
    if (!top || page >= top) {
        return false;
    }
    uint64_t stack_size = top - page;
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

/** @brief Check whether an implicit user stack may fault a given page.
 * @param task Live MM owner. @param page Aligned candidate. @param end Receives
 * the exclusive upper extent. @return True only for permitted stack growth.
 * The metadata query allocates no page and issues no I/O.
 */
bool mm_user_stack_readable_extent(struct task *task,uint64_t page,uint64_t *end)
{
    if (!end || !task_stack_fault_candidate(task,page,4) ||
        !task_stack_growth_allowed(task,page)) return false;
    const struct task_address_space_state *mm = sched_task_mm(task);
    *end = mm->initial_stack_top ? mm->initial_stack_top : task->stack_top;
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

/** @brief Validate a private anonymous demand-zero fault without allocating.
 * @param task Borrowed user task under execution read or exclusive ownership.
 * @param fault_addr Faulting virtual byte address.
 * @param error x86 page-fault error bits.
 * @return True only when the private anonymous mapping admits this fault.
 * The execution gate pins the unshared MM; no allocation, provider I/O,
 * usercopy, scheduling or ownership transfer occurs.
 */
bool syscall_private_anon_fault_eligible(struct task *task, uint64_t fault_addr, uint64_t error)
{
    if (!task || task->kind != TASK_KIND_USER || task->shared_mm || (error & 0x8ULL))
        return false;
    uint64_t page = align_down_page(fault_addr);
    if (page < RELIEFNT_USER_BASE || page >= RELIEFNT_USER_TOP ||
        paging_user_entry_kind(sched_task_as(task), page) != PAGING_USER_ENTRY_EMPTY)
        return false;
    struct task_vma *vma = task_vma_containing(task, page, page + PAGE_SIZE);
    return vma && (vma->flags & TASK_VMA_FLAG_ANON) &&
           !(vma->flags & (TASK_VMA_FLAG_SHARED | TASK_VMA_FLAG_FILE)) &&
           vma->prot != LINUX_PROT_NONE &&
           (!(error & 0x2ULL) || (vma->prot & LINUX_PROT_WRITE)) &&
           (!(error & 0x10ULL) || (vma->prot & LINUX_PROT_EXEC));
}

/** @brief Resolve an eligible private anonymous fault using ordinary frames.
 * @param task Execution-owned current task. @param fault_addr User byte.
 * @param error x86 fault flags. @return One on success, minus one on ENOMEM,
 * or zero when ineligible. No backing I/O or scheduling occurs.
 */
static int task_private_anon_fault_result(struct task *task, uint64_t fault_addr, uint64_t error)
{
    if(!syscall_private_anon_fault_eligible(task,fault_addr,error))return 0;
    uint64_t page=align_down_page(fault_addr);
    struct task_vma *vma=task_vma_containing(task,page,page+PAGE_SIZE);
    int rc=task_map_anonymous_page(task,vma,page);
    return rc==0 ? 1 : rc==-RELIEFOS_ENOMEM ? -1 : 0;
}

/** @brief Preserve the historical boolean private-anonymous fault interface.
 * @param task Borrowed task. @param fault_addr Address. @param error x86 flags.
 * @return One only for a resolved page; no I/O or scheduling occurs.
 */
int syscall_handle_private_anon_fault(struct task *task, uint64_t fault_addr, uint64_t error)
{ return task_private_anon_fault_result(task, fault_addr, error) == 1; }

/** @brief Resolve a present private COW leaf belonging to a writable mapping.
 * @param task Borrowed unshared user task protected by execution ownership.
 * @param vma Covering VMA, or NULL for the initial stack interval.
 * @param page Page-aligned fault address within the user range.
 * @return One if the existing COW copy succeeds; zero otherwise. Uses the
 * normal physical/cache reference operations without usercopy or callbacks.
 */
static int task_private_cow_fault(struct task *task, const struct task_vma *vma,
                                  uint64_t page)
{
    uint64_t flags = address_space_user_page_flags(sched_task_as(task), page);
    uint64_t needed = RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_USER | RELIEFNT_PAGE_COW;
    if ((flags & needed) != needed ||
        (flags & (RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_SHARED | RELIEFNT_PAGE_PROTNONE)))
        return 0;
    if (vma) {
        if (!(vma->prot & LINUX_PROT_WRITE) ||
            (vma->flags & (TASK_VMA_FLAG_SHARED | TASK_VMA_FLAG_DEVICE |
                           TASK_VMA_FLAG_SYSV_SHM | TASK_VMA_FLAG_VDSO | TASK_VMA_FLAG_VVAR)))
            return 0;
    } else {
        const struct task_address_space_state *mm = sched_task_mm(task);
        uint64_t low = mm->initial_stack_low ? mm->initial_stack_low : task->stack_low;
        uint64_t top = mm->initial_stack_top ? mm->initial_stack_top : task->stack_top;
        if (!low || page < low || page >= top) return 0;
    }
    return address_space_handle_cow_fault(sched_task_as(task), page) ? 1 : -1;
}

/** @brief Install an already-resident immutable executable cache page.
 * @param task Borrowed unshared user task protected by execution ownership.
 * @param vma Covering file VMA and retained executable source, nullable.
 * @param page Aligned user page in the VMA.
 * @param error x86 page-fault error bits.
 * @return One after mapping, zero on ineligibility, miss or mapping failure.
 * The cache lookup retains its frame under the cache lock; failed mapping
 * releases it. No provider/source operation or usercopy runs under the gate.
 */
static int task_private_exec_cache_fault(struct task *task, const struct task_vma *vma,
                                        uint64_t page, uint64_t error)
{
    const uint32_t required = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY |
                              TASK_VMA_FLAG_SHARED_FILE;
    const uint32_t excluded = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_SHARED |
                              TASK_VMA_FLAG_DEVICE | TASK_VMA_FLAG_SYSV_SHM |
                              TASK_VMA_FLAG_VDSO | TASK_VMA_FLAG_VVAR;
    if (!vma || !vma->exec_source || (vma->flags & required) != required ||
        (vma->flags & excluded) || vma->file_node.type != RELIEFOS_FS_TYPE_FILE ||
        (vma->file_node.flags & STORAGE_NODE_FLAG_TMPFS) ||
        vma->prot == LINUX_PROT_NONE || (vma->prot & LINUX_PROT_WRITE) ||
        (error & 0x2ULL) || ((error & 0x10ULL) && !(vma->prot & LINUX_PROT_EXEC)))
        return 0;
    uint64_t delta = page - vma->start;
    if (vma->file_offset > UINT64_MAX - delta) return 0;
    uint64_t offset = vma->file_offset + delta;
    if ((offset & (PAGE_SIZE - 1ULL)) || offset >= vma->file_node.size ||
        vma->file_limit < offset) return 0;
    uint64_t available = vma->file_node.size - offset;
    uint64_t bytes = available < PAGE_SIZE ? available : PAGE_SIZE;
    if (vma->file_limit - offset < bytes) return 0;
    uint64_t phys = 0;
    if (page_cache_lookup(&vma->file_node, offset, &phys) != 0) return 0;
    uint64_t flags = (vma->prot & LINUX_PROT_EXEC) ? 0 : RELIEFNT_PAGE_NOEXEC;
    if (!address_space_map_user_page(sched_task_as(task), page, phys, flags)) {
        page_cache_release(phys);
        return 0;
    }
    return 1;
}

/** @brief Resolve a private fault without I/O under reader or writer ownership.
 * @param task Borrowed current user task; unshared MM remains live under gate.
 * @param fault_addr Faulting virtual byte address.
 * @param error x86 user page-fault error bits.
 * @return ANON for demand-zero, RESIDENT for present COW or cached executable
 * pages, or UNHANDLED to let the exclusive slow path handle it.
 * Task/fault context, IRQs masked. No provider callbacks, source ref changes,
 * usercopy or scheduling; allocator and page-cache locks nest below the gate.
 */
enum syscall_private_fault_result syscall_handle_private_page_fault(
    struct task *task, uint64_t fault_addr, uint64_t error)
{
    if (!task || task->kind != TASK_KIND_USER || task->shared_mm ||
        !(error & 0x4ULL) || (error & 0x8ULL) || (error & ~0x1fULL))
        return SYSCALL_PRIVATE_FAULT_UNHANDLED;
    uint64_t page = align_down_page(fault_addr);
    if (page < RELIEFNT_USER_BASE || page >= RELIEFNT_USER_TOP)
        return SYSCALL_PRIVATE_FAULT_UNHANDLED;
    struct task_vma *vma = task_vma_containing(task, page, page + PAGE_SIZE);
    if (address_space_user_page_phys(sched_task_as(task), page)) {
        if ((error & 0x3ULL) == 0x3ULL && !(error & 0x10ULL)) {
            int rc = task_private_cow_fault(task, vma, page);
            return rc > 0 ? SYSCALL_PRIVATE_FAULT_RESIDENT :
                   rc < 0 ? SYSCALL_PRIVATE_FAULT_OOM : SYSCALL_PRIVATE_FAULT_UNHANDLED;
        }
        return SYSCALL_PRIVATE_FAULT_UNHANDLED;
    }
    if (vma && (vma->flags & TASK_VMA_FLAG_ANON)) {
        int rc = task_private_anon_fault_result(task, fault_addr, error);
        return rc > 0 ? SYSCALL_PRIVATE_FAULT_ANON :
               rc < 0 ? SYSCALL_PRIVATE_FAULT_OOM : SYSCALL_PRIVATE_FAULT_UNHANDLED;
    }
    return task_private_exec_cache_fault(task, vma, page, error) ?
        SYSCALL_PRIVATE_FAULT_RESIDENT : SYSCALL_PRIVATE_FAULT_UNHANDLED;
}

/** @brief Resolve an admitted user fault while retaining allocation failure.
 * @param task User task pinned by execution ownership, nullable.
 * @param fault_addr Faulting user byte address. @param error x86 fault flags.
 * @param defer_oom_diagnostic True when the caller reports a terminal fault
 * after pressure retries; other backing failures remain immediately logged.
 * @param record_major True only for an admitted user hardware fault.
 * @return One on resolution, -ENOMEM on a valid allocation failure, or zero
 * for an invalid/denied/provider failure. The loader lock is released before
 * return. The caller holds execution ownership and owns any resulting page
 * mapping; hardware-fault callers also hold the fault lock.
 */
static int task_page_fault_detailed(struct task *task, uint64_t fault_addr, uint64_t error,
                                    bool defer_oom_diagnostic, bool record_major)
{
    if (!task || task->kind != TASK_KIND_USER) {
        return 0;
    }
    task->page_fault_signal = LINUX_SIGSEGV;
    uint64_t page = align_down_page(fault_addr);
    if (page < RELIEFNT_USER_BASE || page >= RELIEFNT_USER_TOP) {
        return 0;
    }
    if ((error & ~0x4ULL) == 0x3ULL) {
        struct task_vma *vma=task_vma_containing(task,page,page+PAGE_SIZE);
        const uint32_t required=TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_SHARED|TASK_VMA_FLAG_SHARED_FILE;
        if (vma && (vma->flags&required)==required &&
            !(vma->flags&(TASK_VMA_FLAG_DEVICE|TASK_VMA_FLAG_SYSV_SHM)) &&
            !(vma->file_node.flags&STORAGE_NODE_FLAG_TMPFS) &&
            (vma->prot&LINUX_PROT_WRITE) &&
            address_space_shared_write_fault(sched_task_as(task),page)) return 1;
    }
    /**
 * @brief A write through either Ring-3 code or a kernel syscall helper can touch the calling process's present, read-only COW PTE. Resolve only that precise protection fault; reserved-bit and instruction-fetch faults remain fatal and ordinary kernel faults never reach this path.
 */
    if ((error & 0x3ULL) == 0x3ULL && !(error & 0x18ULL) &&
        address_space_handle_cow_fault(sched_task_as(task), page)) {
        return 1;
    }
    if ((error & 0x3ULL) == 0x3ULL && !(error & 0x18ULL)) {
        struct task_vma *vma = task_vma_containing(task, page, page + PAGE_SIZE);
        int cow = task_private_cow_fault(task, vma, page);
        if (cow > 0) return 1;
        if (cow < 0) return -RELIEFOS_ENOMEM;
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
    if (paging_user_entry_kind(sched_task_as(task), page) != PAGING_USER_ENTRY_EMPTY) {
        return 0;
    }

    {
        struct task_vma *shared = task_vma_containing(task, page, page + PAGE_SIZE);
        if (shared && (shared->flags & TASK_VMA_FLAG_SYSV_SHM)) {
            if (shared->prot == LINUX_PROT_NONE ||
                ((error & 0x2ULL) && !(shared->prot & LINUX_PROT_WRITE)) ||
                ((error & 0x10ULL) && !(shared->prot & LINUX_PROT_EXEC))) return 0;
            int rc = sysv_shm_fault_page(task, shared, page);
            return rc == 0 ? 1 : rc == -RELIEFOS_ENOMEM ? -RELIEFOS_ENOMEM : 0;
        }
    }

    {
        struct task_vma *anon = task_vma_containing(task, page, page + PAGE_SIZE);
        if (anon && (anon->flags & TASK_VMA_FLAG_ANON)) {
            if (anon->prot == LINUX_PROT_NONE ||
                ((error & 0x2ULL) && !(anon->prot & LINUX_PROT_WRITE)) ||
                ((error & 0x10ULL) && !(anon->prot & LINUX_PROT_EXEC))) {
                return 0;
            }
            int rc = task_map_anonymous_page(task, anon, page);
            return rc == 0 ? 1 : rc == -RELIEFOS_ENOMEM ? -RELIEFOS_ENOMEM : 0;
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
    if (task_stack_fault_candidate(task, page, error)) {
        struct task_address_space_state *mm = sched_task_mm(task);
        uint64_t top = mm->initial_stack_top ? mm->initial_stack_top : task->stack_top;
        uint64_t guard = top - (uint64_t)RELIEFNT_USER_STACK_MAX_PAGES * PAGE_SIZE - PAGE_SIZE;
        if (!task_stack_growth_allowed(task, page)) return 0;
        uint64_t old_low = mm->initial_stack_low ? mm->initial_stack_low : task->stack_low;
        uint32_t mode = mm_lock_flags(sched_task_as(task));
        if (page < old_low && (mode & LINUX_MCL_FUTURE)) {
            if (page <= guard) return 0;
            int rc = mm_lock_future_stack(task, page, old_low, mode);
            if (rc < 0) return rc == -RELIEFOS_ENOMEM ? -RELIEFOS_ENOMEM : 0;
            if (task->stack_top == top && page < task->stack_low) task->stack_low = page;
            mm->initial_stack_low = page;
            return 1;
        }
        if (page > guard && address_space_map_user_stack_page(sched_task_as(task), page)) {
            if (task->stack_top == top && page < task->stack_low) {
                task->stack_low = page;
            }
            if (!mm->initial_stack_low || page < mm->initial_stack_low) mm->initial_stack_low = page;
            return 1;
        }
        return page > guard ? -RELIEFOS_ENOMEM : 0;
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
        bool did_read = false;
        int ret = task_map_file_vma_page_observed(task, vma, page, &did_read, defer_oom_diagnostic);
        userland_loader_unlock(loader_flags);
        if (record_major && did_read && memory_event_add && sched_fault_event_major &&
            sched_fault_event_major(task)) memory_event_add(MEMORY_EVENT_PGMAJFAULT, 1);
        if (ret < 0) {
            if ((vma->file_node.flags & STORAGE_NODE_FLAG_TMPFS) &&
                (vma->flags & TASK_VMA_FLAG_MMAP) &&
                (ret == -RELIEFOS_EINVAL || ret == -RELIEFOS_ENOSPC))
                task->page_fault_signal = LINUX_SIGBUS;
            if (!defer_oom_diagnostic || ret != -RELIEFOS_ENOMEM)
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
        return ret == 0 ? 1 : ret == -RELIEFOS_ENOMEM ? -RELIEFOS_ENOMEM : 0;
    }
}

/** @brief Preserve the historical boolean task-fault contract.
 * @param task Borrowed task. @param fault_addr Address. @param error x86 flags.
 * @return One only when resolved; all other results retain the old zero.
 */
int syscall_handle_task_page_fault(struct task *task, uint64_t fault_addr, uint64_t error)
{ return task_page_fault_detailed(task, fault_addr, error, false, false) > 0; }

/** @brief Resolve a user fault in the current task's address space. */
int syscall_handle_user_page_fault(uint64_t fault_addr, uint64_t error)
{
    return syscall_handle_task_page_fault(sched_current_task(), fault_addr, error);
}

/** @brief Resolve a current task fault while retaining allocation failure.
 * @param fault_addr Faulting user byte address. @param error x86 fault flags.
 * @param user_fault True for a real ring-3 hardware fault.
 * @return One if resolved, -ENOMEM on admitted allocation failure, or zero;
 * caller holds execution ownership. Hardware-fault callers also hold the
 * fault lock; held locks are retained on return.
 */
int syscall_handle_user_page_fault_detailed(uint64_t fault_addr, uint64_t error, bool user_fault)
{ return task_page_fault_detailed(sched_current_task(), fault_addr, error, user_fault, user_fault); }
