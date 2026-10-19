#include "drm_internal.h"

#include <drm/drm.h>
#include <drm/vmwgfx_drm.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/mman.h>
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/usercopy.h>

#include "../svga/device.h"
#include "../svga/svga_protocol.h"

#define DRM_PRIME_PAGE_SIZE 4096ULL
#define DRM_DMA_BUF_IOCTL_SYNC 0x40086200u
#define DRM_DMA_BUF_SYNC_RW 0x3u
#define DRM_DMA_BUF_SYNC_END (1u << 2)
#define DRM_PRIME_FD_RDWR LINUX_O_RDWR
#define DRM_PRIME_FD_CLOEXEC LINUX_O_CLOEXEC

struct drm_prime_file {
    struct drm_vmwgfx_surface *surface;
    struct drm_device *device;
    bool cache_bound;
};

struct drm_dma_buf_sync {
    uint64_t flags;
};

static uint64_t drm_prime_next_ino;

/**
 * @brief Compute the page-aligned opaque size of every face and mip level.
 * @param metadata Surface format, dimensions, cube flag and per-face mip count.
 * @param size_out Required writable output receiving the aligned byte size.
 * @return Zero on success, negative EINVAL for incomplete metadata, or
 *         negative EOVERFLOW when image-size arithmetic cannot be represented.
 */
static int drm_prime_surface_size(
    const struct drm_vmwgfx_surface_metadata *metadata, uint64_t *size_out)
{
    if (!metadata || !size_out) return -LINUX_EINVAL;
    uint32_t block_width, block_height, bytes_per_block, capability;
    int status = svga_format(metadata->desc.format, &block_width,
                             &block_height, &bytes_per_block, &capability);
    if (status || !block_width || !block_height || !bytes_per_block)
        return -LINUX_EINVAL;

    uint32_t face_count = (metadata->desc.flags & SVGA3D_SURFACE_CUBEMAP)
                              ? DRM_VMW_MAX_SURFACE_FACES : 1u;
    uint64_t total = 0;
    for (uint32_t face = 0; face < face_count; ++face) {
        uint32_t levels = metadata->mip_levels[face];
        uint64_t width = metadata->desc.width;
        uint64_t height = metadata->desc.height;
        uint64_t depth = metadata->desc.depth ? metadata->desc.depth : 1u;
        if (!levels) return -LINUX_EINVAL;
        for (uint32_t mip = 0; mip < levels; ++mip) {
            uint64_t blocks_wide = (width + block_width - 1u) / block_width;
            uint64_t blocks_high = (height + block_height - 1u) / block_height;
            if (blocks_wide && blocks_high > UINT64_MAX / blocks_wide)
                return -LINUX_EOVERFLOW;
            uint64_t image = blocks_wide * blocks_high;
            if (image && depth > UINT64_MAX / image)
                return -LINUX_EOVERFLOW;
            image *= depth;
            if (image && bytes_per_block > UINT64_MAX / image)
                return -LINUX_EOVERFLOW;
            image *= bytes_per_block;
            if (total > UINT64_MAX - image) return -LINUX_EOVERFLOW;
            total += image;
            if (width > 1u) width >>= 1;
            if (height > 1u) height >>= 1;
            if (depth > 1u) depth >>= 1;
        }
    }
    if (!total || total > UINT64_MAX - (DRM_PRIME_PAGE_SIZE - 1u))
        return -LINUX_EINVAL;
    *size_out = (total + DRM_PRIME_PAGE_SIZE - 1u) &
                ~(DRM_PRIME_PAGE_SIZE - 1u);
    return 0;
}

/**
 * @brief Resolve a descriptor only when it names this device's shareable
 *        surface PRIME OFD, and return one retained typed surface reference.
 * @param task Task whose fd table supplies the descriptor; required.
 * @param fd Descriptor number in task; an absent/negative descriptor returns
 *           EBADF, while an existing non-PRIME descriptor returns EINVAL.
 * @param device Exact target DRM device object; import provenance is pointer
 *               identity, not a matching surface name or PCI number.
 * @param surface_out Required output set to null on failure and to one owned
 *                    surface reference on success.
 * @return Zero on success, negative EBADF for an absent descriptor, or
 *         negative EINVAL for an existing wrong-object, foreign-device,
 *         stale, or non-shareable descriptor or an unavailable surface ref.
 */
int drm_prime_surface_from_fd(struct task *task, int fd,
                              struct drm_device *device,
                              struct drm_vmwgfx_surface **surface_out)
{
    if (surface_out) *surface_out = NULL;
    if (!task || !device || !surface_out) return -LINUX_EINVAL;
    struct task_file *file = task_file_for_fd(task, fd);
    if (!file) return -LINUX_EBADF;
    struct drm_prime_file *prime = file->drm_prime_file;
    if (!prime || prime->device != device || !prime->surface)
        return -LINUX_EINVAL;
    struct drm_vmwgfx_surface_metadata metadata;
    if (drm_vmwgfx_surface_get_metadata(prime->surface, &metadata) < 0 ||
        !metadata.shareable)
        return -LINUX_EINVAL;
    int ret = drm_vmwgfx_surface_get(prime->surface);
    if (ret) return -LINUX_EINVAL;
    *surface_out = prime->surface;
    return 0;
}

/**
 * @brief Allocate a new descriptor slot referring to an existing cached PRIME
 *        open-file description.
 * @param task Caller receiving the descriptor.
 * @param description Existing shared PRIME OFD; its surface and cache binding
 *                     remain owned by the description.
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

/**
 * @brief Export an authorized shareable surface through a cached opaque PRIME
 *        OFD and allocate its first descriptor transactionally.
 * @param task Caller whose fd namespace and current filesystem owner are used.
 * @param file DRM card OFD authorizing the surface handle lookup.
 * @param argument Kernel copy of PRIME_HANDLE_TO_FD input/output fields.
 * @param created_fd_out Optional output recording the fd for syscall-layer
 *                       rollback if copying the result to userspace fails.
 * @return Zero with argument->fd allocated but still rollback-capable, or a
 *         negative errno; all pre-publication failures release temporary refs.
 */
static int drm_prime_export_surface(struct task *task, struct drm_file *file,
                                    struct drm_prime_handle *argument,
                                    int *created_fd_out)
{
    const uint32_t allowed = DRM_PRIME_FD_CLOEXEC | DRM_PRIME_FD_RDWR;
    if (argument->flags & ~allowed) return -LINUX_EINVAL;
    struct drm_vmwgfx_surface *surface =
        drm_vmwgfx_surface_lookup_get(file, argument->handle);
    if (!surface) return -LINUX_EINVAL;
    struct drm_vmwgfx_surface_metadata metadata;
    int ret = drm_vmwgfx_surface_get_metadata(surface, &metadata);
    if (ret || !metadata.shareable) {
        drm_vmwgfx_surface_put(surface);
        return -LINUX_EINVAL;
    }
    uint64_t size;
    ret = drm_prime_surface_size(&metadata, &size);
    if (ret) {
        drm_vmwgfx_surface_put(surface);
        return ret;
    }

    struct task_file *cached =
        drm_vmwgfx_surface_prime_description(surface);
    if (cached) {
        int fd;
        ret = drm_prime_dup_cached(task, cached, argument->flags, &fd);
        drm_vmwgfx_surface_put(surface);
        if (ret) return ret;
        argument->fd = fd;
        if (created_fd_out) *created_fd_out = fd;
        return 0;
    }

    struct drm_prime_file *prime = kernel_malloc(sizeof(*prime));
    if (!prime) {
        drm_vmwgfx_surface_put(surface);
        return -LINUX_ENOMEM;
    }
    ret = drm_vmwgfx_surface_get(surface);
    if (ret) {
        drm_vmwgfx_surface_put(surface);
        kernel_free(prime);
        return ret;
    }
    *prime = (struct drm_prime_file){
        .surface = surface,
        .device = file->device,
    };
    struct task_file *slot = NULL;
    int fd = task_allocate_fd(task, 0, &slot);
    if (fd < 0) {
        drm_vmwgfx_surface_put(surface);
        drm_vmwgfx_surface_put(surface);
        kernel_free(prime);
        return fd;
    }
    uint64_t ino = __atomic_add_fetch(&drm_prime_next_ino, 1u,
                                      __ATOMIC_RELAXED);
    if (!ino) ino = __atomic_add_fetch(&drm_prime_next_ino, 1u,
                                       __ATOMIC_RELAXED);
    slot->kind = TASK_FILE_KIND_DRM_PRIME;
    slot->flags = (argument->flags & DRM_PRIME_FD_RDWR) ? RELIEFOS_O_RDWR
                                                        : RELIEFOS_O_RDONLY;
    slot->node.type = RELIEFOS_FS_TYPE_FILE;
    slot->node.size = size;
    slot->drm_prime_file = prime;
    slot->drm_prime_ino = ino;
    slot->drm_prime_size = size;
    slot->drm_prime_uid = task->fsuid;
    slot->drm_prime_gid = task->fsgid;
    slot->fd_flags = (argument->flags & DRM_PRIME_FD_CLOEXEC)
                         ? RELIEFOS_FD_CLOEXEC : 0;

    struct task_file *description = task_file_get(slot);
    if (!description) {
        task_discard_file_fd(task, fd);
        drm_vmwgfx_surface_put(surface);
        return -LINUX_ENOMEM;
    }
    ret = drm_vmwgfx_surface_prime_bind(surface, description);
    if (!ret) prime->cache_bound = true;
    task_file_put(description);
    if (ret == -LINUX_EEXIST) {
        /* Another export won the cache race. Closing this unpublished OFD
         * drops its surface reference; duplicate the canonical cached OFD. */
        cached = drm_vmwgfx_surface_prime_description(surface);
        task_discard_file_fd(task, fd);
        if (!cached) {
            drm_vmwgfx_surface_put(surface);
            return -LINUX_EBUSY;
        }
        ret = drm_prime_dup_cached(task, cached, argument->flags, &fd);
        drm_vmwgfx_surface_put(surface);
        if (ret) return ret;
    } else if (ret) {
        task_discard_file_fd(task, fd);
        drm_vmwgfx_surface_put(surface);
        return ret;
    } else {
        drm_vmwgfx_surface_put(surface);
    }
    argument->fd = fd;
    if (created_fd_out) *created_fd_out = fd;
    return 0;
}

/**
 * @brief Import a same-device PRIME surface and transactionally grant this DRM
 *        file a typed logical reference.
 * @param task Caller whose fd table owns argument->fd.
 * @param file Target DRM card OFD receiving the deliberate per-file reference.
 * @param argument Kernel copy of PRIME_FD_TO_HANDLE input/output fields.
 * @param receipt Required rollback receipt consumed by the syscall copyout
 *                 success/failure path.
 * @return Zero with the imported handle and active receipt, or a negative
 *         errno without a retained surface reference.
 */
static int drm_prime_import_surface(struct task *task, struct drm_file *file,
                                    struct drm_prime_handle *argument,
                                    struct drm_vmwgfx_rollback_receipt *receipt)
{
    if (argument->flags) return -LINUX_EINVAL;
    struct drm_vmwgfx_surface *surface = NULL;
    int ret = drm_prime_surface_from_fd(task, argument->fd, file->device,
                                        &surface);
    if (ret) return ret;
    struct drm_vmwgfx_surface_metadata metadata;
    ret = drm_vmwgfx_surface_get_metadata(surface, &metadata);
    if (!ret)
        ret = drm_vmwgfx_surface_add_file_reference(file, surface, receipt);
    drm_vmwgfx_surface_put(surface);
    if (ret) return ret;
    argument->handle = metadata.handle;
    return 0;
}

/**
 * @brief Handle the two supported core PRIME ioctls on an authorized DRM OFD.
 * @param task Caller for fd allocation and user-independent input processing.
 * @param file Primary or render DRM OFD whose per-file handle table is used.
 * @param command PRIME_HANDLE_TO_FD or PRIME_FD_TO_HANDLE.
 * @param data Copied kernel ioctl structure with fields in/out as specified.
 * @param created_fd_out Optional syscall rollback output for exported fd.
 * @param receipt Required import rollback receipt for failed output copy.
 * @return Zero on supported success, negative errno for invalid/unsupported
 *         requests; descriptor and handle outputs remain failure-atomic.
 */
int drm_prime_ioctl(struct task *task, struct drm_file *file,
                    uint32_t command, void *data, int *created_fd_out,
                    struct drm_vmwgfx_rollback_receipt *receipt)
{
    if (created_fd_out) *created_fd_out = -1;
    if (!task || !file || !data || !receipt) return -LINUX_EINVAL;
    if (command == DRM_IOCTL_PRIME_HANDLE_TO_FD)
        return drm_prime_export_surface(task, file, data, created_fd_out);
    if (command == DRM_IOCTL_PRIME_FD_TO_HANDLE)
        return drm_prime_import_surface(task, file, data, receipt);
    return -LINUX_EOPNOTSUPP;
}

/**
 * @brief Drop cached-export association and the PRIME OFD's retained surface
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
    if (prime->surface) {
        if (prime->cache_bound)
            drm_vmwgfx_surface_prime_unbind(prime->surface, file);
        drm_vmwgfx_surface_put(prime->surface);
    }
    kernel_free(prime);
}

/**
 * @brief Report immediate ready bits for the opaque DMA-BUF's empty reservation.
 * @param file PRIME surface description queried by poll.
 * @param events Requested poll events; unsupported bits are ignored.
 * @return Requested subset of POLLIN/POLLRDNORM/POLLOUT/POLLWRNORM, or zero
 *         when file is not a PRIME description.
 */
uint32_t drm_prime_file_poll(const struct task_file *file, uint32_t events)
{
    if (!file || !file->drm_prime_file) return 0;
    const uint32_t ready = POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM;
    return events & ready;
}

/**
 * @brief Implement the opaque TTM object's zero-offset SET and END seeks.
 * @param file PRIME surface description holding the exported page-aligned size.
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
 * @param file PRIME surface description to access.
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
 * @brief Validate Linux DMA_BUF_IOCTL_SYNC flags on the opaque surface OFD.
 * @param task Caller used to safely copy the sync structure from argument.
 * @param file PRIME surface description receiving the ioctl.
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
    if (user_copy_from_task(task, &sync, argument, sizeof(sync)) < 0)
        return -LINUX_EFAULT;
    if ((sync.flags & ~((uint64_t)DRM_DMA_BUF_SYNC_RW |
                        DRM_DMA_BUF_SYNC_END)) ||
        !(sync.flags & DRM_DMA_BUF_SYNC_RW))
        return -LINUX_EINVAL;
    return 0;
}

/**
 * @brief Enforce canonical OFD access precedence before rejecting unsupported
 *        opaque surface mappings.
 * @param file PRIME description being mapped.
 * @param flags Linux mmap flags, including MAP_SHARED when applicable.
 * @param protection Requested PROT_* access bits.
 * @return Negative EACCES when read/write access is forbidden by the OFD mode;
 *         otherwise negative EINVAL because the surface has no mmap operation.
 */
int drm_prime_file_mmap(struct task_file *file, uint64_t flags,
                        uint64_t protection)
{
    if (!file || !file->drm_prime_file) return -LINUX_EBADF;
    if (!file_can_read(file)) return -LINUX_EACCES;
    if ((flags & LINUX_MAP_SHARED) && (protection & LINUX_PROT_WRITE) &&
        !file_can_write(file))
        return -LINUX_EACCES;
    return -LINUX_EINVAL;
}
