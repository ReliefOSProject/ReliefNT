#include "memory_internal.h"
#include <reliefnt/heap.h>
#include <reliefnt/swap.h>
#ifdef CONFIG_SWAP
#include <reliefnt/memory_pressure.h>
#include <reliefnt/futex.h>
#include <linux/errno.h>
#endif

/** @brief Validate a mincore interval against VMAs and the implicit stack.
 * @param task Pinned user MM. @param addr Aligned first byte.
 * @param len Byte count. @param end_out Optional rounded exclusive end.
 * @return Zero or negative EINVAL/ENOMEM without allocating user pages.
 */
static int mm_validate_range(struct task *task, uint64_t addr, uint64_t len,
                             uint64_t *end_out);


/** @brief Revoke complete pages beyond EOF from every alias of a backing inode.
 * @param task User task whose retained file VMAs are protected by execution ownership.
 * @param node Backing inode identity, which may omit anonymous-path decoration.
 * @param size Committed nonnegative file size in bytes.
 * Includes shared and private COW pages. MEMFD describes the anonymous path,
 * not a separate tmpfs inode; volume, inode number and other flags still match.
 */
void syscall_mm_truncate_file(struct task *task, const struct storage_node *node, uint64_t size)
{
    uint64_t hole = align_up_page(size);
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (!vma || !vma->used || !(vma->flags & TASK_VMA_FLAG_FILE) ||
            vma->file_node.volume_id != node->volume_id ||
            vma->file_node.first_cluster != node->first_cluster ||
            ((vma->file_node.flags ^ node->flags) & ~STORAGE_NODE_FLAG_MEMFD)) continue;
        uint64_t skip = hole > vma->file_offset ? hole - vma->file_offset : 0;
        if (skip < vma->end - vma->start)
            task_unmap_pages(task, vma->start + skip, vma->end);
    }
}

/** @brief Check a rounded residency interval and its implicit stack coverage.
 * @param task Execution-owned user MM. @param addr Aligned first byte.
 * @param len Requested bytes. @param end_out Optional exclusive rounded end.
 * @return Zero or negative EINVAL/ENOMEM.
 */
static int mm_validate_range(struct task *task, uint64_t addr, uint64_t len,
                             uint64_t *end_out)
{
    if (addr & (PAGE_SIZE - 1ULL)) return -RELIEFOS_EINVAL;
    if (!task || addr >= RELIEFNT_USER_TOP || len > UINT64_MAX - (PAGE_SIZE - 1ULL))
        return -RELIEFOS_ENOMEM;
    uint64_t mapped_len = align_up_page(len);
    if (mapped_len > RELIEFNT_USER_TOP - addr) return -RELIEFOS_ENOMEM;
    uint64_t end = addr + mapped_len;
    if (mapped_len && addr < RELIEFNT_USER_BASE) return -RELIEFOS_ENOMEM;
    const struct task_address_space_state *mm = sched_task_mm(task);
    uint64_t low = mm->initial_stack_low ? mm->initial_stack_low : task->stack_low;
    uint64_t top = mm->initial_stack_top ? mm->initial_stack_top : task->stack_top;
    for (uint64_t page = addr; page < end;) {
        struct task_vma *vma = task_vma_containing(task, page, page + PAGE_SIZE);
        if (vma) page = vma->end < end ? vma->end : end;
        else if (low && page >= low && page < top) page = top < end ? top : end;
        else return -RELIEFOS_ENOMEM;
    }
    if (end_out) *end_out = end;
    return 0;
}

int64_t syscall_mm_msync(uint64_t addr, uint64_t len, uint64_t flags)
{
    struct task *task = sched_current_task();
    if ((addr & (PAGE_SIZE - 1)) || (flags & ~(uint64_t)(LINUX_MS_ASYNC | LINUX_MS_INVALIDATE |
                                        LINUX_MS_SYNC)) ||
        ((flags & LINUX_MS_ASYNC) && (flags & LINUX_MS_SYNC))) {
        return -RELIEFOS_EINVAL;
    }
    if (len > UINT64_MAX - (PAGE_SIZE - 1)) return -RELIEFOS_ENOMEM;
    uint64_t mapped_len = align_up_page(len);
    if (mapped_len > UINT64_MAX - addr) return -RELIEFOS_ENOMEM;
    if (!mapped_len) return 0;
    if (!task || addr < RELIEFNT_USER_BASE || addr + mapped_len > RELIEFNT_USER_TOP)
        return -RELIEFOS_ENOMEM;
    int ret = 0;
    for (uint64_t page = addr; page < addr + mapped_len;) {
        struct task_vma *vma = task_vma_containing(task, page, page + PAGE_SIZE);
        if (!vma) { ret = -RELIEFOS_ENOMEM; page += PAGE_SIZE; continue; }
        if ((flags & LINUX_MS_INVALIDATE) && (vma->flags & TASK_VMA_FLAG_LOCKED))
            return -RELIEFOS_EBUSY;
        if ((flags & LINUX_MS_SYNC) && (vma->flags & TASK_VMA_FLAG_SHARED_FILE) &&
            !(vma->file_node.flags & STORAGE_NODE_FLAG_TMPFS)) return -RELIEFOS_EOPNOTSUPP;
        /* tmpfs mappings already address the inode's pages. Its fsync is
         * noop_fsync in Linux; there is no separate dirty copy to write back. */
        page = vma->end;
    }
    return ret;
}

#ifdef CONFIG_SWAP
/* This task-owned lookup operand survives only matching syscall retries. */
struct mincore_progress { uint64_t addr, len, vec, end, next; };
#endif

/** @brief Snapshot PTE and backing-cache residency into a validated user vector.
 * @param addr Aligned first user page. @param len Requested byte length.
 * @param vec User output with one byte per rounded page.
 * @return Zero, negative EINVAL/ENOMEM/EFAULT, or a scoped prerequisite retry
 * marker consumed by syscall entry; residency may change after the snapshot.
 */
int64_t syscall_mm_mincore(uint64_t addr, uint64_t len, uint64_t vec)
{
    struct task *task = sched_current_task();
    uint64_t end;
#ifdef CONFIG_SWAP
    struct mincore_progress local={0},*progress=&local;
    struct task_lookup_path *slot=NULL;
    int ret=task_lookup_scope_next(task,&slot);
    if(ret)return ret;
    if(slot && slot->name) {
        if(slot->flags!=MINCORE_PROGRESS_KIND)return -RELIEFOS_EINVAL;
        progress=(struct mincore_progress *)slot->name;
        if(progress->addr!=addr || progress->len!=len || progress->vec!=vec)return -RELIEFOS_EINVAL;
        end=progress->end;
    } else {
        ret=mm_validate_range(task,addr,len,&end);
        if(ret<0)return ret;
        uint64_t count=(end-addr)/PAGE_SIZE;
        if(!count)return 0;
        if(vec<RELIEFNT_USER_BASE || vec>=RELIEFNT_USER_TOP || count>RELIEFNT_USER_TOP-vec)
            return -RELIEFOS_EFAULT;
        if(slot) {
            progress=kernel_malloc(sizeof(*progress));if(!progress)return -RELIEFOS_ENOMEM;
            slot->name=(char *)progress;slot->flags=MINCORE_PROGRESS_KIND;
        }
        *progress=(struct mincore_progress){addr,len,vec,end,0};
    }
#else
    int ret = mm_validate_range(task, addr, len, &end);
    if (ret < 0) return ret;
#endif
    uint64_t pages = (end - addr) / PAGE_SIZE;
    if (!pages) return 0;
#ifndef CONFIG_SWAP
    if (!user_range_writable(vec, pages)) return -RELIEFOS_EFAULT;
#endif
    uint8_t *out = (uint8_t *)(uintptr_t)vec;
    uint64_t i=0;
#ifdef CONFIG_SWAP
    i=progress->next;
#endif
    while(i<pages) {
        uint64_t count=pages-i;
#ifdef CONFIG_SWAP
        /* One output page can be restored with a single ordinary frame. A
         * completed page may subsequently be evicted without losing progress. */
        uint64_t page_bytes=PAGE_SIZE-((vec+i)&(PAGE_SIZE-1ULL));
        if(count>page_bytes)count=page_bytes;
        /* Check only the not-yet-reported interval after a real retry; the
         * original admission already checked the entire target VMA range. */
        ret=mm_validate_range(task,addr+i*PAGE_SIZE,count*PAGE_SIZE,NULL);
        if(ret<0)return ret;
        ret=swap_prepare_user_range(task,vec+i,count,true);
        if(ret==SWAP_STORAGE_CONTENDED)return KERNEL_SYSCALL_SWAP_CONTENDED;
        if(ret==-ENOMEM && slot)return KERNEL_SYSCALL_MINCORE_RECLAIM;
        if(ret<0)return -RELIEFOS_EFAULT;
        if(!user_range_writable(vec+i,count)) {
            /* user_range_writable collapses a valid demand/COW allocation
             * shortage into false. Preserve it for the existing scoped
             * reclaim path, then revalidate before writing the output. */
            uint64_t output=vec+i;
            uint64_t error=6u;
            if(paging_user_entry_kind(sched_task_as(task),output)==PAGING_USER_ENTRY_RESIDENT)
                error|=1u;
            int fault=syscall_handle_user_page_fault_detailed(output,error,false);
            if(fault==-ENOMEM && slot)return KERNEL_SYSCALL_MINCORE_RECLAIM;
            if(fault<=0 || !user_range_writable(output,count))return -RELIEFOS_EFAULT;
        }
#endif
        uint64_t limit=i+count;
        for(;i<limit;++i) {
        uint64_t page = addr + i * PAGE_SIZE;
        bool resident = address_space_user_page_phys(sched_task_as(task), page) != 0;
        struct task_vma *vma = task_vma_containing(task, page, page + PAGE_SIZE);
        if (!resident && vma && (vma->flags & TASK_VMA_FLAG_SYSV_SHM))
            resident = sysv_shm_page_resident(vma, page);
        if (!resident && vma &&
            (vma->flags & (TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_SHARED)) ==
                (TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_SHARED))
            resident = mm_shared_resident(sched_task_as(task), page);
        if (!resident && vma && (vma->flags & TASK_VMA_FLAG_FILE)) {
            uint64_t offset = vma->file_offset + page - vma->start;
            if (vma->file_node.flags & STORAGE_NODE_FLAG_TMPFS)
                resident = storage_tmpfs_page_resident(&vma->file_node, offset);
            else {
                uint64_t phys;
                if (!page_cache_lookup(&vma->file_node, offset, &phys)) {
                    resident = true;
                    page_cache_release(phys);
                }
            }
        }
        out[i] = resident ? 1u : 0u;
        }
#ifdef CONFIG_SWAP
        progress->next=i;
#endif
    }
    return 0;
}

#ifdef CONFIG_SWAP
/** @brief Reclaim for an admitted mincore output and retry in the same writer.
 * @param task Current live task with its own lookup continuation.
 * @param frame Borrowed syscall registers. @return mincore result, retry marker,
 * blocked marker, or terminal EFAULT. No borrowed MM, PTE or lookup pointer
 * survives this call; the timed wait is published before writer release.
 */
int64_t syscall_mm_mincore_reclaim(struct task *task, const struct trap_frame *frame)
{
    if (!task || task!=sched_current_task() || !frame || frame->rax!=LINUX_SYS_MINCORE ||
        frame->rip<2 || !kernel_execution_is_outermost()) return -RELIEFOS_EFAULT;
    struct task_lookup_path *slot=task_lookup_scope_at(task,0);
    if (!slot || slot->flags!=MINCORE_PROGRESS_KIND || !slot->name) return -RELIEFOS_EFAULT;
    const struct mincore_progress *p=(const struct mincore_progress *)slot->name;
    if (p->addr!=frame->rdi || p->len!=frame->rsi || p->vec!=frame->rdx ||
        p->end<p->addr || ((p->end-p->addr)&(PAGE_SIZE-1ULL)) ||
        p->next>=(p->end-p->addr)/PAGE_SIZE || p->vec>=RELIEFNT_USER_TOP ||
        p->next>=RELIEFNT_USER_TOP-p->vec) return -RELIEFOS_EFAULT;
    uint64_t address=p->vec+p->next;
    memory_reclaim_fault_scope_begin(task,frame->rip-2,address);
    int rc=memory_fault_oom(task,address);
    memory_reclaim_fault_scope_end();
    if (rc==-EAGAIN) return KERNEL_SYSCALL_BLOCKED;
    if (rc) return -RELIEFOS_EFAULT;
    /* Consume a released frame before another task can take it. The saved
     * scope carries only scalar progress; recheck the remaining VMA and PTE. */
    task_lookup_scope_begin(task,frame);
    return syscall_mm_mincore(frame->rdi,frame->rsi,frame->rdx);
}
#endif

/** @brief Discard mapped leaves or apply a supported non-destructive hint.
 * @param addr Aligned first user page. @param len Requested byte length.
 * @param advice Low 32-bit Linux advice. @return Zero or a negative errno;
 *         valid portions around holes may already have been discarded.
 */
int64_t syscall_mm_madvise(uint64_t addr, uint64_t len, uint64_t advice)
{
    struct task *task = sched_current_task();
    uint32_t behavior = (uint32_t)advice;
    switch (behavior) {
    case LINUX_MADV_NORMAL:
    case LINUX_MADV_RANDOM:
    case LINUX_MADV_SEQUENTIAL:
    case LINUX_MADV_WILLNEED:
    case LINUX_MADV_DONTNEED:
        break;
    case LINUX_MADV_FREE:
    case LINUX_MADV_DONTFORK:
    case LINUX_MADV_DOFORK:
    case LINUX_MADV_MERGEABLE:
    case LINUX_MADV_UNMERGEABLE:
    case LINUX_MADV_HUGEPAGE:
    case LINUX_MADV_NOHUGEPAGE:
    case LINUX_MADV_DONTDUMP:
    case LINUX_MADV_DODUMP:
    case LINUX_MADV_WIPEONFORK:
    case LINUX_MADV_KEEPONFORK:
    case LINUX_MADV_COLD:
    case LINUX_MADV_PAGEOUT:
    case LINUX_MADV_POPULATE_READ:
    case LINUX_MADV_POPULATE_WRITE:
    case LINUX_MADV_DONTNEED_LOCKED:
        return -RELIEFOS_EOPNOTSUPP;
    default:
        return -RELIEFOS_EINVAL;
    }
    if (addr & (PAGE_SIZE - 1ULL)) return -RELIEFOS_EINVAL;
    if (len > UINT64_MAX - (PAGE_SIZE - 1ULL)) return -RELIEFOS_EINVAL;
    uint64_t length = align_up_page(len);
    if (length > UINT64_MAX - addr) return -RELIEFOS_EINVAL;
    if (!length) return 0;
    if (!task || addr < RELIEFNT_USER_BASE || addr >= RELIEFNT_USER_TOP ||
        length > RELIEFNT_USER_TOP - addr) return -RELIEFOS_ENOMEM;
    uint64_t end = addr + length;
    uint64_t cursor = addr;
    bool hole = false;
    while (cursor < end) {
        struct task_vma *vma = task_vma_containing(task, cursor, cursor + PAGE_SIZE);
        const struct task_address_space_state *mm = sched_task_mm(task);
        uint64_t stack_low = mm->initial_stack_low ? mm->initial_stack_low : task->stack_low;
        uint64_t stack_top = mm->initial_stack_top ? mm->initial_stack_top : task->stack_top;
        uint64_t piece_end;
        if (vma) {
            piece_end = vma->end < end ? vma->end : end;
            if (behavior == LINUX_MADV_DONTNEED) {
                if (vma->flags & (TASK_VMA_FLAG_LOCKED | TASK_VMA_FLAG_DEVICE |
                                  TASK_VMA_FLAG_VDSO | TASK_VMA_FLAG_VVAR))
                    return -RELIEFOS_EINVAL;
                task_unmap_pages(task, cursor, piece_end);
            }
        } else if (stack_low && cursor >= stack_low && cursor < stack_top) {
            piece_end = stack_top < end ? stack_top : end;
            for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
                struct task_vma *next = sched_task_vma_at(task, i);
                if (next && next->used && next->start > cursor && next->start < piece_end)
                    piece_end = next->start;
            }
            if (behavior == LINUX_MADV_DONTNEED) task_unmap_pages(task, cursor, piece_end);
        } else {
            hole = true;
            piece_end = end;
            if (stack_low > cursor && stack_low < piece_end) piece_end = stack_low;
            for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
                struct task_vma *next = sched_task_vma_at(task, i);
                if (next && next->used && next->start > cursor && next->start < piece_end)
                    piece_end = next->start;
            }
        }
        cursor = piece_end;
    }
    return hole ? -RELIEFOS_ENOMEM : 0;
}
