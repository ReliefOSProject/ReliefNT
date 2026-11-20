#include "drm_internal.h"
#include "file_ops.h"
#ifdef CONFIG_DRM_VIRTIO_GPU
#include "virtio/internal.h"
#include <reliefnt/futex.h>
#include <reliefnt/time.h>
#endif

#include <drm/drm.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/mman.h>
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/usercopy.h>


#define DRM_DMA_BUF_IOCTL_SYNC 0x40086200u
#define DRM_DMA_BUF_SYNC_RW 0x3u
#define DRM_DMA_BUF_SYNC_END (1u << 2)
#define DRM_PRIME_FD_RDWR LINUX_O_RDWR
#define DRM_PRIME_FD_CLOEXEC LINUX_O_CLOEXEC

struct drm_prime_file {
    struct drm_device *device;
    struct drm_bo *bo;
    bool cache_bound;
};

struct drm_dma_buf_sync {
    uint64_t flags;
};

static uint64_t drm_prime_next_ino;

/**
 * @brief Allocate a new descriptor slot referring to an existing cached PRIME
 *        open-file description.
 * @param task Caller receiving the descriptor.
 * @param description Existing shared PRIME OFD retaining the buffer object.
 * @param flags DRM export flags; only descriptor-local CLOEXEC is applied here.
 * @param fd_out Required output receiving the new descriptor number.
 * @return Zero after the slot retains description, or a negative errno; no
 *         descriptor is published on failure.
 */
static int drm_prime_dup_cached(struct task *task,
                                struct task_file *description,
                                uint32_t flags, int *fd_out)
{
    struct task_file *slot = NULL;
    int fd = task_allocate_fd(task, 0, &slot);
    if (fd < 0) return fd;
    int ret = task_file_reference(slot, description);
    if (ret < 0) {
        task_discard_file_fd(task, fd);
        return ret;
    }
    slot->fd_flags = (flags & DRM_PRIME_FD_CLOEXEC)
                         ? RELIEFOS_FD_CLOEXEC : 0;
    *fd_out = fd;
    return 0;
}

/** @brief Export a file-authorized VirtGPU GEM object using a canonical DMA-BUF.
 * @param task Execution-owned caller. @param file GEM namespace. @param argument
 * Copied Linux export. @param created_fd_out Rollback output.
 * @return Zero or errno; provider/BO pins precede publication and unwind together.
 */
static int drm_prime_export_bo(struct task *task,struct drm_file *file,
                              struct drm_prime_handle *argument,int *created_fd_out)
{
    if(argument->flags&~(DRM_PRIME_FD_CLOEXEC|DRM_PRIME_FD_RDWR))return -LINUX_EINVAL;
    struct drm_bo *bo=drm_bo_lookup_handle_get(file,argument->handle);
    if(!bo || !drm_bo_virtio(bo)){drm_bo_put(bo);return -LINUX_ENOENT;}
    struct task_file *cached=drm_bo_prime_description(bo);
    int fd=-1,rc;
    if(cached) {
        rc=drm_prime_dup_cached(task,cached,argument->flags,&fd);
        drm_bo_put(bo);if(rc)return rc;
    } else {
        struct drm_prime_file *prime=kernel_malloc(sizeof(*prime));
        if(!prime){drm_bo_put(bo);return -LINUX_ENOMEM;}
        *prime=(struct drm_prime_file){.bo=bo,.device=file->device};
        struct task_file *slot=NULL;fd=task_allocate_fd(task,0,&slot);
        if(fd<0){kernel_free(prime);drm_bo_put(bo);return fd;}
        uint64_t ino=__atomic_add_fetch(&drm_prime_next_ino,1,__ATOMIC_RELAXED);
        if(!ino)ino=__atomic_add_fetch(&drm_prime_next_ino,1,__ATOMIC_RELAXED);
        slot->kind=TASK_FILE_KIND_DRM_PRIME;
        slot->flags=(argument->flags&DRM_PRIME_FD_RDWR) ? RELIEFOS_O_RDWR : RELIEFOS_O_RDONLY;
        slot->node.type=RELIEFOS_FS_TYPE_FILE;slot->node.size=drm_bo_size(bo);
        slot->drm_prime_file=prime;slot->drm_prime_size=drm_bo_size(bo);slot->drm_prime_ino=ino;
        slot->drm_prime_uid=task->fsuid;slot->drm_prime_gid=task->fsgid;
        slot->fd_flags=(argument->flags&DRM_PRIME_FD_CLOEXEC) ? RELIEFOS_FD_CLOEXEC : 0;
        rc=drm_character_attach_anonymous(task,slot);
        if(rc){drm_prime_file_close(slot);task_discard_file_fd(task,fd);return rc;}
        struct task_file *description=task_file_get(slot);
        if(!description){task_discard_file_fd(task,fd);return -LINUX_ENOMEM;}
        rc=drm_bo_prime_bind(bo,description);if(!rc)prime->cache_bound=true;
        task_file_put(description);
        if(rc){task_discard_file_fd(task,fd);return rc;}
    }
    argument->fd=fd;if(created_fd_out)*created_fd_out=fd;return 0;
}

/** @brief Resolve retained GEM pages for an admitted DMA-BUF mapping.
 * @param file Provider-pinned PRIME OFD. @param offset Byte offset. @param length
 * Page-rounded range. @param out Owned BO output. @return Zero or errno.
 */
int drm_prime_mmap_bo(struct task_file *file,uint64_t offset,uint64_t length,struct drm_bo **out)
{
    if(!out)return -LINUX_EINVAL;*out=NULL;
    struct drm_prime_file *prime=file ? file->drm_prime_file : NULL;
    if(!prime || !prime->bo || (offset&4095) || !length || offset>drm_bo_size(prime->bo) ||
       length>drm_bo_size(prime->bo)-offset)return -LINUX_EINVAL;
    drm_bo_get(prime->bo);*out=prime->bo;return 0;
}

/**
 * @brief Handle the two supported core PRIME ioctls on an authorized DRM OFD.
 * @param task Caller for fd allocation and user-independent input processing.
 * @param file Primary or render DRM OFD whose per-file handle table is used.
 * @param command PRIME_HANDLE_TO_FD or PRIME_FD_TO_HANDLE.
 * @param data Copied kernel ioctl structure with fields in/out as specified.
 * @param created_fd_out Optional syscall rollback output for exported fd.
 * @param created_handle_out Optional output for newly granted GEM handles.
 * @return Zero on supported success, negative errno for invalid/unsupported
 *         requests; descriptor and handle outputs remain failure-atomic.
 */
int drm_prime_ioctl(struct task *task, struct drm_file *file,
                    uint32_t command, void *data, int *created_fd_out,
                    uint32_t *created_handle_out)
{
    if (created_fd_out) *created_fd_out = -1;
    if (!task || !file || !data) return -LINUX_EINVAL;
    struct drm_prime_handle *argument = data;
    if (command == DRM_IOCTL_PRIME_HANDLE_TO_FD)
        return drm_prime_export_bo(task, file, argument, created_fd_out);
    if (command != DRM_IOCTL_PRIME_FD_TO_HANDLE) return -LINUX_EOPNOTSUPP;
    if (argument->flags) return -LINUX_EINVAL;
    struct task_file *source = task_file_for_fd(task, argument->fd);
    if (!source) return -LINUX_EBADF;
    struct drm_prime_file *prime = source->drm_prime_file;
    if (!prime || prime->device != file->device || !prime->bo) return -LINUX_EINVAL;
    bool created = false;
    int rc = drm_bo_handle_import(file, prime->bo, &argument->handle, &created);
    if (!rc && created && created_handle_out) *created_handle_out = argument->handle;
    return rc;
}

/**
 * @brief Drop cached-export association and the PRIME OFD's retained BO
 *        when its final shared descriptor reference closes.
 * @param file Final PRIME open-file description being destroyed; null or a
 *             non-PRIME description is ignored.
 * @note This releases resource ownership only; it does not release the
 *       exporter card OFD or its master identity.
 */
void drm_prime_file_close(struct task_file *file)
{
    if (!file || !file->drm_prime_file) return;
    struct drm_prime_file *prime = file->drm_prime_file;
    file->drm_prime_file = NULL;
    if(prime->bo) {
        if(prime->cache_bound)drm_bo_prime_unbind(prime->bo,file);
        drm_bo_put(prime->bo);
    }
    kernel_free(prime);
}

/**
 * @brief Report immediate ready bits for the opaque DMA-BUF's empty reservation.
 * @param file PRIME buffer description queried by poll.
 * @param events Requested poll events; unsupported bits are ignored.
 * @return Requested subset of POLLIN/POLLRDNORM/POLLOUT/POLLWRNORM, or zero
 *         when file is not a PRIME description.
 */
uint32_t drm_prime_file_poll(const struct task_file *file, uint32_t events)
{
    if (!file || !file->drm_prime_file) return 0;
    const struct drm_prime_file *prime=file->drm_prime_file;
    if(prime->bo && drm_bo_gpu_busy(prime->bo))return 0;
    const uint32_t ready = POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM;
    return events & ready;
}

/**
 * @brief Implement the opaque TTM object's zero-offset SET and END seeks.
 * @param file PRIME buffer description holding the exported page-aligned size.
 * @param offset Requested offset; nonzero offsets are unsupported.
 * @param whence ReliefNT SEEK_SET or SEEK_END selector.
 * @return Zero for SEEK_SET, size for SEEK_END, or a negative errno.
 */
int64_t drm_prime_file_llseek(struct task_file *file, int64_t offset,
                              uint32_t whence)
{
    if (!file || !file->drm_prime_file) return -LINUX_EBADF;
    if (offset != 0) return -LINUX_EINVAL;
    if (whence == RELIEFOS_SEEK_SET) return 0;
    if (whence == RELIEFOS_SEEK_END) return (int64_t)file->drm_prime_size;
    return -LINUX_EINVAL;
}

/**
 * @brief Apply OFD mode checks before reporting unsupported opaque-object I/O.
 * @param file PRIME buffer description to access.
 * @param writing True for write, false for read.
 * @return Negative EBADF when OFD access mode denies the operation; otherwise
 *         negative EINVAL because the opaque surface has no byte stream.
 */
int64_t drm_prime_file_rw(const struct task_file *file, bool writing)
{
    if (!file || !file->drm_prime_file) return -LINUX_EBADF;
    if (writing && !file_can_write(file)) return -LINUX_EBADF;
    if (!writing && !file_can_read(file)) return -LINUX_EBADF;
    return -LINUX_EINVAL;
}

/**
 * @brief Validate Linux DMA_BUF_IOCTL_SYNC flags on the PRIME buffer OFD.
 * @param task Caller used to safely copy the sync structure from argument.
 * @param file PRIME buffer description receiving the ioctl.
 * @param command 32-bit request; only DMA_BUF_IOCTL_SYNC is supported.
 * @param argument 64-bit caller address of the sync flags structure.
 * @return Zero for valid READ/WRITE START or END operations, or a negative
 *         Linux errno; successful cache synchronization is not GPU completion.
 */
int64_t drm_prime_file_ioctl(struct task *task, struct task_file *file,
                             uint32_t command, uint64_t argument)
{
    if (!task || !file || !file->drm_prime_file) return -LINUX_EBADF;
    if (command != DRM_DMA_BUF_IOCTL_SYNC) return -LINUX_ENOTTY;
    struct drm_dma_buf_sync sync;
#ifdef CONFIG_SWAP
    int prep=drm_user_import(task,&sync,argument,sizeof(sync));
    if(prep)return prep;
#else
    if (user_copy_from_task(task, &sync, argument, sizeof(sync)) < 0)
        return -LINUX_EFAULT;
#endif
    if ((sync.flags & ~((uint64_t)DRM_DMA_BUF_SYNC_RW |
                        DRM_DMA_BUF_SYNC_END)) ||
        !(sync.flags & DRM_DMA_BUF_SYNC_RW))
        return -LINUX_EINVAL;
#ifdef CONFIG_DRM_VIRTIO_GPU
    struct drm_prime_file *prime=file->drm_prime_file;
    if(prime->bo && !(sync.flags&DRM_DMA_BUF_SYNC_END)) {
        vg_process_retirements(128);
        if(vg.fault)return -LINUX_EIO;
        struct vg_resource *resource=drm_bo_virtio(prime->bo);
        if(resource && resource->error)return resource->error;
        if(drm_bo_gpu_busy(prime->bo)) {
            sched_sleep_current_until(time_ticks()+1);
            return KERNEL_SYSCALL_BLOCKED;
        }
    }
#endif
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}

/**
 * @brief Enforce canonical OFD access precedence for a PRIME buffer mapping.
 * @param file PRIME description being mapped.
 * @param flags Linux mmap flags, including MAP_SHARED when applicable.
 * @param protection Requested PROT_* access bits.
 * @return Negative EACCES when read/write access is forbidden by the OFD mode;
 *         otherwise zero for a GEM-backed export or negative EINVAL.
 */
int drm_prime_file_mmap(struct task_file *file, uint64_t flags,
                        uint64_t protection)
{
    if (!file || !file->drm_prime_file) return -LINUX_EBADF;
    if (!file_can_read(file)) return -LINUX_EACCES;
    if ((flags & LINUX_MAP_SHARED) && (protection & LINUX_PROT_WRITE) &&
        !file_can_write(file))
        return -LINUX_EACCES;
    return file->drm_prime_file->bo ? 0 : -LINUX_EINVAL;
}
