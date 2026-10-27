/* SPDX-License-Identifier: Apache-2.0 */
#include "file_ops.h"
#include <reliefnt/drm.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/usercopy.h>
#include <reliefos/fs_abi.h>
#include <linux/uio.h>

/**
 * @brief Read one bounded slice of DRM event data into user memory.
 * @param task Current task used for the DRM event copyout; not retained.
 * @param file Borrowed DRM file description; it must be open for reading.
 * @param buffer User address of the writable destination range.
 * @param length Requested writable byte range; one DRM read is capped at
 *        RELIEFOS_FS_IO_SLICE_BYTES.
 * @return Bytes read, or negative EBADF/EFAULT or the DRM read's errno.
 * Execution ownership and an admitted OFD pin are required; no registry lock
 * spans user copying, wait registration or scheduling. No ownership transfer.
 */
int64_t drm_event_read(struct task *task, struct task_file *file,
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
 * @brief Read DRM event data across an already imported iovec array.
 * @param task Current task used for DRM event copyout; not retained.
 * @param file Borrowed DRM file description; it must be open for reading.
 * @param iov Kernel-side iovec copy imported by the vector syscall dispatcher;
 *        each writable user range is checked before any read is issued.
 * @param iovcnt Number of entries in iov; values above UIO_MAXIOV are rejected.
 * @param positional Whether position is a preadv offset that needs validation.
 * @param position Signed file offset encoded in the syscall register; DRM event
 *        reads ignore it after rejecting a negative positional offset.
 * @param flags Vector-read flags; zero and RWF_HIPRI are supported.
 * @return Total bytes read, a partial count after a later read error, or
 *         negative EBADF/EINVAL/EFAULT/EOPNOTSUPP or the DRM read's errno.
 * Execution-owned task context with the original OFD retained; vectors are
 * borrowed only during the call. No registry lock spans reads or scheduling.
 */
int64_t drm_event_readv(struct task *task, struct task_file *file,
                                 const struct iovec *iov, uint64_t iovcnt,
                                 bool positional, uint64_t position,
                                 uint64_t flags)
{
    uint64_t requested = 0;
    int64_t total = 0;
    if (!file || !file->drm_file || !file_can_read(file))
        return -RELIEFOS_EBADF;
    if (positional && (int64_t)position < 0) return -RELIEFOS_EINVAL;
    if (flags && flags != RWF_HIPRI) return -RELIEFOS_EOPNOTSUPP;
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
        int64_t part = drm_event_read(task, file,
                                       (uintptr_t)iov[i].iov_base, length);
        if (part < 0) return total ? total : part;
        total += part;
        if ((uint64_t)part < length) break;
    }
    return total;
}
