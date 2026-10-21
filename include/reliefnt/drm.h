/* ReliefOS DRM primary-node boundary. */
#ifndef RELIEFNT_DRM_H
#define RELIEFNT_DRM_H

#include <reliefnt/types.h>

struct pci_device;
struct task;
struct task_file;
struct drm_bo;

/* Register only after the VMware SVGA PCI function has been bound. */
int drm_device_register(const struct pci_device *device);
bool drm_device_is_registered(void);
bool drm_device_render_node_ready(void);
int drm_device_pci_identity(struct pci_device *out);
bool drm_device_kms_scanout_active(void);
void drm_device_kms_vt_release(void);

/**
 * @brief Enable bounded DRM failure records only for the exact boot token.
 * @param cmdline Raw boot command line, which may be NULL.
 * @return No value; the diagnostic gate and per-boot record budget are reset.
 */
void drm_faildiag_configure(const char *cmdline);

/* Per-open-file operations. The task_file OFD owns this state. */
int drm_file_open(struct task *task, struct task_file *file);
void drm_file_close(struct task_file *file);
int drm_file_read(struct task *task, struct task_file *file,
                  void *buffer, uint32_t length);
uint32_t drm_file_poll(struct task_file *file, uint32_t events);
int64_t drm_file_ioctl(struct task *task, struct task_file *file,
                       uint32_t command, uint64_t argument);
/**
 * @brief Route one ioctl on a DRM card or opaque PRIME surface descriptor.
 * @param task Current caller; user pointers in argument are checked against it.
 * @param file Resolved open-file description, which must own a DRM card or
 *             PRIME surface object.
 * @param command Linux x86-64 ioctl command after narrowing to 32 bits.
 * @param argument Original 64-bit ioctl argument value, usually a user pointer.
 * @return The selected DRM/PRIME operation result, or negative EBADF when file
 *         does not contain either supported DRM object type.
 */
int64_t drm_fd_ioctl(struct task *task, struct task_file *file,
                     uint32_t command, uint64_t argument);
/**
 * @brief Release the surface reference and cache binding owned by the last
 *        reference to a PRIME open-file description.
 * @param file PRIME description whose final OFD reference is being released;
 *             null or a non-PRIME description is ignored.
 */
void drm_prime_file_close(struct task_file *file);
/**
 * @brief Report immediate DMA-BUF readiness while its reservation is empty.
 * @param file PRIME surface description to query.
 * @param events Requested poll bits; only supported readable/writable bits are
 *               returned.
 * @return The intersection of supported ready bits and events, or zero for an
 *         invalid PRIME description.
 */
uint32_t drm_prime_file_poll(const struct task_file *file, uint32_t events);
/**
 * @brief Implement the opaque surface DMA-BUF's restricted zero-offset seeks.
 * @param file PRIME description supplying the page-aligned object size.
 * @param offset Requested offset; only zero is supported.
 * @param whence RELIEFOS_SEEK_SET or RELIEFOS_SEEK_END.
 * @return Zero, the object size for SEEK_END, or a negative errno.
 */
int64_t drm_prime_file_llseek(struct task_file *file, int64_t offset,
                              uint32_t whence);
/**
 * @brief Return the canonical access-mode and unsupported-I/O result for a
 *        PRIME surface descriptor.
 * @param file PRIME description being accessed.
 * @param writing True for write access, false for read access.
 * @return Negative EBADF when the descriptor mode denies access, otherwise
 *         negative EINVAL because opaque surface data has no read/write path.
 */
int64_t drm_prime_file_rw(const struct task_file *file, bool writing);
/**
 * @brief Validate DMA_BUF_IOCTL_SYNC on an opaque surface descriptor.
 * @param task Caller used to validate and copy the 64-bit user argument.
 * @param file PRIME surface description receiving the request.
 * @param command 32-bit ioctl command; only DMA_BUF_IOCTL_SYNC is supported.
 * @param argument 64-bit user address of the sync flags structure.
 * @return Zero for supported START/END READ/WRITE synchronization requests,
 *         otherwise a negative Linux errno; success is not a GPU fence.
 */
int64_t drm_prime_file_ioctl(struct task *task, struct task_file *file,
                             uint32_t command, uint64_t argument);
/**
 * @brief Apply the access checks before rejecting unsupported PRIME mmap.
 * @param file PRIME description being mapped.
 * @param flags mmap flags, including MAP_SHARED when requested.
 * @param protection requested PROT_* bits.
 * @return Negative EACCES when the OFD mode denies access, otherwise negative
 *         EINVAL because opaque surface mappings are unsupported.
 */
int drm_prime_file_mmap(struct task_file *file, uint64_t flags,
                        uint64_t protection);
/* Run a bounded vmwgfx retirement pass; return the number of resources freed. */
uint32_t drm_vmwgfx_process_retirements(uint32_t budget);
void drm_vmwgfx_cancel_task_continuation(struct task *task);
bool drm_vmwgfx_task_uninterruptible(const struct task *task);
bool drm_vmwgfx_task_signal_busy(const struct task *task);

/* DRM mmap retains one BO and one task_file/OFD reference per live VMA. */
int drm_file_mmap_bo(struct task_file *file, uint64_t offset, uint64_t length,
                     struct drm_bo **bo_out, uint64_t *bo_offset_out);
void drm_bo_get(struct drm_bo *bo);
void drm_bo_put(struct drm_bo *bo);
uint64_t drm_bo_page_physical(const struct drm_bo *bo, uint64_t offset);
uint64_t drm_bo_size(const struct drm_bo *bo);

#endif
