#include "../internal.h"



/**
 * @brief Read one bounded slice of DRM event data into user memory.
 * @param task Current task used for the DRM event copyout; not retained.
 * @param file Borrowed DRM file description; it must be open for reading.
 * @param buffer User address of the writable destination range.
 * @param length Requested writable byte range; one DRM read is capped at
 *        RELIEFOS_FS_IO_SLICE_BYTES.
 * @return Bytes read, or negative EBADF/EFAULT or the DRM read's errno.
 */
int64_t syscall_drm_read(struct task *task, struct task_file *file,
                                uint64_t buffer, uint64_t length)
{
    uint32_t request_len;
    if (!file || !file->drm_file || !file_can_read(file))
        return -RELIEFOS_EBADF;
    if (!user_range_writable(buffer, length)) return -RELIEFOS_EFAULT;
    request_len = length > RELIEFOS_FS_IO_SLICE_BYTES
        ? RELIEFOS_FS_IO_SLICE_BYTES : (uint32_t)length;
    return drm_file_read(task, file, (void *)(uintptr_t)buffer, request_len);
}

/**
 * @brief Read DRM event data with a positional syscall's offset validation.
 * @param task Current task used for the DRM event copyout; not retained.
 * @param file Borrowed DRM file description; it must be open for reading.
 * @param buffer User address of the writable destination range.
 * @param length Requested byte count, subject to the shared read slice limit.
 * @param position Signed file offset encoded in the syscall register; DRM event
 *        reads ignore its value after rejecting negative offsets.
 * @return Bytes read, negative EINVAL for a negative offset, or the DRM read's
 *         negative errno.
 */
int64_t syscall_drm_pread(struct task *task, struct task_file *file,
                                 uint64_t buffer, uint64_t length,
                                 uint64_t position)
{
    if ((int64_t)position < 0) return -RELIEFOS_EINVAL;
    return syscall_drm_read(task, file, buffer, length);
}

/**
 * @brief Apply DRM's no-op seek behavior after validating the seek request.
 * @param file Borrowed task file; it must refer to a DRM file description.
 * @param whence Linux seek selector, checked by the existing 32-bit <= 4 rule.
 * @return The unchanged file offset, or negative EBADF/EINVAL on invalid input.
 */
int64_t syscall_drm_llseek(struct task_file *file, uint64_t whence)
{
    /* Linux ksys_lseek() rejects values beyond SEEK_MAX before calling the
     * driver's noop_llseek(), which otherwise returns the unchanged f_pos. */
    if (!file || !file->drm_file) return -RELIEFOS_EBADF;
    if ((uint32_t)whence > 4u) return -RELIEFOS_EINVAL; /* SEEK_HOLE */
    return (int64_t)file->offset;
}

/**
 * @brief Read DRM event data across an already imported iovec array.
 * @param task Current task used for DRM event copyout; not retained.
 * @param file Borrowed DRM file description; it must be open for reading.
 * @param iov Kernel-side iovec copy imported by the vector syscall dispatcher;
 *        each writable user range is checked before any read is issued.
 * @param iovcnt Number of entries in iov; values above UIO_MAXIOV are rejected.
 * @param positional Whether position is a preadv offset that needs validation.
 * @param position Signed file offset encoded in the syscall register; DRM event
 *        reads ignore it after rejecting a negative positional offset.
 * @param flags Vector-read flags; zero and LINUX_RWF_HIPRI are supported.
 * @return Total bytes read, a partial count after a later read error, or
 *         negative EBADF/EINVAL/EFAULT/EOPNOTSUPP or the DRM read's errno.
 */
int64_t syscall_drm_readv(struct task *task, struct task_file *file,
                                 const struct iovec *iov, uint64_t iovcnt,
                                 bool positional, uint64_t position,
                                 uint64_t flags)
{
    uint64_t requested = 0;
    int64_t total = 0;
    if (!file || !file->drm_file || !file_can_read(file))
        return -RELIEFOS_EBADF;
    if (positional && (int64_t)position < 0) return -RELIEFOS_EINVAL;
    if (flags && flags != LINUX_RWF_HIPRI) return -RELIEFOS_EOPNOTSUPP;
    if (iovcnt > UIO_MAXIOV) return -RELIEFOS_EINVAL;
    for (uint64_t i = 0; i < iovcnt; ++i) {
        uint64_t length = iov[i].iov_len;
        if ((int64_t)length < 0) return -RELIEFOS_EINVAL;
        if (length > 0x7ffff000u - requested)
            length = 0x7ffff000u - requested;
        if (length && !user_range_writable((uintptr_t)iov[i].iov_base, length))
            return -RELIEFOS_EFAULT;
        requested += length;
    }
    for (uint64_t i = 0; i < iovcnt && requested; ++i) {
        uint64_t length = iov[i].iov_len;
        if (length > 0x7ffff000u - (uint64_t)total)
            length = 0x7ffff000u - (uint64_t)total;
        if (!length) continue;
        int64_t part = syscall_drm_read(task, file,
                                       (uintptr_t)iov[i].iov_base, length);
        if (part < 0) return total ? total : part;
        total += part;
        if ((uint64_t)part < length) break;
    }
    return total;
}
