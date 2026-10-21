#include "memory_internal.h"

/**
 * Task vma attrs match.
 * @param vma Value supplied by the caller.
 * @param prot Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @return The value or status produced by the operation.
 */
static int task_vma_attrs_match(const struct task_vma *vma, uint64_t prot, uint32_t flags);

/**
 * Storage nodes equal.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int storage_nodes_equal(const struct storage_node *a, const struct storage_node *b);

/**
 * Task vma file attrs match.
 * @param vma Value supplied by the caller.
 * @param file_node Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int task_vma_file_attrs_match(const struct task_vma *vma,
                                     const struct storage_node *file_node);

/**
 * Task vma left adjacent.
 * @param task Value supplied by the caller.
 * @param start Value supplied by the caller.
 * @param prot Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @param file_node Value supplied by the caller.
 * @param file_offset Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static struct task_vma *task_vma_left_adjacent(struct task *task, uint64_t start,
                                                uint64_t prot, uint32_t flags,
                                                const struct storage_node *file_node,
                                                uint64_t file_offset, uint32_t max_prot);

/**
 * Task vma right adjacent.
 * @param task Value supplied by the caller.
 * @param end Value supplied by the caller.
 * @param prot Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @param file_node Value supplied by the caller.
 * @param file_offset Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
static struct task_vma *task_vma_right_adjacent(struct task *task, uint64_t end,
                                                 uint64_t prot, uint32_t flags,
                                                 const struct storage_node *file_node,
                                                 uint64_t file_offset,
                                                 uint64_t len, uint32_t max_prot);


/**
 * Task vma free slot.
 * @param task Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
struct task_vma *task_vma_free_slot(struct task *task)
{
    if (!task) {
        return NULL;
    }
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (vma && !vma->used) {
            return vma;
        }
    }
    return sched_task_vma_at(task, sched_task_vma_capacity(task));
}

/**
 * Find a free VMA slot without retaining a pointer across metadata growth.
 * @param task User task whose VMA store is searched.
 * @param index_out Receives the stable VMA slot index.
 * @return Zero on success or a negative errno when the store cannot grow.
 */
int task_vma_free_slot_index(struct task *task, uint32_t *index_out)
{
    if (!task || !index_out) return -RELIEFOS_EINVAL;
    uint32_t capacity = sched_task_vma_capacity(task);
    for (uint32_t i = 0; i < capacity; ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (vma && !vma->used) {
            *index_out = i;
            return 0;
        }
    }
    if (!sched_task_vma_at(task, capacity)) return -RELIEFOS_ENOMEM;
    *index_out = capacity;
    return 0;
}

/**
 * Task vma attrs match.
 * @param vma Value supplied by the caller.
 * @param prot Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @return The value or status produced by the operation.
 */
static int task_vma_attrs_match(const struct task_vma *vma, uint64_t prot, uint32_t flags)
{
    return vma && vma->used && vma->prot == (uint32_t)prot && vma->flags == flags;
}

/**
 * Storage nodes equal.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int storage_nodes_equal(const struct storage_node *a, const struct storage_node *b)
{
    return a && b &&
           a->type == b->type &&
           a->flags == b->flags &&
           a->first_cluster == b->first_cluster &&
           a->volume_id == b->volume_id &&
           a->size == b->size;
}

/**
 * Task vma file attrs match.
 * @param vma Value supplied by the caller.
 * @param file_node Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int task_vma_file_attrs_match(const struct task_vma *vma,
                                     const struct storage_node *file_node)
{
    if (!vma || !(vma->flags & TASK_VMA_FLAG_FILE)) {
        return 1;
    }
    return storage_nodes_equal(&vma->file_node, file_node) &&
           file_node && vma->file_limit == file_node->size;
}

/**
 * Task vma clear.
 * @param vma Value supplied by the caller.
 */
void task_vma_clear(struct task_vma *vma)
{
    if (vma) {
        sysv_shm_vma_release(sched_current_task(), vma);
        if (vma->audio_pcm) audio_pcm_mmap_release(vma->audio_pcm, (enum audio_pcm_mmap_region)vma->audio_mmap_region);
        if (vma->drm_bo) drm_bo_put(vma->drm_bo);
        if (vma->drm_mapping_file) task_file_put(vma->drm_mapping_file);
        (void)storage_inode_put(vma->inode);
        *vma = (struct task_vma){0};
    }
}

/**
 * @brief Give a copied DRM VMA segment independent backing and OFD pins.
 * @param dst Output pointer to the copied destination VMA whose DRM references
 *        do not yet own pins; may be null, in which case no fields are changed.
 * @param src Borrowed input pointer to a stable source VMA; its optional BO and
 *        task_file references are retained for dst. It may be null, in which
 *        case no fields are changed.
 * @return No value; installs independent backing and OFD pins in dst in place.
 *
 * No VMA-store lock is acquired and no VMA address bounds are changed here; the
 * caller keeps the source metadata stable and sets the destination segment's
 * range. This helper only replaces copied DRM pointers with independently
 * retained pins.
 */
void task_vma_retain_drm(struct task_vma *dst,
                                const struct task_vma *src)
{
    if (!dst || !src) return;
    dst->drm_bo = NULL;
    dst->drm_mapping_file = NULL;
    if (src->drm_bo) {
        drm_bo_get(src->drm_bo);
        dst->drm_bo = src->drm_bo;
    }
    if (src->drm_mapping_file)
        dst->drm_mapping_file = task_file_get(src->drm_mapping_file);
}

/**
 * Task vma containing.
 * @param task Value supplied by the caller.
 * @param start Value supplied by the caller.
 * @param end Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
struct task_vma *task_vma_containing(struct task *task, uint64_t start, uint64_t end)
{
    if (!task) {
        return NULL;
    }
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (vma && vma->used && start >= vma->start && end <= vma->end) {
            return vma;
        }
    }
    return NULL;
}

/**
 * Task vma left adjacent.
 * @param task Value supplied by the caller.
 * @param start Value supplied by the caller.
 * @param prot Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @param file_node Value supplied by the caller.
 * @param file_offset Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static struct task_vma *task_vma_left_adjacent(struct task *task, uint64_t start,
                                                uint64_t prot, uint32_t flags,
                                                const struct storage_node *file_node,
                                                uint64_t file_offset, uint32_t max_prot)
{
    if (!task) {
        return NULL;
    }
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (!vma) continue;
        if (task_vma_attrs_match(vma, prot, flags) && vma->max_prot == max_prot &&
            vma->end == start &&
            task_vma_file_attrs_match(vma, file_node) &&
            (!(flags & TASK_VMA_FLAG_FILE) ||
             vma->file_offset + (vma->end - vma->start) == file_offset)) {
            return vma;
        }
    }
    return NULL;
}

/**
 * Task vma right adjacent.
 * @param task Value supplied by the caller.
 * @param end Value supplied by the caller.
 * @param prot Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @param file_node Value supplied by the caller.
 * @param file_offset Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
static struct task_vma *task_vma_right_adjacent(struct task *task, uint64_t end,
                                                 uint64_t prot, uint32_t flags,
                                                 const struct storage_node *file_node,
                                                 uint64_t file_offset,
                                                 uint64_t len, uint32_t max_prot)
{
    if (!task) {
        return NULL;
    }
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (!vma) continue;
        if (task_vma_attrs_match(vma, prot, flags) && vma->max_prot == max_prot &&
            vma->start == end &&
            task_vma_file_attrs_match(vma, file_node) &&
            (!(flags & TASK_VMA_FLAG_FILE) ||
             file_offset + len == vma->file_offset)) {
            return vma;
        }
    }
    return NULL;
}

/**
 * Task vma can record mapping.
 * @param task Value supplied by the caller.
 * @param start Value supplied by the caller.
 * @param end Value supplied by the caller.
 * @param prot Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @param file_node Value supplied by the caller.
 * @param file_offset Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int task_vma_can_record_mapping(struct task *task, uint64_t start, uint64_t end,
                                       uint64_t prot, uint32_t flags,
                                       const struct storage_node *file_node,
                                       uint64_t file_offset, uint32_t max_prot)
{
    uint64_t len = end - start;
    if (flags & TASK_VMA_FLAG_DEVICE) return task_vma_free_slot(task) != NULL;
    return task_vma_left_adjacent(task, start, prot, flags, file_node, file_offset, max_prot) ||
           task_vma_right_adjacent(task, end, prot, flags, file_node, file_offset, len, max_prot) ||
           task_vma_free_slot(task);
}

/**
 * Task vma record mapping.
 * @param task Value supplied by the caller.
 * @param start Value supplied by the caller.
 * @param end Value supplied by the caller.
 * @param prot Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @param file_node Value supplied by the caller.
 * @param file_offset Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int task_vma_record_mapping(struct task *task, uint64_t start, uint64_t end,
                                   uint64_t prot, uint32_t flags,
                                   const struct storage_node *file_node,
                                   uint64_t file_offset, uint32_t max_prot)
{
    uint64_t len = end - start;
    struct task_vma *left = (flags & TASK_VMA_FLAG_DEVICE) ? NULL :
        task_vma_left_adjacent(task, start, prot, flags, file_node, file_offset, max_prot);
    struct task_vma *right = (flags & TASK_VMA_FLAG_DEVICE) ? NULL :
        task_vma_right_adjacent(task, end, prot, flags, file_node, file_offset, len, max_prot);
    if (left && right && left != right) {
        left->end = right->end;
        task_vma_clear(right);
        return 0;
    }
    if (left) {
        left->end = end;
        return 0;
    }
    if (right) {
        right->start = start;
        if (flags & TASK_VMA_FLAG_FILE) right->file_offset = file_offset;
        return 0;
    }

    struct task_vma *slot = task_vma_free_slot(task);
    if (!slot) {
        return -RELIEFOS_ENOMEM;
    }
    struct storage_inode_ref *inode = NULL;
    int ret = file_node ? storage_inode_get(file_node, &inode) : 0;
    if (ret < 0) return ret;
    slot->inode = inode;
    slot->used = 1;
    slot->prot = (uint32_t)prot;
    slot->max_prot = max_prot;
    slot->flags = flags;
    slot->reserved = 0;
    slot->start = start;
    slot->end = end;
    slot->file_offset = file_offset;
    slot->file_limit = file_node ? file_node->size : 0;
    if (file_node) {
        slot->file_node = *file_node;
    } else {
        slot->file_node = (struct storage_node){0};
    }
    return 0;
}

/**
 * Task vma overlaps.
 * @param task Value supplied by the caller.
 * @param start Value supplied by the caller.
 * @param end Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int task_vma_overlaps(const struct task *task, uint64_t start, uint64_t end)
{
    if (!task) {
        return 1;
    }
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        const struct task_vma *vma = sched_task_vma_at((struct task *)task, i);
        if (vma && vma->used && start < vma->end && end > vma->start) {
            return 1;
        }
    }
    return 0;
}

/**
 * Task vma count.
 * @param task Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint32_t task_vma_count(const struct task *task)
{
    uint32_t count = 0;
    if (!task) {
        return 0;
    }
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        const struct task_vma *vma = sched_task_vma_at((struct task *)task, i);
        if (vma && vma->used) {
            ++count;
        }
    }
    return count;
}

uint64_t task_vma_total_bytes(const struct task *task)
{
    uint64_t total = 0;
    if (!task) {
        return 0;
    }
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        const struct task_vma *vma = sched_task_vma_at((struct task *)task, i);
        if (!vma) continue;
        if (!vma->used || vma->end < vma->start) {
            continue;
        }
        if (UINT64_MAX - total < vma->end - vma->start) {
            return UINT64_MAX;
        }
        total += vma->end - vma->start;
    }
    const struct task_address_space_state *mm = sched_task_mm(task);
    uint64_t top = mm->initial_stack_top ? mm->initial_stack_top : task->stack_top;
    uint64_t low = mm->initial_stack_low ? mm->initial_stack_low : task->stack_low;
    if (top > low && low >= RELIEFNT_USER_BASE) {
        uint64_t stack_bytes = top - low;
        /* Initial stacks are not currently represented by ordinary VMAs. */
        for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
            const struct task_vma *vma = sched_task_vma_at((struct task *)task, i);
            if (!vma || !vma->used) continue;
            uint64_t start = low > vma->start ? low : vma->start;
            uint64_t end = top < vma->end ? top : vma->end;
            if (end > start) stack_bytes -= end - start;
        }
        if (UINT64_MAX - total < stack_bytes) return UINT64_MAX;
        total += stack_bytes;
    }
    return total;
}

/** @brief Check net virtual growth before MAP_FIXED destroys any existing VMA. */
bool task_address_space_can_map(const struct task *task, uint64_t start, uint64_t end)
{
    uint64_t limit = sched_task_limits(task)->as.rlim_cur & ~(PAGE_SIZE - 1ULL);
    if (sched_task_limits(task)->as.rlim_cur == LINUX_RLIM_INFINITY) return true;
    uint64_t used = task_vma_total_bytes(task), replaced = 0;
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        const struct task_vma *vma = sched_task_vma_at((struct task *)task, i);
        if (!vma || !vma->used) continue;
        uint64_t first = start > vma->start ? start : vma->start;
        uint64_t last = end < vma->end ? end : vma->end;
        if (last > first) replaced += last - first;
    }
    uint64_t growth = end - start - replaced;
    return used <= limit && growth <= limit - used;
}
