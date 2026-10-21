#include "internal.h"



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

void sched_task_vma_release(struct task *task)
{
    if (!task) return;
    for (uint32_t i = 0; i < sched_task_vma_capacity(task); ++i) {
        struct task_vma *vma = sched_task_vma_at(task, i);
        if (vma) {
            sysv_shm_vma_release(task, vma);
            if (vma->audio_pcm) audio_pcm_mmap_release(vma->audio_pcm, (enum audio_pcm_mmap_region)vma->audio_mmap_region);
            if (vma->drm_bo) drm_bo_put(vma->drm_bo);
            if (vma->drm_mapping_file) task_file_put(vma->drm_mapping_file);
            (void)storage_inode_put(vma->inode);
            vma->inode = NULL;
            vma->audio_pcm = NULL;
            vma->drm_bo = NULL;
            vma->drm_mapping_file = NULL;
        }
    }
    (void)storage_inode_put(sched_task_mm(task)->executable_inode);
    sched_task_mm(task)->executable_inode = NULL;
    if (!sched_task_mm(task)->vma_extra) return;
    kernel_free(sched_task_mm(task)->vma_extra);
    sched_task_mm(task)->vma_extra = NULL;
    sched_task_mm(task)->vma_extra_count = 0;
    sched_task_mm(task)->vma_extra_capacity = 0;
}

/**
 * @brief Retain the PCM mapping reference represented by a copied VMA.
 * @param dst Child VMA receiving the copied audio mapping metadata.
 * @param src Parent VMA whose mapping reference is still live.
 * @return Zero when no mapping exists or the reference was retained, else a negative errno.
 */
int sched_task_vma_retain_audio(struct task_vma *dst,
                                       const struct task_vma *src)
{
    if (!dst || !src || !src->audio_pcm) return 0;
    int ret = audio_pcm_mmap_retain(src->audio_pcm,
                          (enum audio_pcm_mmap_region)src->audio_mmap_region);
    if (ret < 0) return ret;
    dst->audio_pcm = src->audio_pcm;
    dst->audio_mmap_region = src->audio_mmap_region;
    dst->audio_mmap_generation = src->audio_mmap_generation;
    dst->audio_mmap_prot = src->audio_mmap_prot;
    dst->audio_mmap_offset = src->audio_mmap_offset;
    dst->audio_mmap_length = src->audio_mmap_length;
    return 0;
}

/** @brief Retain the BO and OFD references represented by a copied DRM VMA. */
int sched_task_vma_retain_drm(struct task_vma *dst,
                                     const struct task_vma *src)
{
    struct task_file *mapping_file = NULL;
    if (!dst || !src) return -LINUX_EINVAL;
    if (src->drm_mapping_file) {
        mapping_file = task_file_get(src->drm_mapping_file);
        if (!mapping_file) return -LINUX_ENOMEM;
    }
    if (src->drm_bo) drm_bo_get(src->drm_bo);
    dst->drm_mapping_file = mapping_file;
    dst->drm_bo = src->drm_bo;
    return 0;
}

/** @brief Resolve a Linux descriptor number without acquiring a description.
 * @param task Owner under the execution lock. @param fd Linux fd, not array index.
 * @return Borrowed live slot or NULL. Includes stdio and the grown file table.
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
