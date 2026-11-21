#include "drm_internal.h"
#include "file_ops.h"
#ifdef CONFIG_DRM_VIRTIO_GPU
#include "virtio/internal.h"
#include <reliefnt/futex.h>
#endif

#include <drm/drm.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/mman.h>
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/time.h>
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
    void *object;
    void (*put)(void *);
    struct task_file **object_cache;
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

/* Owns prime and its backing reference on entry, including error paths. */
static int drm_prime_new_fd(struct task *task, struct drm_prime_file *prime,
                            uint32_t flags, uint64_t size)
{
    struct task_file *slot;
    int fd = task_allocate_fd(task, 0, &slot);
    if (fd < 0) {
        drm_bo_put(prime->bo);
        if (prime->put)
            prime->put(prime->object);
        kernel_free(prime);
        return fd;
    }
    uint64_t ino = __atomic_add_fetch(&drm_prime_next_ino, 1, __ATOMIC_RELAXED);
    if (!ino)
        ino = __atomic_add_fetch(&drm_prime_next_ino, 1, __ATOMIC_RELAXED);
    slot->kind = TASK_FILE_KIND_DRM_PRIME;
    slot->flags = (flags & DRM_PRIME_FD_RDWR) ? RELIEFOS_O_RDWR : RELIEFOS_O_RDONLY;
    slot->node.type = RELIEFOS_FS_TYPE_FILE;
    slot->node.size = size;
    slot->drm_prime_file = prime;
    slot->drm_prime_size = size;
    slot->drm_prime_ino = ino;
    slot->drm_prime_uid = task->fsuid;
    slot->drm_prime_gid = task->fsgid;
    slot->fd_flags = (flags & DRM_PRIME_FD_CLOEXEC) ? RELIEFOS_FD_CLOEXEC : 0;
    int ret = drm_character_attach_anonymous(task, slot);
    if (ret) {
        drm_prime_file_close(slot);
        task_discard_file_fd(task, fd);
        return ret;
    }
    return fd;
}

int drm_prime_export_object(struct task *task, struct drm_device *device, uint32_t flags,
                            uint64_t size, void *object, void (*put)(void *), struct task_file **cache)
{
    if (cache && *cache) {
        int fd;
        int ret = drm_prime_dup_cached(task, *cache, flags, &fd);
        put(object);
        return ret ? ret : fd;
    }
    struct drm_prime_file *prime = kernel_malloc(sizeof(*prime));
    if (!prime) {
        put(object);
        return -ENOMEM;
    }
    *prime = (struct drm_prime_file){.device = device, .object = object, .put = put};
    int fd = drm_prime_new_fd(task, prime, flags, size);
    if (fd < 0 || !cache)
        return fd;
    struct task_file *description = task_file_get(task_file_for_fd(task, fd));
    if (!description) {
        task_discard_file_fd(task, fd);
        return -ENOMEM;
    }
    *cache = description;
    prime->object_cache = cache;
    task_file_put(description);
    return fd;
}

void *drm_prime_lookup_object(struct task_file *source, const struct drm_device *device)
{
    struct drm_prime_file *prime = source ? source->drm_prime_file : NULL;
    return prime && prime->device == device ? prime->object : NULL;
}

/** @brief Borrow the BO of a matching PRIME OFD under execution ownership. */
struct drm_bo *drm_prime_lookup_bo(struct task_file *source, const struct drm_device *device)
{
    struct drm_prime_file *prime = source ? source->drm_prime_file : NULL;
    return prime && prime->device == device ? prime->bo : NULL;
}

/** @brief Export a file-authorized VirtGPU GEM object using a canonical DMA-BUF.
 * @param task Execution-owned caller. @param file GEM namespace. @param argument
 * Copied Linux export. @param created_fd_out Rollback output.
 * @return Zero or errno; provider/BO pins precede publication and unwind together.
 */
static int drm_prime_export_bo(struct task *task,struct drm_file *file,
                              struct drm_prime_handle *argument,int *created_fd_out)
{
    struct drm_bo *bo=drm_bo_lookup_handle_get(file,argument->handle);
    if (!bo || (!file->device->driver->prime_bo && !drm_bo_virtio(bo))) {
        drm_bo_put(bo);
        return -ENOENT;
    }
    struct task_file *cached=drm_bo_prime_description(bo);
    int fd=-1,rc;
    if(cached) {
        rc=drm_prime_dup_cached(task,cached,argument->flags,&fd);
        drm_bo_put(bo);if(rc)return rc;
    } else {
        struct drm_prime_file *prime=kernel_malloc(sizeof(*prime));
        if(!prime){drm_bo_put(bo);return -LINUX_ENOMEM;}
        *prime=(struct drm_prime_file){.bo=bo,.device=file->device};
        fd = drm_prime_new_fd(task, prime, argument->flags, drm_bo_size(bo));
        if (fd < 0)
            return fd;
        struct task_file *description=task_file_get(task_file_for_fd(task, fd));
        if(!description){task_discard_file_fd(task,fd);return -LINUX_ENOMEM;}
        rc=drm_bo_prime_bind(bo,description);if(!rc)prime->cache_bound=true;
        task_file_put(description);
        if(rc){task_discard_file_fd(task,fd);return rc;}
    }
    argument->fd=fd;if(created_fd_out)*created_fd_out=fd;return 0;
}

const struct drm_driver *drm_prime_mmap_driver(const struct task_file *file)
{
    const struct drm_prime_file *prime = file->drm_prime_file;
    return prime && prime->device ? prime->device->driver : NULL;
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
                    uint32_t *created_handle_out, uint32_t *created_object_out)
{
    if (created_fd_out) *created_fd_out = -1;
    if (!task || !file || !data) return -LINUX_EINVAL;
    struct drm_prime_handle *argument = data;
    const struct drm_driver *driver = file->device->driver;
    if (command == DRM_IOCTL_PRIME_HANDLE_TO_FD) {
        if (argument->flags & ~(DRM_PRIME_FD_CLOEXEC | DRM_PRIME_FD_RDWR))
            return -EINVAL;
        if (driver->prime_export) {
            int ret = driver->prime_export(task, file, argument);
            if (ret != -ENOENT) {
                if (!ret && created_fd_out)
                    *created_fd_out = argument->fd;
                return ret;
            }
        }
        return drm_prime_export_bo(task, file, argument, created_fd_out);
    }
    if (command != DRM_IOCTL_PRIME_FD_TO_HANDLE) return -LINUX_EOPNOTSUPP;
    if (argument->flags) return -LINUX_EINVAL;
    struct task_file *source = task_file_for_fd(task, argument->fd);
    if (!source) return -LINUX_EBADF;
    struct drm_prime_file *prime = source->drm_prime_file;
    if (!prime || prime->device != file->device)
        return -EINVAL;
    if (prime->object && driver->prime_import) {
        int ret = driver->prime_import(file, prime->object, &argument->handle);
        if (!ret && created_object_out)
            *created_object_out = argument->handle;
        return ret;
    }
    if (!prime->bo)
        return -EINVAL;
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
    if (prime->object_cache)
        *prime->object_cache = NULL;
    if (prime->put)
        prime->put(prime->object);
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
