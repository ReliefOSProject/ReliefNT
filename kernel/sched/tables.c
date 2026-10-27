#include <reliefnt/export.h>
#include "internal.h"
#include <reliefnt/exec_file.h>



struct task_vma *sched_task_vma_at(struct task *task, uint32_t index)
{
    if (!task) {
        return NULL;
    }
    if (index < SCHED_TASK_VMA_MAX) {
        return &sched_task_mm(task)->vmas[index];
    }
    index -= SCHED_TASK_VMA_MAX;
    if (index >= sched_task_mm(task)->vma_extra_count) {
        uint32_t wanted = index + 1u;
        /* Slots up to capacity are already zeroed. Appending within that
         * allocation must not relocate existing VMA entries. Growth beyond
         * capacity can still move the store, so callers must reacquire VMA
         * pointers after requesting new slots. */
        if (wanted <= sched_task_mm(task)->vma_extra_capacity) {
            sched_task_mm(task)->vma_extra_count = wanted;
            return &sched_task_mm(task)->vma_extra[index];
        }
        uint32_t capacity = sched_task_mm(task)->vma_extra_capacity ? sched_task_mm(task)->vma_extra_capacity : 16u;
        struct task_vma *replacement;
        while (capacity < wanted) {
            if (capacity > UINT32_MAX / 2u) {
                return NULL;
            }
            capacity *= 2u;
        }
        replacement = (struct task_vma *)kernel_malloc(
            (size_t)capacity * sizeof(*replacement));
        if (!replacement) {
            return NULL;
        }
        for (uint32_t i = 0; i < capacity; ++i) {
            replacement[i] = (struct task_vma){0};
        }
        for (uint32_t i = 0; i < sched_task_mm(task)->vma_extra_count; ++i) {
            replacement[i] = sched_task_mm(task)->vma_extra[i];
        }
        if (sched_task_mm(task)->vma_extra) {
            kernel_free(sched_task_mm(task)->vma_extra);
        }
        sched_task_mm(task)->vma_extra = replacement;
        sched_task_mm(task)->vma_extra_capacity = capacity;
        sched_task_mm(task)->vma_extra_count = wanted;
    }
    return &sched_task_mm(task)->vma_extra[index];
}

uint32_t sched_task_vma_capacity(const struct task *task)
{
    return task ? SCHED_TASK_VMA_MAX + sched_task_mm(task)->vma_extra_count : 0;
}

/** @brief Release all VMA and executable references of a retiring MM.
 * @param task Task whose final MM ownership has been established by caller,
 * or NULL. Borrowed copied resources must have been cleared before rollback.
 * Execution-owned task context; drops inode references before the executable
 * path's last provider pin. Final puts may perform I/O, so no scheduler,
 * filesystem graph or module-table spinlock may span this operation.
 */
void sched_task_vma_release(struct task *task)
{
    if (!task) return;
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (vma) {
            sysv_shm_vma_release(task, vma);
            if (vma->device_mapping) device_mapping_put(vma->device_mapping);
            if (vma->mapping_file) task_file_put(vma->mapping_file);
            vfs_exec_source_put(vma->exec_source);
            (void)storage_inode_put(vma->inode);
            vma->inode = NULL;
            vma->device_mapping = NULL;
            vma->mapping_file = NULL;
            vma->exec_source = NULL;
        }
    }
    if(sched_task_mm(task)->executable_denied_write)storage_inode_allow_write(sched_task_mm(task)->executable_inode);
    sched_task_mm(task)->executable_denied_write=false;
    (void)storage_inode_put(sched_task_mm(task)->executable_inode);
    sched_task_mm(task)->executable_inode = NULL;
    vfs_path_put(&sched_task_mm(task)->executable_path);
    if (!sched_task_mm(task)->vma_extra) return;
    kernel_free(sched_task_mm(task)->vma_extra);
    sched_task_mm(task)->vma_extra = NULL;
    sched_task_mm(task)->vma_extra_count = 0;
    sched_task_mm(task)->vma_extra_capacity = 0;
}

/** @brief Share an independently pinned device backing with a copied VMA.
 * @param dst Child VMA with no acquired device reference yet.
 * @param src Parent VMA retaining its original backing.
 * @return Zero or negative errno; failure leaves dst unowned. Execution-owned
 * task context, no backend callback or allocation spans fork/split admission.
 */
int sched_task_vma_retain_device(struct task_vma *dst,const struct task_vma *src)
{
    if(!dst || !src || !src->device_mapping)return 0;
    int rc=device_mapping_get(src->device_mapping);if(rc)return rc;
    dst->device_mapping=src->device_mapping;dst->device_map_offset=src->device_map_offset;return 0;
}

/** @brief Retain a copied VMA's source OFD/ELF provider.
 * @param dst Child VMA with no owned file pins yet, required.
 * @param src Parent VMA held stable under the execution transaction, required.
 * @return Zero or negative EINVAL/ENOMEM. No reference is transferred on failure;
 * Canonical OFD and ELF source references do not allocate; the latter keeps
 * provider code and immutable bytes alive through fork, split and lazy faults.
 */
int sched_task_vma_retain_file(struct task_vma *dst,
                                     const struct task_vma *src)
{
    struct task_file *mapping_file = NULL;
    if (!dst || !src) return -LINUX_EINVAL;
    if (src->mapping_file) {
        mapping_file = task_file_get(src->mapping_file);
        if (!mapping_file) return -LINUX_ENOMEM;
    }
    dst->mapping_file = mapping_file;
    dst->exec_source = vfs_exec_source_get(src->exec_source);
    return 0;
}

/** @brief Borrow a live numeric descriptor slot without promoting its OFD.
 * @param task Execution-owned task/fd table, nullable. @param fd Linux descriptor number.
 * @return Borrowed live slot or NULL, including stdio/grown tables. No reference
 * is acquired, no allocation/lock/wait occurs; retain the canonical description
 * before any operation that can suspend ownership or permit fd-table reuse.
 */
struct task_file *task_descriptor_for_fd(struct task *task, int fd)
{
    if (!task || fd < 0) return NULL;
    if (fd < 3) return sched_task_fds(task)->stdio_files[fd].used
        ? &sched_task_fds(task)->stdio_files[fd] : NULL;
    if (fd >= 3 + (int)sched_task_file_capacity(task)) return NULL;
    struct task_file *file = sched_task_file_at(task, (uint32_t)(fd - 3));
    return file && file->used ? file : NULL;
}

struct task_file *sched_task_file_at(struct task *task, uint32_t index)
{
    if (!task) {
        return NULL;
    }
    if (index < SCHED_TASK_FILE_MAX) {
        return &sched_task_fds(task)->files[index];
    }
    index -= SCHED_TASK_FILE_MAX;
    if (index >= sched_task_fds(task)->file_extra_count) {
        uint32_t wanted = index + 1u;
        uint32_t capacity = sched_task_fds(task)->file_extra_capacity ? sched_task_fds(task)->file_extra_capacity : 16u;
        struct task_file *replacement;
        while (capacity < wanted) {
            if (capacity > UINT32_MAX / 2u) {
                return NULL;
            }
            capacity *= 2u;
        }
        replacement = (struct task_file *)kernel_malloc(
            (size_t)capacity * sizeof(*replacement));
        if (!replacement) {
            return NULL;
        }
        for (uint32_t i = 0; i < capacity; ++i) {
            replacement[i] = (struct task_file){0};
        }
        for (uint32_t i = 0; i < sched_task_fds(task)->file_extra_count; ++i) {
            replacement[i] = sched_task_fds(task)->file_extra[i];
        }
        if (sched_task_fds(task)->file_extra) {
            kernel_free(sched_task_fds(task)->file_extra);
        }
        sched_task_fds(task)->file_extra = replacement;
        sched_task_fds(task)->file_extra_capacity = capacity;
        sched_task_fds(task)->file_extra_count = wanted;
    }
    return &sched_task_fds(task)->file_extra[index];
}

uint32_t sched_task_file_capacity(const struct task *task)
{
    return task ? SCHED_TASK_FILE_MAX + sched_task_fds(task)->file_extra_count : 0;
}

void sched_task_file_release(struct task *task)
{
    if (!task || !sched_task_fds(task)->file_extra) {
        return;
    }
    kernel_free(sched_task_fds(task)->file_extra);
    sched_task_fds(task)->file_extra = NULL;
    sched_task_fds(task)->file_extra_count = 0;
    sched_task_fds(task)->file_extra_capacity = 0;
}

/**
 * @brief Return the number of indexable slots in the task's PTY alias table.
 * @param task Descriptor-table owner; NULL returns zero.
 * @return Slot count, including reusable holes.
 */
uint32_t sched_task_pty_capacity(const struct task *task)
{
    return task ? sched_task_fds(task)->pty_fd_count : 0;
}

/**
 * @brief Resolve an existing PTY alias slot without changing table state.
 * @param task Descriptor-table owner; NULL is accepted.
 * @param index Zero-based index within the logical PTY alias table.
 * @return Borrowed slot or NULL when outside the logical slot count.
 * @note A later PTY table growth can invalidate the returned pointer.
 */
struct task_pty_fd *sched_task_pty_at(struct task *task, uint32_t index)
{
    if (!task || index >= sched_task_fds(task)->pty_fd_count) return NULL;
    return &sched_task_fds(task)->pty_fds[index];
}

/**
 * @brief Grow a detached descriptor table's PTY alias allocation.
 * @param table Descriptor table whose owned PTY vector may be replaced; required.
 * @param wanted Number of slots needed without changing the logical count.
 * @return Zero on success or negative EINVAL/ENOMEM with existing storage intact.
 * @note Successful growth invalidates pointers borrowed from the previous vector.
 * The caller must serialize table mutation and must not retain entry pointers across growth.
 */
int sched_task_pty_table_reserve(struct task_fd_table_state *table, uint32_t wanted)
{
    struct task_pty_fd *replacement;
    uint32_t capacity;
    if (!table || wanted < table->pty_fd_count) return -RELIEFOS_EINVAL;
    if (wanted <= table->pty_fd_capacity) return 0;
    capacity = table->pty_fd_capacity ? table->pty_fd_capacity : 16u;
    while (capacity < wanted) {
        if (capacity > UINT32_MAX / 2u) return -RELIEFOS_ENOMEM;
        capacity *= 2u;
    }
    if ((size_t)capacity > SIZE_MAX / sizeof(*replacement)) return -RELIEFOS_ENOMEM;
    replacement = kernel_malloc((size_t)capacity * sizeof(*replacement));
    if (!replacement) return -RELIEFOS_ENOMEM;
    for (uint32_t i = 0; i < capacity; ++i)
        replacement[i] = (struct task_pty_fd){0};
    for (uint32_t i = 0; i < table->pty_fd_count; ++i)
        replacement[i] = table->pty_fds[i];
    kernel_free(table->pty_fds);
    table->pty_fds = replacement;
    table->pty_fd_capacity = capacity;
    return 0;
}

/**
 * @brief Reuse a free PTY alias slot or append one after successful growth.
 * @param task Descriptor-table owner; required.
 * @param out Receives the empty slot; required.
 * @return Zero on success or negative EINVAL/ENOMEM without publishing a slot.
 * @note The returned pointer is borrowed and may be invalidated by later growth.
 */
int sched_task_pty_alloc(struct task *task, struct task_pty_fd **out)
{
    struct task_fd_table_state *table;
    if (!task || !out) return -RELIEFOS_EINVAL;
    table = sched_task_fds(task);
    for (uint32_t i = 0; i < table->pty_fd_count; ++i) {
        if (!table->pty_fds[i].used) {
            *out = &table->pty_fds[i];
            return 0;
        }
    }
    if (table->pty_fd_count == UINT32_MAX) return -RELIEFOS_ENOMEM;
    uint32_t index = table->pty_fd_count;
    int ret = sched_task_pty_table_reserve(table, index + 1u);
    if (ret < 0) return ret;
    table->pty_fd_count = index + 1u;
    *out = &table->pty_fds[index];
    return 0;
}

/**
 * @brief Free PTY alias storage after descriptor references and table sharing end.
 * @param task Descriptor-table owner whose entries are released and table is final; NULL is accepted.
 */
void sched_task_pty_release(struct task *task)
{
    if (!task) return;
    struct task_fd_table_state *table = sched_task_fds(task);
    kernel_free(table->pty_fds);
    table->pty_fds = NULL;
    table->pty_fd_count = 0;
    table->pty_fd_capacity = 0;
}

EXPORT_SYMBOL(task_descriptor_for_fd);
