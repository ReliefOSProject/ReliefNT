#ifndef RELIEFNT_DRM_INTERNAL_H
#define RELIEFNT_DRM_INTERNAL_H

#ifndef __KERNEL__
#define RELIEFNT_DRM_RESTORE_KERNEL_MACRO
#define __KERNEL__
#endif
#include <drm/drm.h>
#ifdef RELIEFNT_DRM_RESTORE_KERNEL_MACRO
#undef __KERNEL__
#undef RELIEFNT_DRM_RESTORE_KERNEL_MACRO
#endif
#include <linux/errno.h>
#include <reliefnt/drm.h>
#include <reliefnt/pci.h>
#include <reliefnt/wait.h>

struct drm_magic_entry;
struct drm_device;
struct drm_file;
struct task_file;
struct drm_bo;
struct drm_bo_handle;
struct vg_resource;
struct vg_file;
struct vg_kms_fb;
struct vg_event;

#ifdef CONFIG_SWAP
#include <reliefnt/syscall_internal.h>
/** @brief Prepare actual DRM user bytes at the outer execution writer.
 * @param task Live syscall owner. @param address Original user address.
 * @param length Actual bytes; internally sliced at 32KiB. @param writing
 * True for output. @return Zero, EFAULT or the private contention marker.
 * No user pointer or borrowed object survives a returned marker.
 */
int drm_user_prepare(struct task *task,uint64_t address,uint64_t length,bool writing);
/** @brief Import a bounded operand into owned syscall-scope storage.
 * @param task Live syscall owner. @param destination Required kernel bytes.
 * @param address Original user address. @param length Counted bytes.
 * @return Zero, an allocation/fault errno, or private contention. Scoped
 * prefixes survive a matching retry and common cancellation owns them.
 */
int drm_user_import(struct task *task,void *destination,uint64_t address,uint32_t length);
#endif

#define DRM_FAILDIAG_FIELD_MAX 8u

struct drm_faildiag_context {
    uint32_t pid;
    uint32_t command;
    uint32_t handler;
    const char *handler_name;
    bool attempted;
};

struct drm_faildiag_field {
    const char *name;
    uint64_t value;
};

struct drm_magic_entry {
    struct drm_magic_entry *next;
    struct drm_file *file;
    uint32_t magic;
};

struct drm_master {
    uint32_t references;
    uint32_t next_magic;
    char unique[32];
    uint32_t unique_length;
    struct drm_magic_entry *magics;
};

struct drm_file {
    struct drm_device *device;
    struct drm_master *master;
    uint32_t owner_tgid;
    uint32_t magic;
    bool is_master;
    bool was_master;
    bool authenticated;
    bool is_render;
    struct kernel_wait_queue event_wait;
    uint32_t next_bo_handle;
    struct drm_bo_handle *bo_handles;
    struct vg_file *virtio;
    struct vg_kms_fb *virtio_fbs;
    struct vg_event *virtio_events, *virtio_events_tail;
    uint32_t virtio_event_bytes;
    bool universal_planes;
};

struct drm_device {
    struct pci_device pci;
    struct drm_master *master;
    bool registered;
};

struct drm_device *drm_device_get(void);

/** @brief Identify the VirtIO backend from the admitted singleton's PCI identity.
 * @return True for modern VirtIO-GPU; execution-owned, no optional callback.
 */
static inline bool drm_is_virtio(void)
{
    struct drm_device *device = drm_device_get();
    return device && device->pci.vendor_id == 0x1af4 && device->pci.device_id == 0x1050;
}

struct drm_master *drm_master_create(void);
void drm_master_get(struct drm_master *master);
void drm_master_put(struct drm_master *master);
int drm_file_set_master(struct task *task, struct drm_file *file);
int drm_file_drop_master(struct task *task, struct drm_file *file);

/** @brief Allocate a per-file BO, respecting the owning device's address mask.
 * @param file Borrowed file and device. @param diagnostics Optional diagnostics.
 * @param size Nonzero requested bytes. @param handle_out Required handle output.
 * @param map_offset_out Required mapping-offset output.
 * @return Zero with an owned handle, or negative errno with no published BO.
 */
int drm_bo_create(struct drm_file *file,
                  struct drm_faildiag_context *diagnostics,
                  uint64_t size, uint32_t *handle_out,
                  uint64_t *map_offset_out);
int drm_bo_handle_delete(struct drm_file *file, uint32_t handle);
int drm_bo_ioctl(struct drm_file *file, uint32_t command, void *data,
                 uint32_t *created_handle_out,
                 struct drm_faildiag_context *diagnostics);
void drm_bo_file_release(struct drm_file *file);
struct drm_bo *drm_bo_lookup_handle_get(struct drm_file *file, uint32_t handle);
void drm_bo_get(struct drm_bo *bo);
void drm_bo_put(struct drm_bo *bo);
uint64_t drm_bo_page_physical(const struct drm_bo *bo, uint64_t offset);
uint64_t drm_bo_size(const struct drm_bo *bo);
/** @brief Borrow immutable VirtIO metadata while retaining its GEM BO.
 * @param bo Retained BO. @return Metadata or NULL; execution ownership required.
 */
struct vg_resource *drm_bo_virtio(const struct drm_bo *bo);
/** @brief Attach one owned VirtIO resource to an unpublished GEM BO.
 * @param bo Exclusive BO. @param resource Owned metadata; final put destroys it.
 */
void drm_bo_set_virtio(struct drm_bo *bo, struct vg_resource *resource);
/** @brief Snapshot a retained BO's globally unique mmap selector.
 * @param bo Retained BO. @return Byte selector, or zero for NULL.
 */
uint64_t drm_bo_offset(const struct drm_bo *bo);
/** @brief Import a same-device BO into the file's GEM namespace, deduplicating it.
 * @param file Live target. @param bo Retained source. @param handle Output handle.
 * @param created Output true only for a new handle needing copyout rollback.
 * @return Zero or errno. Task context, allocation occurs before the BO lock.
 */
int drm_bo_handle_import(struct drm_file *file, struct drm_bo *bo, uint32_t *handle, bool *created);
/** @brief Borrow a weak canonical PRIME description under execution ownership.
 * @param bo Retained BO. @return Cached OFD or NULL; no reference transferred.
 */
struct task_file *drm_bo_prime_description(struct drm_bo *bo);
/** @brief Bind a weak export cache, avoiding a BO/OFD reference cycle.
 * @param bo Retained BO. @param description Canonical OFD. @return Zero or EEXIST.
 */
int drm_bo_prime_bind(struct drm_bo *bo,struct task_file *description);
/** @brief Remove a final export's weak cache under execution ownership.
 * @param bo Retained BO. @param description Dying canonical export.
 */
void drm_bo_prime_unbind(struct drm_bo *bo,struct task_file *description);
/** @brief Resolve shared GEM pages through an admitted PRIME OFD.
 * @param file Retained export. @param offset Page-aligned offset. @param length
 * Valid nonzero extent. @param out Owned BO output. @return Zero or errno.
 */
int drm_prime_mmap_bo(struct task_file *file,uint64_t offset,uint64_t length,struct drm_bo **out);
int drm_bo_gpu_reserve(struct drm_bo *bo);
void drm_bo_gpu_release(struct drm_bo *bo);
bool drm_bo_gpu_busy(struct drm_bo *bo);

int drm_kms_ioctl(struct task *task, struct drm_file *file, uint32_t command,
                  void *data, uint32_t *created_fb_out);
void drm_kms_file_release(struct drm_file *file);
void drm_kms_drop_master(struct drm_file *file);
void drm_kms_remove_fb(struct drm_file *file, uint32_t fb_id);

void drm_faildiag_record(struct drm_faildiag_context *context, int error,
                         const char *stage,
                         const struct drm_faildiag_field *fields,
                         uint32_t field_count);
bool drm_faildiag_is_enabled(void);
/**
 * @brief Dispatch a copied PRIME ioctl on a card OFD and record transactional
 *        outputs for syscall-layer rollback.
 * @param task Calling task used for user-memory validation and descriptor
 *             allocation.
 * @param file Authorized DRM card OFD whose per-file GEM handles apply.
 * @param command PRIME_HANDLE_TO_FD or PRIME_FD_TO_HANDLE.
 * @param data Kernel copy of the ioctl argument structure; outputs are written
 *             here for the syscall layer to copy back.
 * @param created_fd_out Optional output set to a newly allocated descriptor so
 *                       the caller can close it if copyout fails.
 * @param created_handle_out Optional output for a newly imported GEM handle.
 * @return Zero on success or a negative Linux errno.
 */
int drm_prime_ioctl(struct task *task, struct drm_file *file,
                    uint32_t command, void *data, int *created_fd_out,
                    uint32_t *created_handle_out);

#endif
