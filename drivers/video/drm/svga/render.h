/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SVGA_RENDER_H
#define RELIEFNT_SVGA_RENDER_H

#include "dma.h"
#include "submit.h"
#include <drm/vmwgfx_drm.h>
#include <reliefnt/usercopy.h>

#define SVGA_PACKET(command, type) struct { SVGA3dCmdHeader header; type body; }
#define SVGA_HEADER(command, type) {(command), sizeof(type)}
#define SVGA_RENDER_HANDLE_BASE 0x01000000u
#define SVGA_RENDER_MAX_OBJECTS 8192u

#define SVGA_VMW_IOR(name, type) DRM_IOR(DRM_COMMAND_BASE + DRM_VMW_ ## name, type)
#define SVGA_VMW_IOW(name, type) DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_ ## name, type)
#define SVGA_VMW_IOWR(name, type) DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_ ## name, type)
#define VMW_CREATE_CONTEXT SVGA_VMW_IOR(CREATE_CONTEXT, struct drm_vmw_context_arg)
#define VMW_UNREF_CONTEXT SVGA_VMW_IOW(UNREF_CONTEXT, struct drm_vmw_context_arg)
#define VMW_CREATE_EXTENDED_CONTEXT SVGA_VMW_IOWR(CREATE_EXTENDED_CONTEXT, union drm_vmw_extended_context_arg)
#define VMW_CREATE_SURFACE SVGA_VMW_IOWR(CREATE_SURFACE, union drm_vmw_surface_create_arg)
#define VMW_UNREF_SURFACE SVGA_VMW_IOW(UNREF_SURFACE, struct drm_vmw_surface_arg)
#define VMW_REF_SURFACE SVGA_VMW_IOWR(REF_SURFACE, union drm_vmw_surface_reference_arg)
#define VMW_GB_SURFACE_CREATE SVGA_VMW_IOWR(GB_SURFACE_CREATE, union drm_vmw_gb_surface_create_arg)
#define VMW_GB_SURFACE_REF SVGA_VMW_IOWR(GB_SURFACE_REF, union drm_vmw_gb_surface_reference_arg)
#define VMW_GB_SURFACE_CREATE_EXT SVGA_VMW_IOWR(GB_SURFACE_CREATE_EXT, union drm_vmw_gb_surface_create_ext_arg)
#define VMW_GB_SURFACE_REF_EXT SVGA_VMW_IOWR(GB_SURFACE_REF_EXT, union drm_vmw_gb_surface_reference_ext_arg)
#define VMW_CREATE_SHADER SVGA_VMW_IOWR(CREATE_SHADER, struct drm_vmw_shader_create_arg)
#define VMW_UNREF_SHADER SVGA_VMW_IOW(UNREF_SHADER, struct drm_vmw_shader_arg)
#define VMW_EXECBUF SVGA_VMW_IOW(EXECBUF, struct drm_vmw_execbuf_arg)
#define VMW_GET_3D_CAP SVGA_VMW_IOW(GET_3D_CAP, struct drm_vmw_get_3d_cap_arg)
#define VMW_FENCE_WAIT SVGA_VMW_IOWR(FENCE_WAIT, struct drm_vmw_fence_wait_arg)
#define VMW_FENCE_SIGNALED SVGA_VMW_IOWR(FENCE_SIGNALED, struct drm_vmw_fence_signaled_arg)
#define VMW_FENCE_UNREF SVGA_VMW_IOW(FENCE_UNREF, struct drm_vmw_fence_arg)
#define VMW_FENCE_EVENT SVGA_VMW_IOW(FENCE_EVENT, struct drm_vmw_fence_event_arg)
#define VMW_SYNCCPU SVGA_VMW_IOW(SYNCCPU, struct drm_vmw_synccpu_arg)
#define VMW_PRESENT SVGA_VMW_IOW(PRESENT, struct drm_vmw_present_arg)
#define VMW_PRESENT_READBACK SVGA_VMW_IOW(PRESENT_READBACK, struct drm_vmw_present_readback_arg)

struct svga_render_file;
struct svga_batch;
struct svga_dx_state;

enum svga_object_type { SVGA_CONTEXT, SVGA_SURFACE, SVGA_SHADER, SVGA_FENCE };
enum svga_context_type { SVGA_CONTEXT_HOST, SVGA_CONTEXT_GB, SVGA_CONTEXT_DX };

/* Object handles are global cookies, unrelated to reusable device IDs.
 * Access requires the owning OFD and the correct object type. */
struct svga_object {
    struct svga_object *hash_next, *owner_next, **owner_previous;
    struct svga_render_file *owner;
    enum svga_object_type type;
    uint64_t validation_serial;
    uint32_t handle, id, references, handle_refs;
    bool published;
};

struct svga_context {
    struct svga_object object;
    enum svga_context_type type;
    struct svga_context *lru_prev, *lru_next;
    struct drm_bo *state_bo; /* Retained across eviction, never exported. */
    struct svga_region *state;
    struct svga_dx_state *dx;
};

struct svga_surface {
    struct svga_object object;
    struct drm_vmw_gb_surface_create_ext_req info;
    struct svga_region *backing;
    struct drm_bo *bo; /* held even when currently not mapped to a MOB */
    struct svga_surface *bo_next, **bo_link;
    struct svga_surface *lru_prev, *lru_next;
    /* Temporary snapshot while the execution writer validates a batch. */
    struct drm_bo *validation_bo;
    uint64_t uploaded_epoch, dirty_serial, validation_dirty, validation_upload;
    struct drm_master *creator_master;
    struct task_file *prime_description; /* weak cache, cleared before the fd drops its ref */
    uint32_t bytes, offset, face_count, image_count;
    uint32_t levels[DRM_VMW_MAX_SURFACE_FACES];
    bool gb, needs_rebind;
    SVGA3dSize images[];
};

struct svga_shader {
    struct svga_object object;
    struct svga_region *backing;
    uint32_t type, bytes, offset;
};

struct svga_fence {
    struct svga_object object;
    uint32_t sequence;
    int status; /* zero pending, one passed, or negative terminal errno */
};

struct svga_render_file {
    struct svga_object *objects;
    struct svga_surface_ref *imports;
    unsigned object_count;
    bool gb_aware;
    struct svga_cpu_grab *grabs;
    struct svga_fence_event *fence_events;
};

/* DRM execution ownership spans validation, relocation, and queue commit. */
struct svga_render_file *svga_render_file_get(struct drm_file *file);
struct svga_object *svga_object_create(struct drm_file *file, enum svga_object_type type,
                                       size_t bytes);
struct svga_object *svga_object_lookup(const struct drm_file *file, enum svga_object_type type,
                                       uint32_t handle);
void svga_object_get(struct svga_object *object);
void svga_object_put(struct svga_object *object);
int svga_object_unref(struct drm_file *file, enum svga_object_type type, uint32_t handle);
int svga_surface_ref(struct drm_file *file, struct svga_surface *surface);
struct svga_surface *svga_surface_lookup_reference(struct drm_file *file, uint32_t handle);
/* Update the weak BO-to-surface index. The caller transfers both the new BO
 * reference and the old reference's disposal. Requires execution ownership. */
void svga_surface_set_bo(struct svga_surface *surface, struct drm_bo *bo);
/* Ensure the current BO is bound before emitting commands that use the surface.
 * The caller already owns a GPU reservation on that BO. */
int svga_surface_validate_residency(struct svga_surface *surface);
/* Queue detaches and subtract their pages from the outstanding demand. */
int svga_surface_reclaim_mobs(uint32_t *pages);
int svga_render_reclaim_mobs(uint32_t pages);
bool svga_surface_is_coherent(const struct svga_surface *surface);
bool svga_bo_is_coherent(const struct drm_bo *bo);
int svga_surface_cpu_access(struct drm_bo *bo);
/* Flush deferred legacy query result stores before destroying their context. */
void svga_query_release(const struct svga_context *context);
void svga_render_file_release(struct drm_file *file);
void svga_render_rollback(struct drm_file *file, uint32_t command, const void *data, uint32_t object);

void svga_render_init(void);
void svga_execbuf_fini(void);
bool svga_render_supported(void);
bool svga_render_gb(void);
unsigned svga_render_shader_model(void); /* 0 for legacy, 40 through 51 for DX */
uint32_t svga_render_max_surface_size(void);
uint32_t svga_render_max_texture_dim(unsigned axis);
int svga_render_param(struct drm_file *file, struct drm_vmw_getparam_arg *arg);
int svga_render_caps(struct task *task, struct drm_file *file, const struct drm_vmw_get_3d_cap_arg *arg);

int svga_context_create(struct drm_file *file, bool dx, struct drm_vmw_context_arg *reply);
int svga_context_publish(struct svga_context *context);
int svga_context_validate_residency(struct svga_context *context);
int svga_surface_ioctl(struct task *task, struct drm_file *file, uint32_t command,
                       void *data, uint32_t *bo_handle, uint32_t *surface_handle);
int svga_prime_export(struct task *task, struct drm_file *file, struct drm_prime_handle *arg);
int svga_prime_import(struct drm_file *file, void *object, uint32_t *handle);
struct svga_shader *svga_shader_lookup(struct drm_file *file, uint32_t handle);
int svga_shader_inline(uint32_t type, const void *code, uint32_t bytes, struct svga_shader **out);
int svga_shader_create(struct drm_file *file, struct drm_vmw_shader_create_arg *arg);
void svga_context_release_backing(struct svga_context *context);
const SVGA3dSize *svga_surface_image(const struct svga_surface *surface, uint32_t face, uint32_t mip,
                                     SVGA3dSize *temporary);
int svga_surface_dma_check(const struct svga_surface *surface, const SVGA3dSize *image,
                           const SVGA3dCopyBox *box, uint32_t pitch, uint32_t maximum_offset);

int svga_surface_transfer_check(const struct svga_surface *surface, const SVGA3dSize *image,
                                const SVGA3dBox *box, uint32_t pitch, uint32_t slice_pitch,
                                uint32_t bytes);

int svga_execbuf(struct task *task, struct drm_file *file, const struct drm_vmw_execbuf_arg *arg);
/* One in-order display submission, with render-style retirement of operands. */
int svga_display_submit(const void *commands, uint32_t bytes, struct svga_surface *source,
                        struct svga_surface *target, struct drm_bo *bo, uint32_t *sequence);
int svga_buffer_wait(struct drm_bo *bo);
void svga_fence_fill(const struct svga_fence *fence, struct drm_vmw_fence_rep *rep);
int svga_fence_ioctl(struct task *task, struct drm_file *file, uint32_t command, void *data);
int svga_synccpu(struct task *task, struct drm_file *file, const struct drm_vmw_synccpu_arg *arg);
void svga_cpu_grabs_release(struct svga_render_file *state);
uint32_t svga_render_retire(uint32_t budget);

static inline int svga_import(struct task *task, void *data, uint64_t address, uint32_t bytes)
{
#ifdef CONFIG_SWAP
    return drm_user_import(task, data, address, bytes);
#else
    return user_copy_from_task(task, data, address, bytes) < 0 ? -EFAULT : 0;
#endif
}

static inline int svga_prepare_output(struct task *task, uint64_t address, uint32_t bytes)
{
#ifdef CONFIG_SWAP
    int ret = drm_user_prepare(task, address, bytes, true);
    if (ret)
        return ret;
#else
    (void)task;
#endif
    return user_range_writable(address, bytes) ? 0 : -EFAULT;
}

#endif
