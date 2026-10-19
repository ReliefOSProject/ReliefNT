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
#include <reliefnt/svga.h>
#include <reliefnt/wait.h>

struct drm_magic_entry;
struct drm_device;
struct drm_file;
struct task_file;
struct drm_bo;
struct drm_bo_handle;
struct drm_kms_fb;
struct drm_vmwgfx_context;
struct drm_vmwgfx_surface;
struct drm_vmwgfx_file_ref;
struct drm_vmwgfx_exec_work;
struct drm_vmw_synccpu_arg;

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

struct drm_vmwgfx_rollback_receipt {
    uint32_t handle;
    uint8_t type;
    bool active;
};

struct drm_vmwgfx_surface_metadata {
    struct svga_surface_desc desc;
    uint32_t handle;
    uint32_t mip_levels[6];
    bool shareable;
    bool scanout;
};

struct drm_vmwgfx_surface_subresource {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
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
    /* GEM BO handles use a bounded per-file namespace. Future context and
     * surface handles use the disjoint DRM_RESOURCE_HANDLE_MIN range. */
    uint32_t next_bo_handle;
    uint32_t next_fence_handle;
    struct drm_bo_handle *bo_handles;
    void *vmwgfx_fences;
    struct drm_kms_fb *kms_fbs;
    struct drm_vmwgfx_file_ref *vmwgfx_resources;
};

struct drm_device {
    struct pci_device pci;
    struct drm_master *master;
    bool registered;
};

struct drm_device *drm_device_get(void);
bool drm_vmwgfx_vgpu9_ready(struct svga_info *info);
struct drm_master *drm_master_create(void);
void drm_master_get(struct drm_master *master);
void drm_master_put(struct drm_master *master);
int drm_file_set_master(struct task *task, struct drm_file *file);
int drm_file_drop_master(struct task *task, struct drm_file *file);

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
int drm_bo_gpu_reserve(struct drm_bo *bo);
void drm_bo_gpu_release(struct drm_bo *bo);
bool drm_bo_gpu_busy(struct drm_bo *bo);
int drm_bo_synccpu_lookup_get(struct drm_file *file, uint32_t handle,
                              uint32_t flags, struct drm_bo **out);
int drm_bo_synccpu_grab_retained(struct drm_file *file, uint32_t handle,
                                 struct drm_bo *bo, uint32_t flags);
int drm_bo_synccpu_ioctl(struct drm_file *file,
                         struct drm_vmw_synccpu_arg *arg);

int drm_kms_ioctl(struct task *task, struct drm_file *file, uint32_t command,
                  void *data, uint32_t *created_fb_out);
void drm_kms_process_retirements(uint32_t budget);
void drm_kms_vt_release(void);
void drm_kms_file_release(struct drm_file *file);
void drm_kms_drop_master(struct drm_file *file);
void drm_kms_remove_fb(struct drm_file *file, uint32_t fb_id);

int drm_vmwgfx_caps_ioctl(struct task *task, struct drm_file *file,
                          uint32_t command, void *data);
int drm_vmwgfx_resource_ioctl(struct task *task, struct drm_file *file,
                              uint32_t command, void *data,
                              struct drm_vmwgfx_rollback_receipt *receipt);
void drm_vmwgfx_resource_rollback(
    struct drm_file *file, struct drm_vmwgfx_rollback_receipt *receipt);
void drm_vmwgfx_resource_file_release(struct drm_file *file);
uint32_t drm_vmwgfx_process_retirements(uint32_t budget);
void drm_vmwgfx_execbuf_process_retirements(uint32_t budget);
int64_t drm_vmwgfx_execbuf_ioctl(struct task *task, struct drm_file *file,
                                 uint32_t command, void *data,
                                 uint64_t user_argument,
                                 struct drm_faildiag_context *diagnostics);
void drm_faildiag_record(struct drm_faildiag_context *context, int error,
                         const char *stage,
                         const struct drm_faildiag_field *fields,
                         uint32_t field_count);
bool drm_faildiag_is_enabled(void);
void drm_vmwgfx_execbuf_file_release(struct drm_file *file);
void drm_vmwgfx_cancel_task_continuation(struct task *task);
int64_t drm_vmwgfx_prepare_ioctl_continuation(
    struct task *task, struct task_file *syscall_file, struct drm_file *file,
    uint32_t command, uint64_t user_argument, void *data, uint32_t data_bytes,
    bool *handled, bool *skip_input);
bool drm_vmwgfx_task_uninterruptible(const struct task *task);
bool drm_vmwgfx_task_signal_busy(const struct task *task);

/* Retained typed lookups are for in-kernel validator, binding, and job users.
 * A successful lookup/get owns one reference, independent of the file's
 * logical handle count. get returns EINVAL once final logical unref begins. */
struct drm_vmwgfx_context *drm_vmwgfx_context_lookup_get(
    struct drm_file *file, uint32_t handle);
int drm_vmwgfx_context_get(struct drm_vmwgfx_context *context);
void drm_vmwgfx_context_put(struct drm_vmwgfx_context *context);
svga_handle drm_vmwgfx_context_backend_handle(
    const struct drm_vmwgfx_context *context);
uint32_t drm_vmwgfx_context_backend_id(
    const struct drm_vmwgfx_context *context);

struct drm_vmwgfx_surface *drm_vmwgfx_surface_lookup_get(
    struct drm_file *file, uint32_t handle);
int drm_vmwgfx_surface_get(struct drm_vmwgfx_surface *surface);
void drm_vmwgfx_surface_put(struct drm_vmwgfx_surface *surface);
svga_handle drm_vmwgfx_surface_backend_handle(
    const struct drm_vmwgfx_surface *surface);
uint32_t drm_vmwgfx_surface_backend_id(
    const struct drm_vmwgfx_surface *surface);
int drm_vmwgfx_surface_get_metadata(
    const struct drm_vmwgfx_surface *surface,
    struct drm_vmwgfx_surface_metadata *metadata);
int drm_vmwgfx_surface_get_subresource(
    const struct drm_vmwgfx_surface *surface, uint32_t face, uint32_t mip,
    struct drm_vmwgfx_surface_subresource *subresource);
int drm_vmwgfx_surface_add_file_reference(
    struct drm_file *file, struct drm_vmwgfx_surface *surface,
    struct drm_vmwgfx_rollback_receipt *receipt);
struct task_file *drm_vmwgfx_surface_prime_description(
    struct drm_vmwgfx_surface *surface);
int drm_vmwgfx_surface_prime_bind(struct drm_vmwgfx_surface *surface,
                                  struct task_file *description);
void drm_vmwgfx_surface_prime_unbind(struct drm_vmwgfx_surface *surface,
                                     struct task_file *description);
/**
 * @brief Dispatch a copied PRIME ioctl on a card OFD and record transactional
 *        outputs for syscall-layer rollback.
 * @param task Calling task used for user-memory validation and descriptor
 *             allocation.
 * @param file Authorized DRM card OFD whose per-file surface handles apply.
 * @param command PRIME_HANDLE_TO_FD or PRIME_FD_TO_HANDLE.
 * @param data Kernel copy of the ioctl argument structure; outputs are written
 *             here for the syscall layer to copy back.
 * @param created_fd_out Optional output set to a newly allocated descriptor so
 *                       the caller can close it if copyout fails.
 * @param receipt Required rollback receipt for an imported logical reference.
 * @return Zero on success or a negative Linux errno; on import, receipt remains
 *         active until the caller commits copyout or rolls it back.
 */
int drm_prime_ioctl(struct task *task, struct drm_file *file,
                    uint32_t command, void *data, int *created_fd_out,
                    struct drm_vmwgfx_rollback_receipt *receipt);
/**
 * @brief Resolve an fd to a same-device PRIME surface and retain its typed
 *        resource for the caller.
 * @param task Caller whose descriptor table supplies fd.
 * @param fd Surface PRIME descriptor number in task.
 * @param device Exact target DRM device identity required for same-device
 *               import.
 * @param surface_out Required output receiving one retained surface reference.
 * @return Zero on success, negative EBADF when fd is absent, or negative
 *         EINVAL for an existing wrong-object, foreign-device, stale, or
 *         non-shareable fd or unavailable surface ref; successful output is
 *         owned and must be put by the caller.
 */
int drm_prime_surface_from_fd(struct task *task, int fd,
                              struct drm_device *device,
                              struct drm_vmwgfx_surface **surface_out);

#endif
