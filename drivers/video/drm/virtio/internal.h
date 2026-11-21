/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_VIRTGPU_INTERNAL_H
#define RELIEFNT_VIRTGPU_INTERNAL_H
#include "../drm_internal.h"
#include "../memory.h"
#include <reliefnt/virtio.h>
#include <linux/virtio_gpu.h>
#include <drm/virtgpu_drm.h>
#include <drm/drm_mode.h>

#define VG_MAX_CAPSETS 16u
#define VG_MAX_COMMAND (16u * 1024u * 1024u)
#define VG_MAX_RESOURCES 4096u
#define VG_MAX_WIDTH 8192u
#define VG_MAX_HEIGHT 8192u
#define VG_IOCTL_UNOWNED 515
struct graphics_connector_info;

struct vg_request {
    struct vg_request *next;
    struct dma_buffer dma;
    struct virtio_gpu_ctrl_hdr *response;
    uint64_t fence, submitted_us;
    uint32_t response_bytes, used_bytes, expected;
    uint32_t context;
    int result;
    bool done, automatic, pinned, cursor;
    void (*complete)(struct vg_request *request);
    void *private;
};
struct vg_capset { uint32_t id, version, size; };
struct vg_device {
    struct virtio_pci transport;
    struct virtio_queue queues[2];
    struct vg_request *requests;
    struct vg_capset capsets[VG_MAX_CAPSETS];
    struct virtio_gpu_resp_display_info display;
    struct virtio_gpu_resp_edid edid[VIRTIO_GPU_MAX_SCANOUTS];
    uint64_t next_fence;
    uint32_t next_resource, next_context, capset_count, scanout_count, request_count;
    bool ready, virgl, fault, resetting, display_changed, reaping;
};
struct vg_resource {
    uint32_t id, width, height, depth, format, target, levels, array_size;
    uint32_t stride;
    bool virgl;
    uint64_t last_fence;
    int error;
    struct vg_file_resource *contexts;
};
struct vg_file_resource {
    struct vg_file_resource *next, *resource_next;
    struct vg_resource *resource;
    struct vg_file *owner;
};
struct vg_file {
    uint32_t context;
    struct vg_file_resource *resources;
};

extern struct vg_device vg;

/** @brief Discover/initialize one modern GPU; task context, no borrowed identity.
 * @param out Required identity output. @return Zero or device/protocol errno.
 */
int vg_device_start(struct pci_device *out);
/** @brief Reset and release hardware only after all external provider users drain. */
void vg_device_stop(void);
/** @brief Fault/reset the GPU and complete requests only after observed DMA stop.
 * @param error Negative completion result. @return Zero if reset stopped DMA,
 * otherwise a negative errno with all exposed pages and callbacks retained.
 * Task/execution context; no resource callbacks or allocations under IRQ locks.
 */
int vg_device_fault(int error);
/** @brief Poll completions and retire bounded work from the execution owner.
 * @param budget Desired retirement count. @return Work retired.
 */
uint32_t vg_process_retirements(uint32_t budget);
/** @brief Allocate/stage a kernel command in owned coherent storage.
 * @param command Kernel packet or NULL for zero-filled staging. @param bytes Packet length.
 * @param response_bytes Response capacity, zero for cursorq. @param expected
 * Expected response type. @param cursor Use independent cursor queue.
 * @return Owned request or NULL. Task/execution context, no user pointer escapes.
 */
struct vg_request *vg_request_create(const void *command, uint32_t bytes,
    uint32_t response_bytes, uint32_t expected, bool cursor);
/** @brief Submit a staged request, keeping all DMA and its module pin alive.
 * @param request Owned prepared request. @return Zero or submission errno.
 */
int vg_request_submit(struct vg_request *request);
/** @brief Poll one synchronous request; timeout resets before releasing DMA.
 * @param request Submitted request. @return Zero or protocol/device errno.
 */
int vg_request_wait(struct vg_request *request);
/** @brief Free a completed/unsubmitted request; live requests are retained.
 * @param request Owned request, consumed only when no device access remains.
 */
void vg_request_free(struct vg_request *request);
/** @brief Execute one kernel command and copy a bounded response after fencing.
 * @param command Kernel packet. @param bytes Packet bytes. @param response
 * Optional output. @param response_bytes Capacity. @param expected Response type.
 * @return Zero or errno. Task context; no device or spinlock held across waiting.
 */
int vg_command(const void *command, uint32_t bytes, void *response,
                 uint32_t response_bytes, uint32_t expected);
/** @brief Drain both rings without allocation, resource release or task pointers. */
void vg_poll(void);
/** @brief Read the kernel's cross-CPU CLOCK_MONOTONIC for GPU deadlines.
 * @return Monotonic microseconds. IRQ/task safe; no GPU lock or allocation.
 */
uint64_t vg_clock_us(void);
/** @brief Refresh display info/EDID after a device display-change notification.
 * @return Zero or errno. Task context, cached data published after response.
 */
int vg_display_refresh(void);
/** @brief Allocate kernel-owned console DMA and its 2D resource before ready.
 * @return Zero or errno; init task context, no persistent external module pin.
 */
int vg_console_start(void);
/** @brief Free console backing after observed GPU reset; task context only. */
void vg_console_stop(void);
/** @brief Publish retained console pixels from task context.
 * @param restore Bind scanout zero after KMS releases all outputs.
 * @return Zero or hardware errno; ordinary updates coalesce CPU damage.
 */
int vg_console_update(bool restore);
/** @brief Borrow the retained console's immutable geometry for fatal output.
 * @param pixels CPU DMA pointer output. @param resource Host ID output.
 * @param width Surface width output. @param height Surface height output.
 * @return True when initialized; fatal owner only, peers stopped, never resumes.
 */
bool vg_console_snapshot(void **pixels,uint32_t *resource,uint32_t *width,uint32_t *height);
/** @brief Preallocate fatal command DMA and reserve a control chain before ready.
 * @return Zero or errno; init context before ordinary submissions.
 */
int vg_panic_start(void);
/** @brief Release fatal command DMA after observed device reset; task context. */
void vg_panic_stop(void);
/** @brief Select current scanout or retained console without locks/allocation.
 * Fatal owner only after other CPUs stop; normal execution must never resume.
 */
void vg_panic_prepare(void);
/** @brief Copy only painted boot-framebuffer cells into the fatal target.
 * @param x Left pixel. @param y Top pixel. @param width Painted width.
 * @param height Painted height. Fatal owner only, no locks or allocations.
 */
void vg_panic_damage(uint32_t x,uint32_t y,uint32_t width,uint32_t height);
/** @brief Publish bounded fatal transfers via the reserved queue descriptors.
 * Fatal owner only, peers parked, interrupts disabled, no normal execution resumes.
 */
void vg_panic_present(void);
/** @brief Create/attach a resource for a retained GEM BO.
 * @param bo Owned existing pages. @param parameters Validated dimensions/format.
 * @param virgl Use 3D allocation. @return Zero or errno; failures unwind hardware.
 */
int vg_resource_create(struct drm_bo *bo, const struct drm_virtgpu_resource_create *parameters, bool virgl);
/** @brief Retire a final resource before its BO pages can be returned.
 * @param resource Owned metadata. @return True if DMA stopped; false quarantines
 * the resource, pages and module pin after an unresettable device failure.
 */
bool vg_resource_destroy(struct vg_resource *resource);
/** @brief Transfer CPU damage, if needed, and flush one scanout resource.
 * @param bo Retained resource BO. @param rect Bounds within resource.
 * @return Zero or errno. Task context; preserves backing through completion.
 */
int vg_resource_flush(struct drm_bo *bo, struct virtio_gpu_rect rect);
/** @brief Upload a 2D backing rectangle before scanout/cursor publication.
 * @param bo Retained BO. @param rect Valid resource rectangle.
 * @return Zero for VirGL or completed transfer; negative errno otherwise.
 */
int vg_resource_upload(struct drm_bo *bo, struct virtio_gpu_rect rect);
/** @brief Select a retained resource or disable one scanout.
 * @param scanout Hardware output. @param resource Resource ID, zero to disable.
 * @param rect Resource subrectangle. @return Zero or hardware errno.
 */
int vg_scanout(uint32_t scanout, uint32_t resource, struct virtio_gpu_rect rect);
/** @brief Lazily create a private VirGL context for one DRM OFD.
 * @param file Live DRM OFD. @return Zero or errno, no shared process authority.
 */
int vg_context_create(struct drm_file *file);
/** @brief Attach a retained resource once to the OFD's rendering context.
 * @param file Live OFD. @param bo Retained GEM BO. @return Zero or errno.
 */
int vg_context_attach(struct drm_file *file, struct drm_bo *bo);
/** @brief Destroy an OFD's context and release its retained resource references.
 * @param file Closing OFD. Task context after queued submissions drain.
 */
void vg_context_close(struct drm_file *file);
/** @brief Detach final resource bindings before its backing pages can retire.
 * @param resource Final owned resource. @return Zero or protocol errno; task context.
 */
int vg_context_detach_resource(struct vg_resource *resource);
/** @brief Dispatch copied Linux VirtGPU private operations and core extensions.
 * @param task Current caller. @param file Authorized OFD. @param command Canonical
 * ioctl. @param data Kernel copy. @param created_handle BO rollback output.
 * @param created_fd Sync_file rollback output, populated only after submission.
 * @return Zero, errno or internal blocked status; no borrowed user pointer escapes.
 */
int vg_ioctl(struct task *task, struct drm_file *file, uint32_t command, void *data,
             uint32_t *created_handle, int *created_fd, uint32_t *created_object);
/** @brief Dispatch copied KMS arguments with all nested user ranges prepared.
 * @param task Caller. @param file Authorized OFD. @param command Canonical ioctl.
 * @param data Copied argument. @param created_fb Rollback output.
 * @return Zero, errno or private retry marker before irreversible operations.
 */
int vg_kms_ioctl(struct task *task, struct drm_file *file, uint32_t command, void *data, uint32_t *created_fb);
/** @brief Release one file's KMS objects and pending events.
 * @param file Closing/master-releasing OFD, never accessed after return.
 */
void vg_kms_file_release(struct drm_file *file);
/** @brief Remove a file-owned framebuffer after an ioctl copyout failure.
 * @param file Owner. @param id Published framebuffer ID.
 */
void vg_kms_remove_fb(struct drm_file *file, uint32_t id);
/** @brief Disable user scanouts and bind the driver's retained text surface. */
void vg_kms_vt_release(void);
/** @brief Report whether any scanout is currently enabled. @return Ownership. */
bool vg_kms_active(void);
/** @brief Copy one live output's status, supported modes and validated EDID.
 * @param index Zero-based scanout. @param out Required copied destination.
 * @return Zero or errno. Task/execution context under provider admission.
 */
int vg_connector_info(uint32_t index,struct graphics_connector_info *out);
#endif
