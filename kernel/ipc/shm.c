/*
 * ReliefOS anonymous shared-memory device (/dev/shm0).
 *
 * This is the Linux memfd replacement required by the Unix-IPC windowing
 * design.  ftruncate() sizes the segment, mmap() maps it through the same
 * device-page path as /dev/fb0, fork/SCM_RIGHTS keep sharing the same physical
 * buffer, and the last descriptor release frees the pages.
 */
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>
#include <reliefnt/object.h>
#include <reliefnt/sched.h>
#include <reliefnt/storage.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <linux/fcntl.h>
#include <linux/mman.h>

#define TASK_SHM_MAX 32u
#define TASK_SHM_PAGE 4096ULL
#define TASK_SHM_MAX_BYTES (1920ULL * 1080ULL * 4ULL)

struct task_shm {
    uint32_t used;
    uint32_t refs;
    uint32_t pages;
    uint32_t seals;
    uint64_t bytes;
    uint64_t physical;
};

static struct task_shm shm_segments[TASK_SHM_MAX];
static uint32_t shm_next_slot;

static uint64_t shm_page_count(uint64_t bytes)
{
    return (uint32_t)((bytes + TASK_SHM_PAGE - 1ULL) / TASK_SHM_PAGE);
}

void task_shm_retain(struct task_file *file)
{
    struct task_shm *segment;
    if (!file || !(file->flags & TASK_FILE_FLAG_DEV_SHM)) return;
    segment = (struct task_shm *)kernel_object_lookup(kernel_objects(),
                                                      (uint32_t)file->aux,
                                                      KERNEL_OBJECT_DEVICE);
    if (segment) ++segment->refs;
}

void task_shm_release(struct task_file *file)
{
    struct task_shm *segment;
    void *removed = NULL;
    if (!file || !(file->flags & TASK_FILE_FLAG_DEV_SHM)) return;
    segment = (struct task_shm *)kernel_object_lookup(kernel_objects(),
                                                      (uint32_t)file->aux,
                                                      KERNEL_OBJECT_DEVICE);
    if (!segment || !segment->refs) return;
    --segment->refs;
    if (segment->refs) return;
    if (kernel_object_remove(kernel_objects(), (uint32_t)file->aux,
                             KERNEL_OBJECT_DEVICE, &removed) == 0 && removed) {
        struct task_shm *dead = (struct task_shm *)removed;
        for (uint32_t slot = 0; slot < TASK_SHM_MAX; ++slot) {
            if (shm_segments[slot].used && shm_segments[slot].physical == dead->physical) {
                shm_segments[slot].used = 0;
                break;
            }
        }
        if (dead->physical && dead->pages) {
            mm_free_pages(dead->physical, dead->pages);
        }
        kernel_free(dead);
    }
}

int task_shm_attach(struct task_file *file, uint32_t create_flags)
{
    struct task_shm *segment;
    uint32_t handle;
    uint32_t slot;
    if (!file) return -RELIEFOS_EINVAL;
    for (slot = 0; slot < TASK_SHM_MAX; ++slot) {
        if (!shm_segments[slot].used) break;
    }
    if (slot == TASK_SHM_MAX) return -RELIEFOS_EMFILE;
    segment = (struct task_shm *)kernel_malloc(sizeof(*segment));
    if (!segment) return -RELIEFOS_ENOMEM;
    /* Linux shmem inodes start with F_SEAL_SEAL; memfd_create(2) with
     * MFD_ALLOW_SEALING lifts the lock so F_ADD_SEALS can seal later. */
    *segment = (struct task_shm){.used = 1, .refs = 1, .pages = 1,
        .bytes = TASK_SHM_PAGE,
        .seals = (create_flags & LINUX_MFD_ALLOW_SEALING) ? 0 : LINUX_F_SEAL_SEAL};
    segment->physical = mm_alloc_pages(1);
    if (!segment->physical) {
        kernel_free(segment);
        return -RELIEFOS_ENOMEM;
    }
    handle = kernel_object_insert(kernel_objects(), segment, KERNEL_OBJECT_DEVICE);
    if (!handle) {
        mm_free_pages(segment->physical, 1);
        kernel_free(segment);
        return -RELIEFOS_EMFILE;
    }
    shm_segments[slot] = *segment;
    file->aux = handle;
    file->flags |= TASK_FILE_FLAG_DEV_SHM;
    file->node.size = TASK_SHM_PAGE;
    return 0;
}

int task_shm_truncate(struct task_file *file, uint64_t size)
{
    struct task_shm *segment;
    uint32_t pages;
    uint64_t physical;
    if (!file || !(file->flags & TASK_FILE_FLAG_DEV_SHM)) return -RELIEFOS_EBADF;
    if (size > TASK_SHM_MAX_BYTES) return -RELIEFOS_EINVAL;
    segment = (struct task_shm *)kernel_object_lookup(kernel_objects(),
                                                      (uint32_t)file->aux,
                                                      KERNEL_OBJECT_DEVICE);
    if (!segment) return -RELIEFOS_EBADF;
    /* Linux shmem_setattr: F_SEAL_SHRINK blocks size reduction and
     * F_SEAL_GROW blocks size increase, both with EPERM. */
    if (size < segment->bytes && (segment->seals & LINUX_F_SEAL_SHRINK))
        return -RELIEFOS_EPERM;
    if (size > segment->bytes && (segment->seals & LINUX_F_SEAL_GROW))
        return -RELIEFOS_EPERM;
    pages = (uint32_t)shm_page_count(size);
    if (pages == segment->pages) {
        segment->bytes = size;
        file->node.size = size;
        return 0;
    }
    physical = pages ? mm_alloc_pages(pages) : 0;
    if (pages && !physical) return -RELIEFOS_ENOMEM;
    if (segment->physical && segment->pages) {
        uint32_t copy_pages = segment->pages < pages ? segment->pages : pages;
        for (uint32_t i = 0; i < copy_pages * (uint32_t)(TASK_SHM_PAGE / sizeof(uint32_t)); ++i) {
            ((uint32_t *)(uintptr_t)physical)[i] =
                ((const uint32_t *)(uintptr_t)segment->physical)[i];
        }
        mm_free_pages(segment->physical, segment->pages);
    }
    segment->physical = physical;
    segment->pages = pages;
    segment->bytes = size;
    file->node.size = size;
    return 0;
}

int task_shm_read(struct task_file *file, void *buffer, uint32_t length)
{
    struct task_shm *segment;
    uint64_t capacity;
    if (!file || !(file->flags & TASK_FILE_FLAG_DEV_SHM)) return -RELIEFOS_EBADF;
    if (length && !buffer) return -RELIEFOS_EFAULT;
    segment = (struct task_shm *)kernel_object_lookup(kernel_objects(),
                                                      (uint32_t)file->aux,
                                                      KERNEL_OBJECT_DEVICE);
    if (!segment) return -RELIEFOS_EBADF;
    capacity = segment->bytes;
    if (file->offset >= capacity) return 0;
    if ((uint64_t)length > capacity - file->offset)
        length = (uint32_t)(capacity - file->offset);
    if (length) __builtin_memcpy(buffer, (const void *)(uintptr_t)(segment->physical + file->offset), length);
    file->offset += length;
    return (int)length;
}

int task_shm_write(struct task_file *file, const void *buffer, uint32_t length)
{
    struct task_shm *segment;
    uint64_t end;
    if (!file || !(file->flags & TASK_FILE_FLAG_DEV_SHM)) return -RELIEFOS_EBADF;
    if (length && !buffer) return -RELIEFOS_EFAULT;
    segment = (struct task_shm *)kernel_object_lookup(kernel_objects(),
                                                      (uint32_t)file->aux,
                                                      KERNEL_OBJECT_DEVICE);
    if (!segment) return -RELIEFOS_EBADF;
    if (!length) return 0;
    /* Linux shmem_write_begin: F_SEAL_WRITE/F_SEAL_FUTURE_WRITE block every
     * write; F_SEAL_GROW blocks writes past the current end of file. */
    if (segment->seals & (LINUX_F_SEAL_WRITE | LINUX_F_SEAL_FUTURE_WRITE))
        return -RELIEFOS_EPERM;
    if (file->offset > TASK_SHM_MAX_BYTES ||
        (uint64_t)length > TASK_SHM_MAX_BYTES - file->offset) return -RELIEFOS_EFBIG;
    end = file->offset + length;
    if (end > segment->bytes && (segment->seals & LINUX_F_SEAL_GROW))
        return -RELIEFOS_EPERM;
    /* POSIX regular-file semantics: a write past the current size extends the
     * file (Linux memfd/shmem writes do the same) instead of failing at the
     * old end of file.  Truncate-backed growth keeps the copy/zero behavior in
     * one place. */
    if (end > segment->bytes && task_shm_truncate(file, end) < 0) return -RELIEFOS_ENOSPC;
    __builtin_memcpy((void *)(uintptr_t)(segment->physical + file->offset), buffer, length);
    file->offset += length;
    return (int)length;
}

int task_shm_map(const struct task_file *file, uint64_t offset, uint64_t length,
                 uint32_t prot, uint64_t *physical)
{
    struct task_shm *segment;
    if (!file || !physical || !(file->flags & TASK_FILE_FLAG_DEV_SHM)) {
        return -RELIEFOS_EBADF;
    }
    segment = (struct task_shm *)kernel_object_lookup(kernel_objects(),
                                                      (uint32_t)file->aux,
                                                      KERNEL_OBJECT_DEVICE);
    if (!segment || !segment->physical) return -RELIEFOS_EBADF;
    /* Linux seal_check_write: F_SEAL_WRITE/F_SEAL_FUTURE_WRITE reject new
     * writable shared mappings with EPERM.  Shm device mappings are direct
     * physical aliases here (no copy-on-write even for MAP_PRIVATE), so any
     * writable mapping is a write path and follows the same rule. */
    if ((prot & LINUX_PROT_WRITE) &&
        (segment->seals & (LINUX_F_SEAL_WRITE | LINUX_F_SEAL_FUTURE_WRITE)))
        return -RELIEFOS_EPERM;
    /* mmap lengths are page-rounded up by the mmap entry path, so the bound
     * must be the page-backed allocation, not the exact logical size. */
    {
        uint64_t capacity = (uint64_t)segment->pages * TASK_SHM_PAGE;
        if (offset > capacity || length > capacity - offset) {
            return -RELIEFOS_EINVAL;
        }
    }
    *physical = segment->physical + offset;
    return 0;
}

/* Linux F_ADD_SEALS fails with EBUSY while writable mappings pin the file
 * (mm/memfd.c: memfd_wait_for_pins reports the same condition).  Shm device
 * mappings alias the segment physically, so scan every address space for a
 * writable mapping that overlaps the segment's page range. */
static int task_shm_writable_mapped(const struct task_shm *segment)
{
    struct task_snapshot_info snapshots[SCHED_TASK_MAX];
    uint32_t count = sched_snapshot(snapshots, SCHED_TASK_MAX, 0);
    uint64_t seg_start = segment->physical;
    uint64_t seg_end = segment->physical + (uint64_t)segment->pages * TASK_SHM_PAGE;
    for (uint32_t s = 0; s < count; ++s) {
        struct task *other = sched_find(snapshots[s].pid);
        struct task_address_space_state *mm;
        if (!other) continue;
        mm = sched_task_mm(other);
        if (!mm) continue;
        for (uint32_t i = 0; i < SCHED_TASK_VMA_MAX + mm->vma_extra_count; ++i) {
            const struct task_vma *vma = sched_task_vma_at(other, i);
            uint64_t vma_end;
            if (!vma || !vma->used || !(vma->prot & LINUX_PROT_WRITE)) continue;
            if (!vma->device_physical) continue;
            vma_end = vma->device_physical + (vma->end - vma->start);
            if (vma->device_physical < seg_end && vma_end > seg_start) return 1;
        }
    }
    return 0;
}

int64_t task_shm_get_seals(const struct task_file *file)
{
    struct task_shm *segment;
    /* Linux memfd_get_seals: only seal-capable shmem files answer; any other
     * file reports EINVAL. */
    if (!file || !(file->flags & TASK_FILE_FLAG_DEV_SHM)) return -RELIEFOS_EINVAL;
    segment = (struct task_shm *)kernel_object_lookup(kernel_objects(),
                                                      (uint32_t)file->aux,
                                                      KERNEL_OBJECT_DEVICE);
    if (!segment) return -RELIEFOS_EBADF;
    return (int64_t)segment->seals;
}

int task_shm_add_seals(struct task_file *file, uint32_t seals)
{
    struct task_shm *segment;
    /* Linux memfd_add_seals validates the descriptor mode and the seal bits
     * before the sealing-support check: a descriptor without write access is
     * EPERM even when the file could never carry seals. */
    if (!file || !file_can_write(file)) return -RELIEFOS_EPERM;
    if (seals & ~LINUX_F_ALL_SEALS) return -RELIEFOS_EINVAL;
    if (!(file->flags & TASK_FILE_FLAG_DEV_SHM)) return -RELIEFOS_EINVAL;
    segment = (struct task_shm *)kernel_object_lookup(kernel_objects(),
                                                      (uint32_t)file->aux,
                                                      KERNEL_OBJECT_DEVICE);
    if (!segment) return -RELIEFOS_EBADF;
    /* Seals are monotonic; F_SEAL_SEAL seals the sealing itself. */
    if (segment->seals & LINUX_F_SEAL_SEAL) return -RELIEFOS_EPERM;
    /* Linux rejects the first F_SEAL_WRITE with EBUSY while writable mappings
     * exist; callers retry, which is why the check must see live mappings. */
    if ((seals & LINUX_F_SEAL_WRITE) && !(segment->seals & LINUX_F_SEAL_WRITE) &&
        task_shm_writable_mapped(segment))
        return -RELIEFOS_EBUSY;
    segment->seals |= seals;
    return 0;
}
