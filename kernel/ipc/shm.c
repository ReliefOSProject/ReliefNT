/* Linux anonymous shmem files. tmpfs owns sparse pages and VMA inode pins. */
#include <reliefnt/heap.h>
#include <reliefnt/object.h>
#include <reliefnt/sched.h>
#include <reliefnt/storage.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <linux/fcntl.h>
#include <linux/mman.h>

struct task_shm {
    uint32_t refs;
    uint32_t seals;
    struct storage_node node;
};

/** @brief Resolve the seal state belonging to a pinned anonymous-file description.
 * @param file Borrowed description under execution ownership, nullable.
 * @return Borrowed seal state or NULL for a non-shmem/stale description.
 */
static struct task_shm *task_shm_for_file(const struct task_file *file)
{
    if (!file || !(file->flags & TASK_FILE_FLAG_DEV_SHM)) return NULL;
    return kernel_object_lookup(kernel_objects(), (uint32_t)file->aux, KERNEL_OBJECT_DEVICE);
}

/** @brief Retain seal-state ownership when an independent description is created.
 * @param file Live shmem description under execution ownership, nullable.
 */
void task_shm_retain(struct task_file *file)
{
    struct task_shm *segment = task_shm_for_file(file);
    if (segment) ++segment->refs;
}

/** @brief Release seal state; backing inode/page lifetime is owned by OFDs and VMAs.
 * @param file Retiring shmem description under execution ownership, nullable.
 */
void task_shm_release(struct task_file *file)
{
    struct task_shm *segment = task_shm_for_file(file);
    if (!segment || !segment->refs || --segment->refs) return;
    void *removed = NULL;
    if (!kernel_object_remove(kernel_objects(), (uint32_t)file->aux, KERNEL_OBJECT_DEVICE, &removed))
        kernel_free(removed);
}

/** @brief Attach a zero-length anonymous tmpfs inode and independent seal state.
 * @param file Fresh description; receives the owned inode pin only on success.
 * @param create_flags Linux MFD flags, validated by the syscall entry.
 * @return Zero or negative EINVAL, ENOMEM, ENFILE or backing-store error;
 * no partially attached object is left on failure. Requires execution ownership.
 */
int task_shm_attach(struct task_file *file, uint32_t create_flags)
{
    if (!file || (file->flags & TASK_FILE_FLAG_DEV_SHM)) return -RELIEFOS_EINVAL;
    struct task_shm *segment = kernel_malloc(sizeof(*segment));
    if (!segment) return -RELIEFOS_ENOMEM;
    *segment = (struct task_shm){.refs = 1,
        .seals = create_flags & LINUX_MFD_ALLOW_SEALING ? 0 : LINUX_F_SEAL_SEAL};
    const struct task *task = sched_current_task();
    struct storage_inode_ref *inode = NULL;
    int ret = storage_shmem_create(task ? task->euid : 0, task ? task->egid : 0, &segment->node, &inode);
    if (ret < 0) { kernel_free(segment); return ret; }
    uint32_t handle = kernel_object_insert(kernel_objects(), segment, KERNEL_OBJECT_DEVICE);
    if (!handle) {
        (void)storage_inode_put(inode);
        kernel_free(segment);
        return -RELIEFOS_ENFILE;
    }
    file->aux = handle;
    file->flags = (file->flags & ~TASK_FILE_FLAG_DEV_NODE) | TASK_FILE_FLAG_DEV_SHM;
    file->node = segment->node;
    file->inode = inode;
    return 0;
}

/** @brief Change logical size without requiring a physically contiguous allocation.
 * @param file Writable shmem description under execution ownership.
 * @param size New nonnegative signed-off_t size in bytes.
 * @return Zero or negative EINVAL/EBADF/EPERM/backing error. Shrink revokes
 * affected mappings before releasing pages; growth is sparse and zero-filled.
 */
int task_shm_truncate(struct task_file *file, uint64_t size)
{
    struct task_shm *segment = task_shm_for_file(file);
    if (!segment) return -RELIEFOS_EBADF;
    if (!file_can_write(file) || size > INT64_MAX) return -RELIEFOS_EINVAL;
    if ((size < segment->node.size && (segment->seals & LINUX_F_SEAL_SHRINK)) ||
        (size > segment->node.size && (segment->seals & LINUX_F_SEAL_GROW))) return -RELIEFOS_EPERM;
    int ret = storage_truncate_held_node(&segment->node, size);
    if (!ret) file->node.size = size;
    return ret;
}

/** @brief Read file bytes or zeros from holes, advancing the shared OFD offset.
 * @param file Pinned shmem description under execution ownership.
 * @param buffer Validated writable kernel-accessible destination for length bytes.
 * @param length Maximum byte count.
 * @return Bytes read, zero at EOF, or negative EBADF/EFAULT/backing error.
 */
int task_shm_read(struct task_file *file, void *buffer, uint32_t length)
{
    struct task_shm *segment = task_shm_for_file(file);
    if (!segment) return -RELIEFOS_EBADF;
    if (length && !buffer) return -RELIEFOS_EFAULT;
    if (!length) return 0;
    uint32_t got = 0;
    int ret = storage_read_node(&segment->node, file->offset, buffer, length, &got);
    file->offset += got;
    return got ? (int)got : ret;
}

/** @brief Write sparse file pages, preserving partial progress on allocation failure.
 * @param file Pinned writable shmem OFD under execution ownership.
 * @param buffer Validated source for length bytes.
 * @param length Requested byte count.
 * @return Bytes written or negative EBADF/EFAULT/EFBIG/EPERM/backing error.
 * Gaps are zero-filled by tmpfs and no offset or length wraps signed off_t.
 */
int task_shm_write(struct task_file *file, const void *buffer, uint32_t length)
{
    struct task_shm *segment = task_shm_for_file(file);
    if (!segment || !file_can_write(file)) return -RELIEFOS_EBADF;
    if (length && !buffer) return -RELIEFOS_EFAULT;
    if (!length) return 0;
    if (segment->seals & (LINUX_F_SEAL_WRITE | LINUX_F_SEAL_FUTURE_WRITE)) return -RELIEFOS_EPERM;
    if (file->offset > INT64_MAX || length > (uint64_t)INT64_MAX - file->offset) return -RELIEFOS_EFBIG;
    if (file->offset + length > segment->node.size && (segment->seals & LINUX_F_SEAL_GROW))
        return -RELIEFOS_EPERM;
    uint32_t written = 0;
    int ret = storage_write_held_node(&segment->node, file->offset, buffer, length, &written);
    file->offset += written;
    if (written) {
        if (file->offset > segment->node.size) segment->node.size = file->offset;
        file->node.size = segment->node.size;
    }
    return written ? (int)written : ret;
}

/** @brief Enforce shared-map seals without restricting private COW mappings.
 * @param file Pinned anonymous-file description under execution ownership.
 * @param flags Validated Linux MAP flags.
 * @param prot Requested protection bits.
 * @param max_prot In/out maximum protection; cleared WRITE prevents later mprotect bypass.
 * @return Zero or negative EBADF/EPERM. Backing allocation is deferred to faults.
 */
int task_shm_mmap_check(const struct task_file *file, uint32_t flags, uint32_t prot, uint32_t *max_prot)
{
    struct task_shm *segment = task_shm_for_file(file);
    if (!segment) return -RELIEFOS_EBADF;
    if ((flags & LINUX_MAP_SHARED) &&
        (segment->seals & (LINUX_F_SEAL_WRITE | LINUX_F_SEAL_FUTURE_WRITE))) {
        if (prot & LINUX_PROT_WRITE) return -RELIEFOS_EPERM;
        if (max_prot) *max_prot &= ~LINUX_PROT_WRITE;
    }
    return 0;
}

/** @brief Check every task's potentially writable shared VMA for this inode.
 * @param segment Borrowed live seal state under execution ownership.
 * @return One if any shared mapping can write, zero otherwise. Private COW
 * mappings do not prevent sealing; no allocation or bounded snapshot is used.
 */
static int task_shm_writable_mapped(const struct task_shm *segment)
{
    struct task *other;
    for (uint32_t s = 0; (other = sched_task_at(s)); ++s) {
        for (uint32_t i = 0; i < sched_task_vma_capacity(other); ++i) {
            const struct task_vma *vma = sched_task_vma_at(other, i);
            if (!vma || !vma->used || !(vma->flags & TASK_VMA_FLAG_SHARED) ||
                !(vma->max_prot & LINUX_PROT_WRITE) || !(vma->flags & TASK_VMA_FLAG_FILE)) continue;
            if (vma->file_node.volume_id == segment->node.volume_id &&
                vma->file_node.first_cluster == segment->node.first_cluster &&
                (vma->file_node.flags & STORAGE_NODE_FLAG_TMPFS)) return 1;
        }
    }
    return 0;
}

/** @brief Read the inode-wide monotonic seal set.
 * @param file Pinned description under execution ownership, nullable.
 * @return Seal mask or negative EINVAL for non-shmem, EBADF for a stale handle.
 */
int64_t task_shm_get_seals(const struct task_file *file)
{
    if (!file || !(file->flags & TASK_FILE_FLAG_DEV_SHM)) return -RELIEFOS_EINVAL;
    struct task_shm *segment = task_shm_for_file(file);
    return segment ? (int64_t)segment->seals : -RELIEFOS_EBADF;
}

/** @brief Add Linux seals atomically after access, flag and live-map checks.
 * @param file Writable shmem description under execution ownership.
 * @param seals Requested known seal bits; existing seals are never cleared.
 * @return Zero or negative EPERM/EINVAL/EBADF/EBUSY; failure changes no seals.
 */
int task_shm_add_seals(struct task_file *file, uint32_t seals)
{
    if (!file || !file_can_write(file)) return -RELIEFOS_EPERM;
    if (seals & ~LINUX_F_ALL_SEALS) return -RELIEFOS_EINVAL;
    if (!(file->flags & TASK_FILE_FLAG_DEV_SHM)) return -RELIEFOS_EINVAL;
    struct task_shm *segment = task_shm_for_file(file);
    if (!segment) return -RELIEFOS_EBADF;
    if (segment->seals & LINUX_F_SEAL_SEAL) return -RELIEFOS_EPERM;
    if ((seals & LINUX_F_SEAL_WRITE) && !(segment->seals & LINUX_F_SEAL_WRITE) &&
        task_shm_writable_mapped(segment)) return -RELIEFOS_EBUSY;
    segment->seals |= seals;
    return 0;
}
