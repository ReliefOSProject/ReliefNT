#include "memory_internal.h"

static int mm_validate_range(struct task *task, uint64_t addr, uint64_t len,
                             uint64_t *end_out);


void syscall_mm_truncate_file(struct task *task, const struct storage_node *node, uint64_t size)
{
    uint64_t hole = align_up_page(size);
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (!vma || !vma->used || !(vma->flags & TASK_VMA_FLAG_FILE) ||
            vma->file_node.volume_id != node->volume_id ||
            vma->file_node.first_cluster != node->first_cluster ||
            vma->file_node.flags != node->flags) continue;
        uint64_t skip = hole > vma->file_offset ? hole - vma->file_offset : 0;
        if (skip < vma->end - vma->start)
            task_unmap_pages(task, vma->start + skip, vma->end);
    }
}

static int mm_validate_range(struct task *task, uint64_t addr, uint64_t len,
                             uint64_t *end_out)
{
    uint64_t mapped_len;
    if (!task || !len || (addr & (PAGE_SIZE - 1ULL)) ||
        addr < RELIEFNT_USER_BASE || addr >= RELIEFNT_USER_TOP ||
        align_user_len(len, &mapped_len) < 0 ||
        mapped_len > RELIEFNT_USER_TOP - addr) return -RELIEFOS_EINVAL;
    uint64_t end = addr + mapped_len;
    for (uint64_t page = addr; page < end; page += PAGE_SIZE) {
        if (!task_vma_containing(task, page, page + PAGE_SIZE)) return -RELIEFOS_ENOMEM;
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

int64_t syscall_mm_mincore(uint64_t addr, uint64_t len, uint64_t vec)
{
    struct task *task = sched_current_task();
    uint64_t end;
    int ret = mm_validate_range(task, addr, len, &end);
    if (ret < 0) return ret;
    uint64_t pages = (end - addr) / PAGE_SIZE;
    if (!user_range_writable(vec, pages)) return -RELIEFOS_EFAULT;
    uint8_t *out = (uint8_t *)(uintptr_t)vec;
    for (uint64_t i = 0; i < pages; ++i) {
        out[i] = address_space_user_page_phys(sched_task_as(task), addr + i * PAGE_SIZE) ? 1u : 0u;
    }
    return 0;
}

int64_t syscall_mm_madvise(uint64_t addr, uint64_t len, uint64_t advice)
{
    struct task *task = sched_current_task();
    switch (advice) {
    case LINUX_MADV_NORMAL:
    case LINUX_MADV_RANDOM:
    case LINUX_MADV_SEQUENTIAL:
    case LINUX_MADV_WILLNEED:
        return mm_validate_range(task, addr, len, NULL);
    default:
        return -RELIEFOS_EOPNOTSUPP;
    }
}
