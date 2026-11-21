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
struct svga_file;
struct svga_render_file;
struct graphics_connector_info;

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
    bool universal_planes, atomic_modeset, cursor_hotspot;
    struct svga_file *svga;
    struct svga_render_file *svga_render;
};

enum drm_ioctl_permission {
    DRM_PERMISSION_AUTH = 1u << 0,
    DRM_PERMISSION_MASTER = 1u << 1,
    DRM_PERMISSION_ADMIN = 1u << 2,
    DRM_PERMISSION_RENDER_ALLOW = 1u << 3,
};

struct drm_driver_ioctl {
    uint32_t command;
    uint32_t permission;
};

/* Internal to the pinned DRM provider. The device owns one immutable table.
 * Task methods run under execution ownership. File methods use the exact
 * device retained by the OFD, including rollback and final close. Panic
 * methods may only use preallocated state, after other CPUs have stopped. */
struct drm_driver {
    const char *name, *description;
    int major, minor, patchlevel;
    int (*start)(struct pci_device *out);
    void (*stop)(void);
    bool (*render_ready)(void);
    const struct drm_driver_ioctl *ioctls;
    unsigned ioctl_count;
    int (*ioctl)(struct task *task, struct drm_file *file, uint32_t command,
                 void *data, uint32_t *handle, int *fd, uint32_t *object);
    void (*ioctl_rollback)(struct drm_file *file, uint32_t command, const void *data, uint32_t object);
    bool prime_bo;
    /* Optional demand-mapping preparation for non-dumb BOs. */
    int (*bo_cpu_access)(struct drm_bo *bo);
    int (*prime_export)(struct task *task, struct drm_file *file, struct drm_prime_handle *arg);
    int (*prime_import)(struct drm_file *file, void *object, uint32_t *handle);
    int (*dumb_init)(struct drm_bo *bo, uint32_t width, uint32_t height,
                     uint32_t bpp, uint32_t pitch);
    int (*get_cap)(struct drm_get_cap *cap);
    bool universal_planes, atomic_modeset, cursor_hotspot;
    void (*file_release)(struct drm_file *file);
    int (*events_read)(struct task *task, struct drm_file *file,
                       uint64_t address, uint32_t length);
    bool (*events_pending)(const struct drm_file *file);
    int (*kms_ioctl)(struct task *task, struct drm_file *file, uint32_t command,
                     void *data, uint32_t *created_fb);
    void (*kms_file_release)(struct drm_file *file);
    void (*kms_remove_fb)(struct drm_file *file, uint32_t id);
    bool (*kms_active)(void);
    void (*kms_vt_release)(void);
    int (*connector_info)(uint32_t index, struct graphics_connector_info *out);
    uint32_t (*retire)(uint32_t budget);
    void (*panic_prepare)(void);
    void (*panic_damage)(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    void (*panic_present)(void);
};

struct drm_device {
    struct pci_device pci;
    const struct drm_driver *driver;
    struct drm_master *master;
    bool registered;
};

struct drm_device *drm_device_get(void);
int drm_device_register(const struct pci_device *device, const struct drm_driver *driver);
void drm_device_unregister(void);
void drm_device_hotplug(const struct drm_device *device);

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
/* Allocate an unmapped backing with one owned reference and no file handle. */
int drm_bo_alloc(uint64_t size, struct drm_bo **out);
int drm_bo_handle_delete(struct drm_file *file, uint32_t handle);
/** @brief Add a separately closeable GEM handle to a retained same-device BO.
 * @param file Target OFD. @param bo Borrowed BO. @param handle New ID on success.
 * @return Zero or errno. The handle keeps its own BO reference.
 */
int drm_bo_handle_create(struct drm_file *file, struct drm_bo *bo, uint32_t *handle);
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
bool drm_bo_is_dumb(const struct drm_bo *bo);
struct device_mapping_access *drm_bo_mapping_access(struct drm_bo *bo);
const struct drm_driver *drm_prime_mmap_driver(const struct task_file *file);

int drm_kms_copy_array(struct task *task, uint64_t address, uint32_t capacity,
                       const void *data, uint32_t count, uint32_t size);
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
                    uint32_t *created_handle_out, uint32_t *created_object_out);

/* Opaque PRIME objects have no GEM mmap and belong to one DRM device.
 * Export consumes the caller's object reference on every return. The optional
 * weak cache belongs to the object and is cleared before the final put. */
int drm_prime_export_object(struct task *task, struct drm_device *device, uint32_t flags,
                            uint64_t size, void *object, void (*put)(void *), struct task_file **cache);
void *drm_prime_lookup_object(struct task_file *source, const struct drm_device *device);
/** @brief Borrow the BO of a matching PRIME OFD under execution ownership.
 * @param source Retained PRIME OFD. @param device Expected DRM device.
 * @return A borrowed BO or NULL. Opaque exports have no BO.
 */
struct drm_bo *drm_prime_lookup_bo(struct task_file *source, const struct drm_device *device);

#endif
