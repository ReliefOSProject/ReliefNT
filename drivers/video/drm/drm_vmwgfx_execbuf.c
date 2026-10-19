#include "drm_internal.h"

#include <drm/vmwgfx_drm.h>
#include <linux/errno.h>
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <reliefnt/futex.h>
#include <reliefnt/panic.h>
#include <reliefnt/sched.h>
#include <reliefnt/time.h>
#include <reliefnt/usercopy.h>

#include "../svga/svga_protocol.h"
#include "drm_vmwgfx_execbuf_parser.h"

#define DRM_VMW_EXECBUF_V1_SHORT_SIZE \
    __builtin_offsetof(struct drm_vmw_execbuf_arg, context_handle)
#define DRM_VMW_FENCE_MAX_GLOBAL 128u
#define DRM_VMW_EXEC_WORK_MAX 32u
#define DRM_VMW_EXEC_WORK_BYTES_MAX (8u * 1024u * 1024u)
#define DRM_VMW_EXEC_ATTACHMENTS_MAX 128u
#define DRM_VMW_EXEC_ATTACHMENT_BYTES_MAX (128u * 1024u * 1024u)
#define DRM_VMW_EXEC_CONTEXTS_MAX 64u
#define DRM_VMW_EXEC_SURFACES_MAX 256u
#define DRM_VMW_EXEC_BOS_MAX 128u
#define DRM_VMW_EXEC_OUTPUT_BYTES_MAX (256u * 1024u)
#define DRM_VMW_EXEC_ATTACHMENT_CACHE_TICKS 100u
#define DRM_VMW_FENCE_WAIT_FAST_POLL_US 500u
#define DRM_VMW_FENCE_WAIT_FAST_POLL_SAMPLES 8192u
#define DRM_VMW_FENCE_WAIT_FAST_CLOCK_RESOLUTION_NS 1000

struct drm_vmwgfx_exec_work;

struct drm_vmwgfx_attachment {
    struct drm_vmwgfx_attachment *next;
    struct drm_bo *bo;
    svga_handle gmr;
    uint64_t idle_since;
    uint64_t last_progress_tick;
    uint32_t bytes;
    uint32_t mobid;
    uint32_t references;
    bool detaching;
    bool ready;
    int error;
};

struct drm_vmwgfx_context_ref {
    struct drm_vmwgfx_context *object;
    uint32_t drm_id;
    uint32_t backend_id;
    svga_handle backend;
};

struct drm_vmwgfx_surface_ref {
    struct drm_vmwgfx_surface *object;
    struct drm_vmwgfx_surface_metadata metadata;
    uint32_t drm_id;
    uint32_t backend_id;
    svga_handle backend;
};

struct drm_vmwgfx_bo_ref {
    struct drm_bo *object;
    struct drm_vmwgfx_attachment *attachment;
    uint32_t drm_id;
};

struct drm_vmwgfx_exec_work {
    struct drm_vmwgfx_exec_work *next;
    struct drm_file *preparing_file;
    uint8_t *snapshot;
    uint8_t *scratch;
    uint32_t *output;
    uint32_t source_bytes;
    uint32_t packet_count;
    uint32_t output_capacity;
    uint32_t context_count, surface_count, bo_count;
    uint32_t allocation_bytes;
    struct drm_vmwgfx_context_ref contexts[DRM_VMW_EXEC_CONTEXTS_MAX];
    struct drm_vmwgfx_surface_ref surfaces[DRM_VMW_EXEC_SURFACES_MAX];
    struct drm_vmwgfx_bo_ref bos[DRM_VMW_EXEC_BOS_MAX];
    struct svga_batch batch;
    struct drm_vmwgfx_fence *fence_object;
    svga_handle fence;
    bool batch_open;
    bool submitted;
    bool reset_pending;
    bool cleanup_queued;
};

enum drm_vmwgfx_continuation_kind {
    DRM_VMW_CONT_FENCE_WAIT = 1,
    DRM_VMW_CONT_SYNCCPU = 2,
    DRM_VMW_CONT_FENCE_REPLY_FAULT = 3,
};

struct drm_vmwgfx_task_continuation {
    struct drm_vmwgfx_task_continuation *next;
    enum drm_vmwgfx_continuation_kind kind;
    struct drm_file *file;
    struct task_file *syscall_file;
    int32_t syscall_fd;
    uint32_t syscall_number;
    uint64_t user_argument;
    uint64_t deadline;
    uint32_t command;
    uint32_t snapshot_bytes;
    union {
        struct drm_vmw_fence_wait_arg wait;
        struct drm_vmw_synccpu_arg synccpu;
    } snapshot;
    struct drm_bo *bo;
    struct drm_vmwgfx_fence *fence_object;
    svga_handle fence;
};

struct drm_vmwgfx_fence {
    struct drm_vmwgfx_fence *next;
    uint32_t references;
    uint32_t handle;
    svga_handle token;
    bool user_linked;
};

static struct kernel_spinlock drm_vmwgfx_fence_lock = KERNEL_SPINLOCK_INIT;
static uint32_t drm_vmwgfx_fence_count;
static struct kernel_spinlock drm_vmwgfx_exec_lock = KERNEL_SPINLOCK_INIT;
static struct drm_vmwgfx_attachment *drm_vmwgfx_attachments;
static struct drm_vmwgfx_exec_work *drm_vmwgfx_exec_jobs;
static struct drm_vmwgfx_exec_work *drm_vmwgfx_exec_jobs_tail;
static struct drm_vmwgfx_exec_work *drm_vmwgfx_exec_cleanup;
static struct drm_vmwgfx_task_continuation *drm_vmwgfx_continuation_cleanup;
static uint32_t drm_vmwgfx_exec_work_count;
static uint32_t drm_vmwgfx_exec_work_bytes;
static uint32_t drm_vmwgfx_attachment_count;
static uint64_t drm_vmwgfx_attachment_bytes;
static uint32_t drm_vmwgfx_attachment_progress_cursor;

static void drm_vmwgfx_execdiag(
    struct drm_faildiag_context *diagnostics, int error, const char *stage,
    uint32_t source_bytes, uint32_t output_bytes, uint32_t output_capacity,
    uint32_t contexts, uint32_t surfaces, uint32_t bos)
{
    if (!drm_faildiag_is_enabled()) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    uint32_t work_count = drm_vmwgfx_exec_work_count;
    uint32_t work_bytes = drm_vmwgfx_exec_work_bytes;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    const struct drm_faildiag_field fields[] = {
        {"source", source_bytes},
        {"output", output_bytes},
        {"capacity", output_capacity},
        {"work_count", work_count},
        {"work_bytes", work_bytes},
        {"contexts", contexts},
        {"surfaces", surfaces},
        {"bos", bos},
    };
    drm_faildiag_record(diagnostics, error, stage, fields,
                        sizeof(fields) / sizeof(fields[0]));
}

static void drm_vmwgfx_exec_submit_diag(
    struct drm_faildiag_context *diagnostics, int error, const char *stage,
    uint32_t source_bytes, uint32_t output_bytes, uint32_t output_capacity,
    const struct svga_batch_submit_failure *failure)
{
    if (!drm_faildiag_is_enabled()) return;
    const struct drm_faildiag_field fields[] = {
        {"source", source_bytes},
        {"output", output_bytes},
        {"capacity", output_capacity},
        {"reason", failure ? failure->reason : 0},
        {"index", failure ? failure->index : SVGA_BATCH_SUBMIT_INDEX_NONE},
        {"detail0", failure ? failure->detail0 : 0},
        {"detail1", failure ? failure->detail1 : 0},
        {"detail2", failure ? failure->detail2 : 0},
    };
    drm_faildiag_record(diagnostics, error, stage, fields,
                        sizeof(fields) / sizeof(fields[0]));
}

static void drm_vmwgfx_attachment_diag(
    struct drm_faildiag_context *diagnostics, int error, const char *stage,
    uint32_t source_bytes, uint32_t output_capacity, uint32_t output_bytes,
    uint32_t bo_refs)
{
    if (!drm_faildiag_is_enabled()) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    uint32_t work_count = drm_vmwgfx_exec_work_count;
    uint32_t work_bytes = drm_vmwgfx_exec_work_bytes;
    uint32_t attachment_count = drm_vmwgfx_attachment_count;
    uint64_t attachment_bytes = drm_vmwgfx_attachment_bytes;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    const struct drm_faildiag_field fields[] = {
        {"source", source_bytes},
        {"output_capacity", output_capacity},
        {"output", output_bytes},
        {"work_count", work_count},
        {"work_bytes", work_bytes},
        {"attachments", attachment_count},
        {"attachment_bytes", attachment_bytes},
        {"bo_refs", bo_refs},
    };
    drm_faildiag_record(diagnostics, error, stage, fields,
                        sizeof(fields) / sizeof(fields[0]));
}

static void drm_vmwgfx_fence_diag(
    struct drm_faildiag_context *diagnostics, int error, const char *stage,
    uint32_t source_bytes, uint32_t output_bytes, uint32_t output_capacity)
{
    if (!drm_faildiag_is_enabled()) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    uint32_t work_count = drm_vmwgfx_exec_work_count;
    uint32_t work_bytes = drm_vmwgfx_exec_work_bytes;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    uint32_t fence_count = drm_vmwgfx_fence_count;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
    const struct drm_faildiag_field fields[] = {
        {"source", source_bytes},
        {"output", output_bytes},
        {"capacity", output_capacity},
        {"work_count", work_count},
        {"work_bytes", work_bytes},
        {"fences", fence_count},
    };
    drm_faildiag_record(diagnostics, error, stage, fields,
                        sizeof(fields) / sizeof(fields[0]));
}

static void drm_vmwgfx_exec_job_enqueue_locked(
    struct drm_vmwgfx_exec_work *work)
{
    work->next = NULL;
    if (drm_vmwgfx_exec_jobs_tail)
        drm_vmwgfx_exec_jobs_tail->next = work;
    else
        drm_vmwgfx_exec_jobs = work;
    drm_vmwgfx_exec_jobs_tail = work;
}

static void drm_vmwgfx_continuation_release(
    struct drm_vmwgfx_task_continuation *continuation);

static struct drm_vmwgfx_fence *drm_vmwgfx_fence_find_locked(
    struct drm_file *file, uint32_t handle)
{
    for (struct drm_vmwgfx_fence *fence = file
            ? (struct drm_vmwgfx_fence *)file->vmwgfx_fences : NULL;
         fence; fence = fence->next)
        if (fence->handle == handle) return fence;
    return NULL;
}

static uint32_t drm_vmwgfx_fence_handle_locked(struct drm_file *file)
{
    uint32_t first = file->next_fence_handle;
    if (!first) first = 1;
    uint32_t candidate = first;
    do {
        if (!drm_vmwgfx_fence_find_locked(file, candidate)) {
            file->next_fence_handle = candidate + 1u;
            if (!file->next_fence_handle) file->next_fence_handle = 1;
            return candidate;
        }
        ++candidate;
        if (!candidate) candidate = 1;
    } while (candidate != first);
    return 0;
}

static void drm_vmwgfx_fence_put(struct drm_vmwgfx_fence *fence)
{
    if (!fence) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    bool free_fence = fence->references && --fence->references == 0;
    if (free_fence && drm_vmwgfx_fence_count) --drm_vmwgfx_fence_count;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
    if (free_fence) kernel_free(fence);
}

static void drm_vmwgfx_fence_get(struct drm_vmwgfx_fence *fence)
{
    if (!fence) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    if (fence->references != UINT32_MAX) ++fence->references;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
}

static struct drm_vmwgfx_fence *drm_vmwgfx_fence_reserve(
    struct drm_file *file, bool user_handle, const char **failure_stage)
{
    if (failure_stage) *failure_stage = "exec.fence.heap";
    struct drm_vmwgfx_fence *fence = kernel_malloc(sizeof(*fence));
    if (!fence) return NULL;
    *fence = (struct drm_vmwgfx_fence){.references = 1};
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    if (drm_vmwgfx_fence_count >= DRM_VMW_FENCE_MAX_GLOBAL) {
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
        kernel_free(fence);
        if (failure_stage) *failure_stage = "exec.fence.quota";
        return NULL;
    }
    if (user_handle) fence->handle = drm_vmwgfx_fence_handle_locked(file);
    if (user_handle && !fence->handle) {
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
        kernel_free(fence);
        if (failure_stage) *failure_stage = "exec.fence.handle";
        return NULL;
    }
    ++drm_vmwgfx_fence_count;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
    if (failure_stage) *failure_stage = NULL;
    return fence;
}

static void drm_vmwgfx_fence_publish(struct drm_file *file,
                                     struct drm_vmwgfx_fence *fence)
{
    if (!file || !fence || !fence->handle) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    fence->references++;
    fence->user_linked = true;
    fence->next = (struct drm_vmwgfx_fence *)file->vmwgfx_fences;
    file->vmwgfx_fences = fence;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
}

static struct drm_vmwgfx_fence *drm_vmwgfx_fence_lookup_get(
    struct drm_file *file, uint32_t handle)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    struct drm_vmwgfx_fence *fence = drm_vmwgfx_fence_find_locked(file, handle);
    if (fence) ++fence->references;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
    return fence;
}

/**
 * @brief Remove a fence handle, optionally requiring its retained object.
 * @param file DRM file whose user-visible fence list is searched.
 * @param handle User-visible fence handle to remove.
 * @param expected If non-NULL, the handle must still name this retained fence.
 * @return True if the matching user-visible reference was removed.
 */
static bool drm_vmwgfx_fence_remove_retained(
    struct drm_file *file, uint32_t handle,
    struct drm_vmwgfx_fence *expected)
{
    struct drm_vmwgfx_fence *removed = NULL;
    bool found = false;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    struct drm_vmwgfx_fence **link = file
        ? (struct drm_vmwgfx_fence **)&file->vmwgfx_fences : NULL;
    while (link && *link &&
           ((*link)->handle != handle || (expected && *link != expected)))
        link = &(*link)->next;
    if (link && *link) {
        found = true;
        removed = *link;
        *link = removed->next;
        removed->next = NULL;
        removed->user_linked = false;
        if (removed->references && --removed->references == 0) {
            if (drm_vmwgfx_fence_count) --drm_vmwgfx_fence_count;
        } else {
            removed = NULL;
        }
    }
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
    if (removed) kernel_free(removed);
    return found;
}

static bool drm_vmwgfx_fence_remove(struct drm_file *file, uint32_t handle)
{
    return drm_vmwgfx_fence_remove_retained(file, handle, NULL);
}

static int drm_vmwgfx_map_svga_status(int status)
{
    switch (status) {
    case 0: return 0;
    case SVGA_EBUSY: return -LINUX_EBUSY;
    case SVGA_ENOMEM: return -LINUX_ENOMEM;
    case SVGA_ENODEV: return -LINUX_ENODEV;
    case SVGA_ENOTSUP: return -LINUX_EOPNOTSUPP;
    case SVGA_ETIMEDOUT: return -LINUX_ETIMEDOUT;
    case SVGA_EINVAL: return -LINUX_EINVAL;
    default: return -LINUX_EIO;
    }
}

static bool drm_vmwgfx_add_u64(uint64_t left, uint64_t right,
                               uint64_t *result)
{
    if (!result || left > UINT64_MAX - right) return false;
    *result = left + right;
    return true;
}

static bool drm_vmwgfx_mul_u64(uint64_t left, uint64_t right,
                               uint64_t *result)
{
    if (!result || (left && right > UINT64_MAX / left)) return false;
    *result = left * right;
    return true;
}

static struct drm_vmwgfx_context_ref *drm_vmwgfx_exec_find_context(
    struct drm_vmwgfx_exec_work *work, uint32_t drm_id)
{
    for (uint32_t i = 0; i < work->context_count; ++i)
        if (work->contexts[i].drm_id == drm_id) return &work->contexts[i];
    return NULL;
}

static struct drm_vmwgfx_surface_ref *drm_vmwgfx_exec_find_surface(
    struct drm_vmwgfx_exec_work *work, uint32_t drm_id)
{
    for (uint32_t i = 0; i < work->surface_count; ++i)
        if (work->surfaces[i].drm_id == drm_id) return &work->surfaces[i];
    return NULL;
}

static struct drm_vmwgfx_bo_ref *drm_vmwgfx_exec_find_bo(
    struct drm_vmwgfx_exec_work *work, uint32_t drm_id)
{
    for (uint32_t i = 0; i < work->bo_count; ++i)
        if (work->bos[i].drm_id == drm_id) return &work->bos[i];
    return NULL;
}

static int drm_vmwgfx_exec_hold_context(struct drm_vmwgfx_exec_work *work,
                                         struct drm_file *file,
                                         uint32_t drm_id,
                                         struct drm_vmwgfx_context_ref **out)
{
    if (!drm_id || drm_id == SVGA3D_INVALID_ID) return -LINUX_EINVAL;
    struct drm_vmwgfx_context_ref *entry =
        drm_vmwgfx_exec_find_context(work, drm_id);
    if (entry) { if (out) *out = entry; return 0; }
    if (work->context_count >= DRM_VMW_EXEC_CONTEXTS_MAX)
        return -LINUX_ENOMEM;
    struct drm_vmwgfx_context *object =
        drm_vmwgfx_context_lookup_get(file, drm_id);
    if (!object) return -LINUX_EINVAL;
    svga_handle backend = drm_vmwgfx_context_backend_handle(object);
    uint32_t backend_id = drm_vmwgfx_context_backend_id(object);
    if (!backend || backend_id == SVGA3D_INVALID_ID) {
        drm_vmwgfx_context_put(object);
        return -LINUX_EINVAL;
    }
    entry = &work->contexts[work->context_count++];
    *entry = (struct drm_vmwgfx_context_ref){
        .object = object, .drm_id = drm_id,
        .backend_id = backend_id, .backend = backend,
    };
    if (out) *out = entry;
    return 0;
}

static int drm_vmwgfx_exec_hold_surface(struct drm_vmwgfx_exec_work *work,
                                         struct drm_file *file,
                                         uint32_t drm_id,
                                         uint32_t face, uint32_t mip,
                                         struct drm_vmwgfx_surface_ref **out)
{
    if (!drm_id || drm_id == SVGA3D_INVALID_ID) return -LINUX_EINVAL;
    struct drm_vmwgfx_surface_ref *entry =
        drm_vmwgfx_exec_find_surface(work, drm_id);
    if (!entry) {
        if (work->surface_count >= DRM_VMW_EXEC_SURFACES_MAX)
            return -LINUX_ENOMEM;
        struct drm_vmwgfx_surface *object =
            drm_vmwgfx_surface_lookup_get(file, drm_id);
        if (!object) return -LINUX_EINVAL;
        entry = &work->surfaces[work->surface_count++];
        *entry = (struct drm_vmwgfx_surface_ref){.object = object,
            .drm_id = drm_id,
            .backend = drm_vmwgfx_surface_backend_handle(object),
            .backend_id = drm_vmwgfx_surface_backend_id(object)};
        if (!entry->backend || entry->backend_id == SVGA3D_INVALID_ID ||
            drm_vmwgfx_surface_get_metadata(object, &entry->metadata) != 0) {
            drm_vmwgfx_surface_put(object);
            *entry = (struct drm_vmwgfx_surface_ref){0};
            --work->surface_count;
            return -LINUX_EINVAL;
        }
    }
    struct drm_vmwgfx_surface_subresource subresource;
    if (drm_vmwgfx_surface_get_subresource(entry->object, face, mip,
                                           &subresource) != 0)
        return -LINUX_EINVAL;
    if (out) *out = entry;
    return 0;
}

static int drm_vmwgfx_exec_hold_bo(struct drm_vmwgfx_exec_work *work,
                                    struct drm_file *file, uint32_t drm_id,
                                    struct drm_vmwgfx_bo_ref **out)
{
    if (!drm_id || drm_id >= 0x01000000u) return -LINUX_EINVAL;
    struct drm_vmwgfx_bo_ref *entry =
        drm_vmwgfx_exec_find_bo(work, drm_id);
    if (entry) { if (out) *out = entry; return 0; }
    if (work->bo_count >= DRM_VMW_EXEC_BOS_MAX) return -LINUX_ENOMEM;
    struct drm_bo *object = drm_bo_lookup_handle_get(file, drm_id);
    if (!object) return -LINUX_EINVAL;
    int ret = drm_bo_gpu_reserve(object);
    if (ret) {
        drm_bo_put(object);
        return ret;
    }
    entry = &work->bos[work->bo_count++];
    *entry = (struct drm_vmwgfx_bo_ref){.object = object, .drm_id = drm_id};
    if (out) *out = entry;
    return 0;
}

static uint32_t drm_vmwgfx_surface_element_bytes(uint32_t type)
{
    switch (type) {
    case SVGA3D_DECLTYPE_FLOAT1: return 4;
    case SVGA3D_DECLTYPE_FLOAT2: return 8;
    case SVGA3D_DECLTYPE_FLOAT3: return 12;
    case SVGA3D_DECLTYPE_FLOAT4: return 16;
    case SVGA3D_DECLTYPE_D3DCOLOR:
    case SVGA3D_DECLTYPE_UBYTE4:
    case SVGA3D_DECLTYPE_UBYTE4N:
    case SVGA3D_DECLTYPE_SHORT2:
    case SVGA3D_DECLTYPE_SHORT2N:
    case SVGA3D_DECLTYPE_USHORT2N:
    case SVGA3D_DECLTYPE_UDEC3:
    case SVGA3D_DECLTYPE_DEC3N:
    case SVGA3D_DECLTYPE_FLOAT16_2: return 4;
    case SVGA3D_DECLTYPE_SHORT4:
    case SVGA3D_DECLTYPE_SHORT4N:
    case SVGA3D_DECLTYPE_USHORT4N:
    case SVGA3D_DECLTYPE_FLOAT16_4: return 8;
    default: return 0;
    }
}

static bool drm_vmwgfx_exec_surface_is_buffer(
    const struct drm_vmwgfx_surface_ref *surface)
{
    return surface && surface->metadata.desc.format == SVGA3D_BUFFER &&
           surface->metadata.desc.width != 0;
}

static int drm_vmwgfx_exec_validate_array(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const SVGA3dArray *array, uint32_t type)
{
    struct drm_vmwgfx_surface_ref *surface;
    int ret = drm_vmwgfx_exec_hold_surface(work, file, array->surfaceId,
                                            0, 0, &surface);
    if (ret) return ret;
    if (!drm_vmwgfx_exec_surface_is_buffer(surface) || !array->stride)
        return -LINUX_EINVAL;
    uint32_t element = drm_vmwgfx_surface_element_bytes(type);
    uint64_t size = surface->metadata.desc.width;
    uint64_t end;
    if (!element || array->stride < element || array->offset > size ||
        !drm_vmwgfx_add_u64(array->offset, element, &end) || end > size)
        return -LINUX_EINVAL;
    return 0;
}

static bool drm_vmwgfx_exec_has_context(uint32_t command)
{
    return command != SVGA_3D_CMD_SURFACE_COPY &&
           command != SVGA_3D_CMD_SURFACE_STRETCHBLT &&
           command != SVGA_3D_CMD_SURFACE_DMA;
}

static int drm_vmwgfx_exec_surface_image(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const SVGA3dSurfaceImageId *image,
    struct drm_vmwgfx_surface_ref **surface_out,
    struct drm_vmwgfx_surface_subresource *subresource_out)
{
    if (!image || !surface_out || !subresource_out) return -LINUX_EINVAL;
    struct drm_vmwgfx_surface_ref *surface;
    int ret = drm_vmwgfx_exec_hold_surface(work, file, image->sid,
        image->face, image->mipmap, &surface);
    if (ret) return ret;
    ret = drm_vmwgfx_surface_get_subresource(surface->object, image->face,
                                              image->mipmap, subresource_out);
    if (ret) return -LINUX_EINVAL;
    *surface_out = surface;
    return 0;
}

static bool drm_vmwgfx_exec_box_fits(uint32_t x, uint32_t y, uint32_t z,
                                     uint32_t width, uint32_t height,
                                     uint32_t depth,
                                     const struct drm_vmwgfx_surface_subresource *size)
{
    return size && width && height && depth && x <= size->width &&
           width <= size->width - x && y <= size->height &&
           height <= size->height - y && z <= size->depth &&
           depth <= size->depth - z;
}

static uint32_t drm_vmwgfx_exec_format_bytes(uint32_t format)
{
    switch (format) {
    case SVGA3D_BUFFER: return 1;
    case SVGA3D_ALPHA8: return 1;
    case SVGA3D_Z_D16: return 2;
    case SVGA3D_X8R8G8B8:
    case SVGA3D_A8R8G8B8:
    case SVGA3D_Z_D24S8: return 4;
    default: return 0;
    }
}

static int drm_vmwgfx_exec_validate_surface_copy(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const uint8_t *body, uint32_t bytes)
{
    SVGA3dCmdSurfaceCopy copy;
    __builtin_memcpy(&copy, body, sizeof(copy));
    struct drm_vmwgfx_surface_ref *source, *destination;
    struct drm_vmwgfx_surface_subresource source_size, destination_size;
    int ret = drm_vmwgfx_exec_surface_image(work, file, &copy.src,
                                             &source, &source_size);
    if (ret) return ret;
    ret = drm_vmwgfx_exec_surface_image(work, file, &copy.dest,
                                       &destination, &destination_size);
    if (ret) return ret;
    if (source->metadata.desc.format != destination->metadata.desc.format)
        return -LINUX_EINVAL;
    uint32_t count = (bytes - sizeof(copy)) / sizeof(SVGA3dCopyBox);
    const SVGA3dCopyBox *boxes =
        (const SVGA3dCopyBox *)(body + sizeof(copy));
    for (uint32_t i = 0; i < count; ++i) {
        const SVGA3dCopyBox *box = &boxes[i];
        if (!drm_vmwgfx_exec_box_fits(box->srcx, box->srcy, box->srcz,
                box->w, box->h, box->d, &source_size) ||
            !drm_vmwgfx_exec_box_fits(box->x, box->y, box->z,
                box->w, box->h, box->d, &destination_size))
            return -LINUX_EINVAL;
    }
    return 0;
}

static int drm_vmwgfx_exec_validate_stretch(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const uint8_t *body)
{
    SVGA3dCmdSurfaceStretchBlt stretch;
    __builtin_memcpy(&stretch, body, sizeof(stretch));
    if (stretch.mode >= SVGA3D_STRETCH_BLT_MAX) return -LINUX_EINVAL;
    struct drm_vmwgfx_surface_ref *source, *destination;
    struct drm_vmwgfx_surface_subresource source_size, destination_size;
    int ret = drm_vmwgfx_exec_surface_image(work, file, &stretch.src,
                                             &source, &source_size);
    if (ret) return ret;
    ret = drm_vmwgfx_exec_surface_image(work, file, &stretch.dest,
                                       &destination, &destination_size);
    if (ret) return ret;
    (void)source;
    (void)destination;
    if (!drm_vmwgfx_exec_box_fits(stretch.boxSrc.x, stretch.boxSrc.y,
            stretch.boxSrc.z, stretch.boxSrc.w, stretch.boxSrc.h,
            stretch.boxSrc.d, &source_size) ||
        !drm_vmwgfx_exec_box_fits(stretch.boxDest.x, stretch.boxDest.y,
            stretch.boxDest.z, stretch.boxDest.w, stretch.boxDest.h,
            stretch.boxDest.d, &destination_size))
        return -LINUX_EINVAL;
    return 0;
}

static int drm_vmwgfx_exec_validate_dma(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const uint8_t *body, uint32_t bytes)
{
    SVGA3dCmdSurfaceDMA dma;
    __builtin_memcpy(&dma, body, sizeof(dma));
    uint32_t boxes_bytes = bytes - sizeof(SVGA3dCmdSurfaceDMASuffix);
    SVGA3dCmdSurfaceDMASuffix suffix;
    __builtin_memcpy(&suffix, body + boxes_bytes, sizeof(suffix));
    struct drm_vmwgfx_surface_ref *host;
    struct drm_vmwgfx_surface_subresource host_size;
    int ret = drm_vmwgfx_exec_surface_image(work, file, &dma.host,
                                             &host, &host_size);
    if (ret) return ret;
    uint32_t bytes_per_pixel =
        drm_vmwgfx_exec_format_bytes(host->metadata.desc.format);
    if (!bytes_per_pixel) return -LINUX_EOPNOTSUPP;
    struct drm_vmwgfx_bo_ref *bo;
    ret = drm_vmwgfx_exec_hold_bo(work, file, dma.guest.ptr.gmrId, &bo);
    if (ret) return ret;
    uint64_t bo_size = drm_bo_size(bo->object);
    if (dma.guest.ptr.offset > bo_size) return -LINUX_EINVAL;
    uint64_t available = bo_size - dma.guest.ptr.offset;
    if (suffix.maximumOffset > available)
        suffix.maximumOffset = available > UINT32_MAX
            ? UINT32_MAX : (uint32_t)available;
    uint32_t pitch = dma.guest.pitch;
    if (!pitch) {
        uint64_t tight_pitch;
        if (!drm_vmwgfx_mul_u64(host_size.width, bytes_per_pixel,
                                &tight_pitch) || tight_pitch > UINT32_MAX)
            return -LINUX_EINVAL;
        pitch = (uint32_t)tight_pitch;
    }
    uint32_t count = (boxes_bytes - sizeof(dma)) / sizeof(SVGA3dCopyBox);
    const SVGA3dCopyBox *boxes =
        (const SVGA3dCopyBox *)(body + sizeof(dma));
    for (uint32_t i = 0; i < count; ++i) {
        const SVGA3dCopyBox *box = &boxes[i];
        if (!drm_vmwgfx_exec_box_fits(box->x, box->y, box->z,
                box->w, box->h, box->d, &host_size))
            return -LINUX_EINVAL;
        uint64_t x_end, y_end, z_end, row_bytes;
        if (!drm_vmwgfx_add_u64(box->srcx, box->w, &x_end) ||
            !drm_vmwgfx_add_u64(box->srcy, box->h, &y_end) ||
            !drm_vmwgfx_add_u64(box->srcz, box->d, &z_end) ||
            !drm_vmwgfx_mul_u64(x_end, bytes_per_pixel, &row_bytes) ||
            row_bytes > pitch)
            return -LINUX_EINVAL;
        uint64_t slice_stride, row_index, base_index, byte_end;
        if (!drm_vmwgfx_mul_u64(pitch, host_size.height, &slice_stride) ||
            !drm_vmwgfx_mul_u64(z_end - 1u,
                                slice_stride, &base_index) ||
            !drm_vmwgfx_mul_u64(y_end - 1u, pitch, &row_index) ||
            !drm_vmwgfx_add_u64(base_index, row_index, &row_index) ||
            !drm_vmwgfx_add_u64(row_index, row_bytes, &byte_end) ||
            byte_end > suffix.maximumOffset || byte_end > available)
            return -LINUX_EINVAL;
    }
    return 0;
}

static uint32_t drm_vmwgfx_primitive_vertices(uint32_t type,
                                              uint32_t primitives)
{
    uint64_t count = 0;
    switch (type) {
    case SVGA3D_PRIMITIVE_POINTLIST: count = primitives; break;
    case SVGA3D_PRIMITIVE_LINELIST: count = (uint64_t)primitives * 2u; break;
    case SVGA3D_PRIMITIVE_TRIANGLELIST:
        count = (uint64_t)primitives * 3u;
        break;
    case SVGA3D_PRIMITIVE_LINESTRIP:
        count = primitives ? (uint64_t)primitives + 1u : 0;
        break;
    case SVGA3D_PRIMITIVE_TRIANGLESTRIP:
    case SVGA3D_PRIMITIVE_TRIANGLEFAN:
        count = primitives ? (uint64_t)primitives + 2u : 0;
        break;
    default: return UINT32_MAX;
    }
    return count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
}

static bool drm_vmwgfx_surface_has_hint(
    const struct drm_vmwgfx_surface_ref *surface, uint32_t hint)
{
    return surface && (surface->metadata.desc.flags & hint) != 0;
}

static int drm_vmwgfx_exec_validate_draw(
    struct drm_vmwgfx_exec_work *work, struct drm_file *file,
    const uint8_t *body)
{
    SVGA3dCmdDrawPrimitives header;
    __builtin_memcpy(&header, body, sizeof(header));
    const SVGA3dVertexDecl *decls =
        (const SVGA3dVertexDecl *)(body + sizeof(header));
    const SVGA3dPrimitiveRange *ranges =
        (const SVGA3dPrimitiveRange *)(decls + header.numVertexDecls);
    for (uint32_t i = 0; i < header.numRanges; ++i) {
        const SVGA3dPrimitiveRange *range = &ranges[i];
        uint32_t vertices = drm_vmwgfx_primitive_vertices(
            range->primType, range->primitiveCount);
        if (vertices == UINT32_MAX) return -LINUX_EINVAL;
        if (range->indexArray.surfaceId == SVGA3D_INVALID_ID) {
            if (range->indexWidth) return -LINUX_EINVAL;
            continue;
        }
        if (range->indexWidth != 2u && range->indexWidth != 4u)
            return -LINUX_EINVAL;
        struct drm_vmwgfx_surface_ref *index_surface;
        int ret = drm_vmwgfx_exec_hold_surface(work, file,
            range->indexArray.surfaceId, 0, 0, &index_surface);
        if (ret) return ret;
        if (!drm_vmwgfx_exec_surface_is_buffer(index_surface) ||
            !(index_surface->metadata.desc.flags &
              SVGA3D_SURFACE_HINT_INDEXBUFFER))
            return -LINUX_EINVAL;
        uint64_t index_bytes, end;
        if (!drm_vmwgfx_mul_u64(vertices, range->indexWidth, &index_bytes) ||
            !drm_vmwgfx_add_u64(range->indexArray.offset, index_bytes, &end) ||
            end > index_surface->metadata.desc.width)
            return -LINUX_EINVAL;
    }

    for (uint32_t i = 0; i < header.numVertexDecls; ++i) {
        const SVGA3dVertexDecl *decl = &decls[i];
        int ret = drm_vmwgfx_exec_validate_array(work, file, &decl->array,
                                                  decl->identity.type);
        if (ret) return ret;
        struct drm_vmwgfx_surface_ref *surface =
            drm_vmwgfx_exec_find_surface(work, decl->array.surfaceId);
        if (!drm_vmwgfx_surface_has_hint(surface,
                                         SVGA3D_SURFACE_HINT_VERTEXBUFFER))
            return -LINUX_EINVAL;
        /* For non-indexed draws the index bias is the first vertex. On
         * indexed draws the index bytes are validated above; their values
         * remain guest GPU data and are not scanned without a coherent copy. */
        for (uint32_t r = 0; r < header.numRanges; ++r) {
            const SVGA3dPrimitiveRange *range = &ranges[r];
            if (range->indexArray.surfaceId != SVGA3D_INVALID_ID ||
                !range->primitiveCount)
                continue;
            int64_t first = range->indexBias;
            uint64_t last;
            uint64_t element = drm_vmwgfx_surface_element_bytes(
                decl->identity.type);
            if (first < 0 ||
                !drm_vmwgfx_add_u64((uint64_t)first,
                    drm_vmwgfx_primitive_vertices(range->primType,
                                                   range->primitiveCount) - 1u,
                    &last) ||
                !drm_vmwgfx_mul_u64(last, decl->array.stride, &last) ||
                !drm_vmwgfx_add_u64(decl->array.offset, last, &last) ||
                !drm_vmwgfx_add_u64(last, element, &last) ||
                last > surface->metadata.desc.width)
                return -LINUX_EINVAL;
        }
    }
    return 0;
}

struct drm_vmwgfx_exec_walk {
    struct drm_vmwgfx_exec_work *work;
    struct drm_file *file;
};

static int drm_vmwgfx_exec_validate_packet(uint32_t command,
                                            const uint8_t *body,
                                            uint32_t body_bytes,
                                            uint32_t packet_offset,
                                            void *opaque)
{
    (void)packet_offset;
    struct drm_vmwgfx_exec_walk *walk = opaque;
    struct drm_vmwgfx_exec_work *work = walk->work;
    struct drm_file *file = walk->file;
    if (drm_vmwgfx_exec_has_context(command)) {
        uint32_t cid;
        __builtin_memcpy(&cid, body, sizeof(cid));
        int ret = drm_vmwgfx_exec_hold_context(work, file, cid, NULL);
        if (ret) return ret;
    }

    switch (command) {
    case SVGA_3D_CMD_SURFACE_COPY:
        return drm_vmwgfx_exec_validate_surface_copy(work, file, body,
                                                      body_bytes);
    case SVGA_3D_CMD_SURFACE_STRETCHBLT:
        return drm_vmwgfx_exec_validate_stretch(work, file, body);
    case SVGA_3D_CMD_SURFACE_DMA:
        return drm_vmwgfx_exec_validate_dma(work, file, body, body_bytes);
    case SVGA_3D_CMD_SETRENDERTARGET: {
        SVGA3dCmdSetRenderTarget packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        if (packet.target.sid == SVGA3D_INVALID_ID) return 0;
        struct drm_vmwgfx_surface_ref *surface;
        int ret = drm_vmwgfx_exec_hold_surface(work, file,
            packet.target.sid, packet.target.face, packet.target.mipmap,
            &surface);
        if (ret) return ret;
        uint32_t hint = packet.type == SVGA3D_RT_DEPTH ||
                        packet.type == SVGA3D_RT_STENCIL
            ? SVGA3D_SURFACE_HINT_DEPTHSTENCIL
            : SVGA3D_SURFACE_HINT_RENDERTARGET;
        if (!drm_vmwgfx_surface_has_hint(surface, hint) ||
            surface->metadata.desc.format == SVGA3D_BUFFER)
            return -LINUX_EINVAL;
        return 0;
    }
    case SVGA_3D_CMD_SETTEXTURESTATE: {
        uint32_t count = (body_bytes - sizeof(uint32_t)) /
                         sizeof(SVGA3dTextureState);
        const SVGA3dTextureState *states =
            (const SVGA3dTextureState *)(body + sizeof(uint32_t));
        for (uint32_t i = 0; i < count; ++i) {
            if (states[i].name != SVGA3D_TS_BIND_TEXTURE ||
                states[i].value == SVGA3D_INVALID_ID)
                continue;
            struct drm_vmwgfx_surface_ref *surface;
            int ret = drm_vmwgfx_exec_hold_surface(work, file,
                states[i].value, 0, 0, &surface);
            if (ret) return ret;
            if (!drm_vmwgfx_surface_has_hint(
                    surface, SVGA3D_SURFACE_HINT_TEXTURE) ||
                surface->metadata.desc.format == SVGA3D_BUFFER)
                return -LINUX_EINVAL;
        }
        return 0;
    }
    case SVGA_3D_CMD_DRAW_PRIMITIVES:
        return drm_vmwgfx_exec_validate_draw(work, file, body);
    case SVGA_3D_CMD_SHADER_DEFINE: {
        SVGA3dCmdDefineShader packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.shid < 5000u ? 0 : -LINUX_EINVAL;
    }
    case SVGA_3D_CMD_SHADER_DESTROY: {
        SVGA3dCmdDestroyShader packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.shid < 5000u ? 0 : -LINUX_EINVAL;
    }
    case SVGA_3D_CMD_SET_SHADER: {
        SVGA3dCmdSetShader packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.shid == SVGA3D_INVALID_ID || packet.shid < 5000u
            ? 0 : -LINUX_EINVAL;
    }
    case SVGA_3D_CMD_END_QUERY:
    case SVGA_3D_CMD_WAIT_FOR_QUERY: {
        SVGAGuestPtr result;
        __builtin_memcpy(&result, body + sizeof(uint32_t) * 2u,
                         sizeof(result));
        struct drm_vmwgfx_bo_ref *bo;
        int ret = drm_vmwgfx_exec_hold_bo(work, file, result.gmrId, &bo);
        if (ret) return ret;
        uint64_t end;
        if ((result.offset & 3u) ||
            !drm_vmwgfx_add_u64(result.offset, sizeof(SVGA3dQueryResult),
                                &end) || end > drm_bo_size(bo->object))
            return -LINUX_EINVAL;
        return 0;
    }
    default:
        return 0;
    }
}

static int drm_vmwgfx_exec_translate_packet(uint32_t command,
                                              const uint8_t *body,
                                              uint32_t body_bytes,
                                              uint32_t packet_offset,
                                              void *opaque)
{
    struct drm_vmwgfx_exec_walk *walk = opaque;
    struct drm_vmwgfx_exec_work *work = walk->work;
    uint8_t *translated = work->scratch + packet_offset + 8u;
    const uint8_t *source = body;
    if (body_bytes) __builtin_memcpy(translated, source, body_bytes);
    uint32_t context_id = 0;
    struct drm_vmwgfx_context_ref *context = NULL;
    if (drm_vmwgfx_exec_has_context(command)) {
        __builtin_memcpy(&context_id, translated, sizeof(context_id));
        context = drm_vmwgfx_exec_find_context(work, context_id);
        if (!context) return -LINUX_EINVAL;
        __builtin_memcpy(translated, &context->backend_id,
                         sizeof(context->backend_id));
    }

    switch (command) {
    case SVGA_3D_CMD_SURFACE_COPY: {
        SVGA3dSurfaceImageId images[2];
        __builtin_memcpy(&images[0], translated, sizeof(images[0]));
        __builtin_memcpy(&images[1], translated + sizeof(images[0]),
                         sizeof(images[1]));
        struct drm_vmwgfx_surface_ref *a =
            drm_vmwgfx_exec_find_surface(work, images[0].sid);
        struct drm_vmwgfx_surface_ref *b =
            drm_vmwgfx_exec_find_surface(work, images[1].sid);
        if (!a || !b) return -LINUX_EINVAL;
        images[0].sid = a->backend_id;
        images[1].sid = b->backend_id;
        __builtin_memcpy(translated, images, sizeof(images));
        break;
    }
    case SVGA_3D_CMD_SURFACE_STRETCHBLT: {
        SVGA3dCmdSurfaceStretchBlt packet;
        __builtin_memcpy(&packet, translated, sizeof(packet));
        struct drm_vmwgfx_surface_ref *a =
            drm_vmwgfx_exec_find_surface(work, packet.src.sid);
        struct drm_vmwgfx_surface_ref *b =
            drm_vmwgfx_exec_find_surface(work, packet.dest.sid);
        if (!a || !b) return -LINUX_EINVAL;
        packet.src.sid = a->backend_id;
        packet.dest.sid = b->backend_id;
        __builtin_memcpy(translated, &packet, sizeof(packet));
        break;
    }
    case SVGA_3D_CMD_SURFACE_DMA: {
        SVGA3dCmdSurfaceDMA packet;
        __builtin_memcpy(&packet, translated, sizeof(packet));
        uint32_t boxes_bytes = body_bytes -
                              sizeof(SVGA3dCmdSurfaceDMASuffix);
        SVGA3dCmdSurfaceDMASuffix suffix;
        __builtin_memcpy(&suffix, translated + boxes_bytes,
                         sizeof(suffix));
        struct drm_vmwgfx_surface_ref *host =
            drm_vmwgfx_exec_find_surface(work, packet.host.sid);
        struct drm_vmwgfx_bo_ref *bo =
            drm_vmwgfx_exec_find_bo(work, packet.guest.ptr.gmrId);
        if (!host || !bo || !bo->attachment || !bo->attachment->ready)
            return -LINUX_EINVAL;
        packet.host.sid = host->backend_id;
        packet.guest.ptr.gmrId = svga_resource_id(bo->attachment->gmr);
        if (packet.guest.ptr.gmrId == SVGA3D_INVALID_ID)
            return -LINUX_EINVAL;
        uint64_t bo_size = drm_bo_size(bo->object);
        if (packet.guest.ptr.offset > bo_size) return -LINUX_EINVAL;
        uint64_t available = bo_size - packet.guest.ptr.offset;
        if (suffix.maximumOffset > available)
            suffix.maximumOffset = available > UINT32_MAX
                ? UINT32_MAX : (uint32_t)available;
        __builtin_memcpy(translated, &packet, sizeof(packet));
        __builtin_memcpy(translated + boxes_bytes, &suffix, sizeof(suffix));
        return drm_vmwgfx_map_svga_status(svga_batch_append(&work->batch,
            command, true, translated, body_bytes));
    }
    case SVGA_3D_CMD_SETRENDERTARGET: {
        SVGA3dCmdSetRenderTarget packet;
        __builtin_memcpy(&packet, translated, sizeof(packet));
        struct drm_vmwgfx_surface_ref *surface = NULL;
        svga_handle surface_handle = SVGA_INVALID_HANDLE;
        if (packet.target.sid != SVGA3D_INVALID_ID) {
            surface = drm_vmwgfx_exec_find_surface(work, packet.target.sid);
            if (!surface) return -LINUX_EINVAL;
            surface_handle = surface->backend;
            packet.target.sid = surface->backend_id;
        }
        int ret = drm_vmwgfx_map_svga_status(svga_batch_stage_render_target(
            &work->batch, context->backend, packet.type, surface_handle,
            packet.target.face, packet.target.mipmap));
        if (ret) return ret;
        __builtin_memcpy(translated, &packet, sizeof(packet));
        break;
    }
    case SVGA_3D_CMD_SETTEXTURESTATE: {
        uint32_t count = (body_bytes - sizeof(uint32_t)) /
                         sizeof(SVGA3dTextureState);
        SVGA3dTextureState *states =
            (SVGA3dTextureState *)(translated + sizeof(uint32_t));
        for (uint32_t i = 0; i < count; ++i) {
            if (states[i].name != SVGA3D_TS_BIND_TEXTURE) continue;
            svga_handle surface_handle = SVGA_INVALID_HANDLE;
            if (states[i].value != SVGA3D_INVALID_ID) {
                struct drm_vmwgfx_surface_ref *surface =
                    drm_vmwgfx_exec_find_surface(work, states[i].value);
                if (!surface) return -LINUX_EINVAL;
                states[i].value = surface->backend_id;
                surface_handle = surface->backend;
            }
            int ret = drm_vmwgfx_map_svga_status(svga_batch_stage_texture(
                &work->batch, context->backend, states[i].stage,
                surface_handle));
            if (ret) return ret;
        }
        break;
    }
    case SVGA_3D_CMD_DRAW_PRIMITIVES: {
        SVGA3dCmdDrawPrimitives header;
        __builtin_memcpy(&header, translated, sizeof(header));
        SVGA3dVertexDecl *decls =
            (SVGA3dVertexDecl *)(translated + sizeof(header));
        SVGA3dPrimitiveRange *ranges =
            (SVGA3dPrimitiveRange *)(decls + header.numVertexDecls);
        for (uint32_t i = 0; i < header.numVertexDecls; ++i) {
            struct drm_vmwgfx_surface_ref *surface =
                drm_vmwgfx_exec_find_surface(work,
                    decls[i].array.surfaceId);
            if (!surface) return -LINUX_EINVAL;
            decls[i].array.surfaceId = surface->backend_id;
        }
        for (uint32_t i = 0; i < header.numRanges; ++i) {
            if (ranges[i].indexArray.surfaceId == SVGA3D_INVALID_ID) continue;
            struct drm_vmwgfx_surface_ref *surface =
                drm_vmwgfx_exec_find_surface(work,
                    ranges[i].indexArray.surfaceId);
            if (!surface) return -LINUX_EINVAL;
            ranges[i].indexArray.surfaceId = surface->backend_id;
        }
        break;
    }
    case SVGA_3D_CMD_SHADER_DEFINE: {
        SVGA3dCmdDefineShader packet;
        __builtin_memcpy(&packet, translated, sizeof(packet));
        return drm_vmwgfx_map_svga_status(svga_batch_shader_define(
            &work->batch, context->backend, packet.shid, packet.type,
            translated + sizeof(packet), body_bytes - sizeof(packet)));
    }
    case SVGA_3D_CMD_SHADER_DESTROY: {
        SVGA3dCmdDestroyShader packet;
        __builtin_memcpy(&packet, translated, sizeof(packet));
        return drm_vmwgfx_map_svga_status(svga_batch_shader_destroy(
            &work->batch, context->backend, packet.shid, packet.type));
    }
    case SVGA_3D_CMD_SET_SHADER: {
        SVGA3dCmdSetShader packet;
        __builtin_memcpy(&packet, translated, sizeof(packet));
        return drm_vmwgfx_map_svga_status(svga_batch_bind_shader(
            &work->batch, context->backend, packet.type, packet.shid));
    }
    case SVGA_3D_CMD_SET_SHADER_CONST: {
        SVGA3dCmdSetShaderConst packet;
        __builtin_memcpy(&packet, translated, sizeof(packet));
        uint32_t registers = 1u + (body_bytes - sizeof(packet)) / 16u;
        /* The batch helper copies all vectors synchronously from the snapshot. */
        const uint32_t *values = (const uint32_t *)(translated +
            __builtin_offsetof(SVGA3dCmdSetShaderConst, values));
        return drm_vmwgfx_map_svga_status(svga_batch_shader_constants(
            &work->batch, context->backend, packet.reg, packet.type,
            packet.ctype, values, registers));
    }
    case SVGA_3D_CMD_BEGIN_QUERY:
    case SVGA_3D_CMD_END_QUERY:
    case SVGA_3D_CMD_WAIT_FOR_QUERY: {
        SVGA3dQueryType type;
        __builtin_memcpy(&type, translated + sizeof(uint32_t), sizeof(type));
        if (command == SVGA_3D_CMD_BEGIN_QUERY)
            return drm_vmwgfx_map_svga_status(svga_batch_query_begin(
                &work->batch, context->backend, type));
        SVGAGuestPtr result;
        __builtin_memcpy(&result, translated + 2u * sizeof(uint32_t),
                         sizeof(result));
        struct drm_vmwgfx_bo_ref *bo =
            drm_vmwgfx_exec_find_bo(work, result.gmrId);
        if (!bo || !bo->attachment || !bo->attachment->ready)
            return -LINUX_EINVAL;
        int ret = command == SVGA_3D_CMD_END_QUERY
            ? svga_batch_query_end(&work->batch, context->backend, type,
                                   bo->attachment->gmr, result.offset)
            : svga_batch_query_wait(&work->batch, context->backend, type,
                                    bo->attachment->gmr, result.offset);
        return drm_vmwgfx_map_svga_status(ret);
    }
    default:
        break;
    }
    return drm_vmwgfx_map_svga_status(svga_batch_append(&work->batch,
        command, true, translated, body_bytes));
}

static void drm_vmwgfx_exec_attachment_owner_put(void *opaque)
{
    struct drm_vmwgfx_attachment *attachment = opaque;
    if (!attachment) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    struct drm_vmwgfx_attachment **link = &drm_vmwgfx_attachments;
    while (*link && *link != attachment) link = &(*link)->next;
    if (*link == attachment) {
        *link = attachment->next;
        if (drm_vmwgfx_attachment_count) --drm_vmwgfx_attachment_count;
        if (drm_vmwgfx_attachment_bytes >= attachment->bytes)
            drm_vmwgfx_attachment_bytes -= attachment->bytes;
        else
            drm_vmwgfx_attachment_bytes = 0;
    }
    struct drm_bo *bo = attachment->bo;
    attachment->bo = NULL;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    if (bo) drm_bo_put(bo);
    kernel_free(attachment);
}

static int drm_vmwgfx_exec_attachment_progress(
    struct drm_vmwgfx_attachment *attachment)
{
    if (!attachment || !attachment->bo || attachment->detaching)
        return -LINUX_EINVAL;
    if (attachment->error) return attachment->error;
    if (attachment->ready) return 0;
    uint64_t phys = drm_bo_page_physical(attachment->bo, 0);
    uint64_t size = drm_bo_size(attachment->bo);
    if (!phys || (phys & 4095u) || !size || size > UINT32_MAX)
        return -LINUX_EINVAL;
    int status = svga_gmr_borrowed_attach(phys, (uint32_t)size, attachment,
        drm_vmwgfx_exec_attachment_owner_put, &attachment->gmr);
    if (status == SVGA_EBUSY) return -LINUX_EBUSY;
    if (status) {
        attachment->error = drm_vmwgfx_map_svga_status(status);
        return attachment->error;
    }
    status = svga_gmr_borrowed_mob_id(attachment->gmr,
                                      &attachment->mobid);
    if (status) {
        if (status == SVGA_EBUSY) return -LINUX_EBUSY;
        attachment->error = drm_vmwgfx_map_svga_status(status);
        return attachment->error;
    }
    attachment->ready = true;
    return 0;
}

static void drm_vmwgfx_exec_queue_cleanup(struct drm_vmwgfx_exec_work *work)
{
    if (!work || work->cleanup_queued) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    if (!work->cleanup_queued) {
        work->cleanup_queued = true;
        work->next = drm_vmwgfx_exec_cleanup;
        drm_vmwgfx_exec_cleanup = work;
    }
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
}

static void drm_vmwgfx_exec_attachment_put(
    struct drm_vmwgfx_attachment *attachment)
{
    if (!attachment) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    if (attachment->references) --attachment->references;
    if (!attachment->references) attachment->idle_since = time_ticks();
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
}

static int drm_vmwgfx_exec_attachment_get(
    struct drm_bo *bo, struct drm_vmwgfx_attachment **out,
    const char **failure_stage)
{
    if (failure_stage) *failure_stage = "exec.attach.validate";
    if (!bo || !out) return -LINUX_EINVAL;
    *out = NULL;
    uint64_t bytes = drm_bo_size(bo);
    if (!bytes || bytes > DRM_VMW_EXEC_ATTACHMENT_BYTES_MAX)
        return -LINUX_EINVAL;

    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    for (struct drm_vmwgfx_attachment *attachment = drm_vmwgfx_attachments;
         attachment; attachment = attachment->next) {
        if (attachment->bo != bo || attachment->detaching) continue;
        if (attachment->references == UINT32_MAX) {
            kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
            if (failure_stage) *failure_stage = "exec.attach.ref_limit";
            return -LINUX_ENOMEM;
        }
        ++attachment->references;
        *out = attachment;
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
        if (failure_stage) *failure_stage = "exec.attach.backend";
        return drm_vmwgfx_exec_attachment_progress(attachment);
    }
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);

    if (failure_stage) *failure_stage = "exec.attach.heap";
    struct drm_vmwgfx_attachment *candidate = kernel_malloc(sizeof(*candidate));
    if (!candidate) return -LINUX_ENOMEM;
    *candidate = (struct drm_vmwgfx_attachment){.bo = bo, .bytes = (uint32_t)bytes,
        .references = 1, .idle_since = time_ticks()};
    drm_bo_get(bo); /* transferred to the attachment's backend owner */

    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    for (struct drm_vmwgfx_attachment *attachment = drm_vmwgfx_attachments;
         attachment; attachment = attachment->next) {
        if (attachment->bo != bo || attachment->detaching) continue;
        if (attachment->references == UINT32_MAX) {
            kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
            drm_bo_put(bo);
            kernel_free(candidate);
            if (failure_stage) *failure_stage = "exec.attach.ref_limit";
            return -LINUX_ENOMEM;
        }
        ++attachment->references;
        *out = attachment;
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
        drm_bo_put(bo);
        kernel_free(candidate);
        if (failure_stage) *failure_stage = "exec.attach.backend";
        return drm_vmwgfx_exec_attachment_progress(attachment);
    }
    if (drm_vmwgfx_attachment_count >= DRM_VMW_EXEC_ATTACHMENTS_MAX ||
        bytes > DRM_VMW_EXEC_ATTACHMENT_BYTES_MAX - drm_vmwgfx_attachment_bytes) {
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
        drm_bo_put(bo);
        kernel_free(candidate);
        if (failure_stage) *failure_stage = "exec.attach.quota";
        return -LINUX_ENOMEM;
    }
    candidate->next = drm_vmwgfx_attachments;
    drm_vmwgfx_attachments = candidate;
    ++drm_vmwgfx_attachment_count;
    drm_vmwgfx_attachment_bytes += bytes;
    *out = candidate;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    if (failure_stage) *failure_stage = "exec.attach.backend";
    return drm_vmwgfx_exec_attachment_progress(candidate);
}

static void drm_vmwgfx_exec_work_release(struct drm_vmwgfx_exec_work *work)
{
    if (!work) return;
    if (work->batch_open) {
        int ret = drm_vmwgfx_map_svga_status(svga_batch_abort(&work->batch));
        if (ret) {
            drm_vmwgfx_exec_queue_cleanup(work);
            return;
        }
        work->batch_open = false;
    }
    for (uint32_t i = 0; i < work->context_count; ++i)
        drm_vmwgfx_context_put(work->contexts[i].object);
    for (uint32_t i = 0; i < work->surface_count; ++i)
        drm_vmwgfx_surface_put(work->surfaces[i].object);
    for (uint32_t i = 0; i < work->bo_count; ++i) {
        drm_vmwgfx_exec_attachment_put(work->bos[i].attachment);
        drm_bo_gpu_release(work->bos[i].object);
        drm_bo_put(work->bos[i].object);
    }
    drm_vmwgfx_fence_put(work->fence_object);
    kernel_free(work->snapshot);
    kernel_free(work->scratch);
    kernel_free(work->output);
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    if (drm_vmwgfx_exec_work_count) --drm_vmwgfx_exec_work_count;
    if (drm_vmwgfx_exec_work_bytes >= work->allocation_bytes)
        drm_vmwgfx_exec_work_bytes -= work->allocation_bytes;
    else
        drm_vmwgfx_exec_work_bytes = 0;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    kernel_free(work);
}

static void drm_vmwgfx_exec_work_fail(
    struct drm_vmwgfx_exec_work *work,
    struct drm_faildiag_context *diagnostics, int error, const char *stage)
{
    if (!work) return;
    uint32_t source_bytes = work->source_bytes;
    uint32_t output_bytes = work->batch.bytes;
    uint32_t output_capacity = work->output_capacity;
    uint32_t contexts = work->context_count;
    uint32_t surfaces = work->surface_count;
    uint32_t bos = work->bo_count;
    drm_vmwgfx_exec_work_release(work);
    drm_vmwgfx_execdiag(diagnostics, error, stage, source_bytes,
        output_bytes, output_capacity, contexts, surfaces, bos);
}

static void drm_vmwgfx_exec_attachment_fail(
    struct drm_vmwgfx_exec_work *work,
    struct drm_faildiag_context *diagnostics, int error, const char *stage)
{
    if (!work) return;
    uint32_t source_bytes = work->source_bytes;
    uint32_t output_capacity = work->output_capacity;
    uint32_t bo_refs = work->bo_count;
    drm_vmwgfx_exec_work_release(work);
    drm_vmwgfx_attachment_diag(diagnostics, error, stage, source_bytes,
        output_capacity, 0, bo_refs);
}

static void drm_vmwgfx_exec_fence_fail(
    struct drm_vmwgfx_exec_work *work,
    struct drm_faildiag_context *diagnostics, int error, const char *stage)
{
    if (!work) return;
    uint32_t source_bytes = work->source_bytes;
    uint32_t output_bytes = work->batch.bytes;
    uint32_t output_capacity = work->output_capacity;
    drm_vmwgfx_exec_work_release(work);
    drm_vmwgfx_fence_diag(diagnostics, error, stage, source_bytes,
        output_bytes, output_capacity);
}

static void drm_vmwgfx_exec_submit_fail(
    struct drm_vmwgfx_exec_work *work,
    struct drm_faildiag_context *diagnostics, int error, const char *stage)
{
    if (!work) return;
    uint32_t source_bytes = work->source_bytes;
    uint32_t output_bytes = work->batch.bytes;
    uint32_t output_capacity = work->output_capacity;
    struct svga_batch_submit_failure failure = work->batch.submit_failure;
    drm_vmwgfx_exec_work_release(work);
    drm_vmwgfx_exec_submit_diag(diagnostics, error, stage, source_bytes,
        output_bytes, output_capacity, &failure);
}

static void drm_vmwgfx_exec_work_alloc_diag(
    struct drm_faildiag_context *diagnostics, int error, const char *stage,
    uint32_t source_bytes, uint32_t output_capacity, uint64_t requested)
{
    if (!drm_faildiag_is_enabled()) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    uint32_t work_count = drm_vmwgfx_exec_work_count;
    uint32_t work_bytes = drm_vmwgfx_exec_work_bytes;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    const struct drm_faildiag_field fields[] = {
        {"source", source_bytes},
        {"output_capacity", output_capacity},
        {"requested", requested},
        {"work_count", work_count},
        {"work_bytes", work_bytes},
        {"work_limit", DRM_VMW_EXEC_WORK_BYTES_MAX},
    };
    drm_faildiag_record(diagnostics, error, stage, fields,
                        sizeof(fields) / sizeof(fields[0]));
}

static struct drm_vmwgfx_exec_work *drm_vmwgfx_exec_work_alloc(
    uint32_t source_bytes, uint32_t packet_count, int *error,
    struct drm_faildiag_context *diagnostics)
{
    uint64_t output_capacity64 = source_bytes
        ? (uint64_t)source_bytes * 4u + 1024u : 0;
    if (output_capacity64 < 4096u && source_bytes) output_capacity64 = 4096u;
    if (output_capacity64 > DRM_VMW_EXEC_OUTPUT_BYTES_MAX)
        output_capacity64 = DRM_VMW_EXEC_OUTPUT_BYTES_MAX;
    uint32_t output_capacity = (uint32_t)output_capacity64;
    uint64_t allocation = sizeof(struct drm_vmwgfx_exec_work) +
        (uint64_t)source_bytes * 2u + output_capacity;
    if (allocation > UINT32_MAX || allocation > DRM_VMW_EXEC_WORK_BYTES_MAX) {
        if (error) *error = -LINUX_ENOMEM;
        drm_vmwgfx_exec_work_alloc_diag(diagnostics, -LINUX_ENOMEM,
            "exec.work.bytes", source_bytes, output_capacity, allocation);
        return NULL;
    }

    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    if (drm_vmwgfx_exec_work_count >= DRM_VMW_EXEC_WORK_MAX ||
        allocation > DRM_VMW_EXEC_WORK_BYTES_MAX - drm_vmwgfx_exec_work_bytes) {
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
        if (error) *error = -LINUX_ENOMEM;
        drm_vmwgfx_exec_work_alloc_diag(diagnostics, -LINUX_ENOMEM,
            "exec.work.quota", source_bytes, output_capacity, allocation);
        return NULL;
    }
    ++drm_vmwgfx_exec_work_count;
    drm_vmwgfx_exec_work_bytes += (uint32_t)allocation;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);

    struct drm_vmwgfx_exec_work *work = kernel_malloc(sizeof(*work));
    if (!work) goto no_memory;
    *work = (struct drm_vmwgfx_exec_work){.source_bytes = source_bytes,
        .packet_count = packet_count, .output_capacity = output_capacity,
        .allocation_bytes = (uint32_t)allocation};
    if (source_bytes) {
        work->snapshot = kernel_malloc(source_bytes);
        work->scratch = kernel_malloc(source_bytes);
        work->output = kernel_malloc(output_capacity);
        if (!work->snapshot || !work->scratch || !work->output) {
            kernel_free(work->snapshot);
            kernel_free(work->scratch);
            kernel_free(work->output);
            kernel_free(work);
            goto no_memory;
        }
    }
    if (error) *error = 0;
    return work;

no_memory:
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    if (drm_vmwgfx_exec_work_count) --drm_vmwgfx_exec_work_count;
    if (drm_vmwgfx_exec_work_bytes >= allocation)
        drm_vmwgfx_exec_work_bytes -= (uint32_t)allocation;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
    if (error) *error = -LINUX_ENOMEM;
    drm_vmwgfx_exec_work_alloc_diag(diagnostics, -LINUX_ENOMEM,
        "exec.work.heap", source_bytes, output_capacity, allocation);
    return NULL;
}

void drm_vmwgfx_execbuf_process_retirements(uint32_t budget)
{
    uint64_t now = time_ticks();
    if (budget > 8u) budget = 8u;
    for (uint32_t i = 0; i < budget; ++i) {
        uint64_t flags;
        kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
        struct drm_vmwgfx_task_continuation *continuation =
            drm_vmwgfx_continuation_cleanup;
        if (continuation) {
            drm_vmwgfx_continuation_cleanup = continuation->next;
            continuation->next = NULL;
        }
        struct drm_vmwgfx_exec_work *work = drm_vmwgfx_exec_cleanup;
        if (work) {
            drm_vmwgfx_exec_cleanup = work->next;
            work->next = NULL;
            work->cleanup_queued = false;
        } else {
            work = drm_vmwgfx_exec_jobs;
            if (work) {
                drm_vmwgfx_exec_jobs = work->next;
                if (!drm_vmwgfx_exec_jobs)
                    drm_vmwgfx_exec_jobs_tail = NULL;
                work->next = NULL;
            }
        }
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
        drm_vmwgfx_continuation_release(continuation);
        if (work) {
            if (!work->submitted) {
                drm_vmwgfx_exec_work_release(work);
            } else {
                bool signaled = false;
                int status = drm_vmwgfx_map_svga_status(
                    svga_fence_status(work->fence, &signaled));
                bool quiesced = false;
                if (status == -LINUX_EIO) {
                    work->reset_pending = true;
                    quiesced = svga_fence_is_quiesced(work->fence);
                }
                if ((!status && signaled) ||
                    (work->reset_pending && quiesced)) {
                    drm_vmwgfx_exec_work_release(work);
                } else {
                    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
                    drm_vmwgfx_exec_job_enqueue_locked(work);
                    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
                }
            }
        }

        struct drm_vmwgfx_attachment *attachment = NULL;
        bool detach = false;
        kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
        struct drm_vmwgfx_attachment *cursor = drm_vmwgfx_attachments;
        if (cursor) {
            uint32_t skip = drm_vmwgfx_attachment_progress_cursor++ %
                            (drm_vmwgfx_attachment_count
                                ? drm_vmwgfx_attachment_count : 1u);
            while (skip-- && cursor) cursor = cursor->next;
        }
        if (cursor && cursor->last_progress_tick != now) {
            cursor->last_progress_tick = now;
            attachment = cursor;
            if (!cursor->references && now >= cursor->idle_since &&
                now - cursor->idle_since >= DRM_VMW_EXEC_ATTACHMENT_CACHE_TICKS) {
                cursor->detaching = true;
                detach = true;
            }
        }
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
        if (!attachment) continue;
        if (detach) {
            if (!attachment->gmr) {
                drm_vmwgfx_exec_attachment_owner_put(attachment);
            } else {
                /* A busy detach may already have queued host destruction.
                 * Keep this cache entry unavailable until the owner callback
                 * unlinks it after the backend retirement fence completes. */
                (void)svga_gmr_borrowed_detach(attachment->gmr);
            }
        } else if (!attachment->ready && !attachment->error) {
            (void)drm_vmwgfx_exec_attachment_progress(attachment);
        }
    }
}

static bool drm_vmwgfx_continuation_matches(
    const struct drm_vmwgfx_task_continuation *continuation,
    const struct task *task, const struct task_file *syscall_file,
    const struct drm_file *file, uint32_t command, uint64_t user_argument)
{
    return continuation && task && continuation->syscall_file == syscall_file &&
        continuation->syscall_fd == task->syscall_fd &&
        continuation->syscall_number == task->syscall_file_number &&
        continuation->file == file && continuation->command == command &&
        continuation->user_argument == user_argument;
}

static void drm_vmwgfx_continuation_set_key(
    struct drm_vmwgfx_task_continuation *continuation,
    struct task *task, struct drm_file *file, uint32_t command,
    uint64_t user_argument)
{
    continuation->file = file;
    continuation->syscall_file = task->syscall_file;
    continuation->syscall_fd = task->syscall_fd;
    continuation->syscall_number = task->syscall_file_number;
    continuation->command = command;
    continuation->user_argument = user_argument;
}

static void drm_vmwgfx_continuation_release(
    struct drm_vmwgfx_task_continuation *continuation)
{
    if (!continuation) return;
    drm_bo_put(continuation->bo);
    drm_vmwgfx_fence_put(continuation->fence_object);
    kernel_free(continuation);
}

static void drm_vmwgfx_continuation_queue_release(
    struct drm_vmwgfx_task_continuation *continuation)
{
    if (!continuation) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &flags);
    continuation->next = drm_vmwgfx_continuation_cleanup;
    drm_vmwgfx_continuation_cleanup = continuation;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, flags);
}

static int64_t drm_vmwgfx_resume_continuation(
    struct task *task, struct task_file *syscall_file, struct drm_file *file,
    uint32_t command, uint64_t user_argument, void *data, uint32_t data_bytes,
    bool *handled, bool *skip_input)
{
    if (handled) *handled = false;
    if (skip_input) *skip_input = false;
    struct drm_vmwgfx_task_continuation *continuation = task
        ? task->drm_vmwgfx_continuation : NULL;
    if (!continuation) return 0;
    if (!drm_vmwgfx_continuation_matches(continuation, task, syscall_file,
            file, command, user_argument)) {
        drm_vmwgfx_cancel_task_continuation(task);
        return 0;
    }
    if (continuation->kind == DRM_VMW_CONT_FENCE_REPLY_FAULT) {
        bool signaled = false;
        int status = drm_vmwgfx_map_svga_status(
            svga_fence_status(continuation->fence, &signaled));
        if (status == -LINUX_EBUSY) status = 0;
        if (status == -LINUX_EIO) {
            struct svga_fence_progress progress;
            if (!svga_fence_progress_snapshot(&progress) &&
                progress.generation != (uint32_t)(continuation->fence >> 32))
                signaled = true;
        }
        if (signaled || status < 0 || time_ticks() >= continuation->deadline) {
            task->drm_vmwgfx_continuation = NULL;
            drm_vmwgfx_continuation_release(continuation);
            if (handled) *handled = true;
            return -LINUX_EFAULT;
        }
        sched_sleep_current_until(time_ticks() + 1u);
        if (handled) *handled = true;
        return KERNEL_SYSCALL_BLOCKED;
    }
    if (!data || !skip_input || continuation->snapshot_bytes > data_bytes) {
        drm_vmwgfx_cancel_task_continuation(task);
        if (handled) *handled = true;
        return -LINUX_EFAULT;
    }
    if (continuation->kind == DRM_VMW_CONT_FENCE_WAIT)
        __builtin_memcpy(data, &continuation->snapshot.wait,
                         sizeof(continuation->snapshot.wait));
    else if (continuation->kind == DRM_VMW_CONT_SYNCCPU)
        __builtin_memcpy(data, &continuation->snapshot.synccpu,
                         sizeof(continuation->snapshot.synccpu));
    else {
        drm_vmwgfx_cancel_task_continuation(task);
        if (handled) *handled = true;
        return -LINUX_EINVAL;
    }
    *skip_input = true;
    return 0;
}

int64_t drm_vmwgfx_prepare_ioctl_continuation(
    struct task *task, struct task_file *syscall_file, struct drm_file *file,
    uint32_t command, uint64_t user_argument, void *data, uint32_t data_bytes,
    bool *handled, bool *skip_input)
{
    return drm_vmwgfx_resume_continuation(task, syscall_file, file, command,
        user_argument, data, data_bytes, handled, skip_input);
}

bool drm_vmwgfx_task_uninterruptible(const struct task *task)
{
    const struct drm_vmwgfx_task_continuation *continuation = task
        ? task->drm_vmwgfx_continuation : NULL;
    return continuation &&
        continuation->kind == DRM_VMW_CONT_FENCE_REPLY_FAULT;
}

bool drm_vmwgfx_task_signal_busy(const struct task *task)
{
    const struct drm_vmwgfx_task_continuation *continuation = task
        ? task->drm_vmwgfx_continuation : NULL;
    return continuation && continuation->kind == DRM_VMW_CONT_SYNCCPU;
}

static int drm_vmwgfx_execbuf(struct task *task, struct drm_file *file,
                              uint32_t command, struct drm_vmw_execbuf_arg *arg,
                              uint64_t user_argument,
                              struct drm_faildiag_context *diagnostics)
{
    uint32_t arg_size = _IOC_SIZE(command);
    if (_IOC_DIR(command) != _IOC_WRITE ||
        (arg_size != sizeof(*arg) && arg_size != DRM_VMW_EXECBUF_V1_SHORT_SIZE)) {
        const struct drm_faildiag_field fields[] = {
            {"argument_bytes", arg_size},
            {"source", arg->command_size},
        };
        drm_faildiag_record(diagnostics, -LINUX_EINVAL, "exec.argument",
                            fields, sizeof(fields) / sizeof(fields[0]));
        return -LINUX_EINVAL;
    }
    if (arg->version != 1u || arg->flags != 0u ||
        arg->command_size > DRM_VMW_EXECBUF_MAX_COMMAND_BYTES ||
        (arg->command_size && !arg->commands)) {
        const struct drm_faildiag_field fields[] = {
            {"argument_bytes", arg_size},
            {"source", arg->command_size},
            {"source_limit", DRM_VMW_EXECBUF_MAX_COMMAND_BYTES},
        };
        drm_faildiag_record(diagnostics, -LINUX_EINVAL, "exec.argument",
                            fields, sizeof(fields) / sizeof(fields[0]));
        return -LINUX_EINVAL;
    }

    /* Linux 6.12 ignores this field for every version-1 payload, including
     * the full-size encoding. Keep the local copy canonical as well. */
    arg->context_handle = SVGA3D_INVALID_ID;
    int allocation_error = 0;
    struct drm_vmwgfx_exec_work *work = drm_vmwgfx_exec_work_alloc(
        arg->command_size, 0, &allocation_error, diagnostics);
    if (!work) return allocation_error;
    if (arg->command_size &&
        user_copy_from_task(task, work->snapshot, arg->commands,
                            arg->command_size) < 0) {
        drm_vmwgfx_exec_work_fail(work, diagnostics, -LINUX_EFAULT,
                                  "exec.input_copy");
        return -LINUX_EFAULT;
    }

    int status = 0;
    struct drm_vmwgfx_exec_walk walk = {.work = work, .file = file};
    if (arg->command_size) {
        status = drm_vmwgfx_validate_stream(work->snapshot,
                                             arg->command_size,
                                             &work->packet_count);
        if (status) {
            drm_vmwgfx_exec_work_fail(work, diagnostics, status,
                                      "exec.stream");
            return status;
        }
        status = drm_vmwgfx_walk_stream(work->snapshot,
            arg->command_size, &work->packet_count,
            drm_vmwgfx_exec_validate_packet, &walk);
        if (status) {
            const char *stage = status == -LINUX_ENOMEM
                ? "exec.references" : "exec.validation";
            drm_vmwgfx_exec_work_fail(work, diagnostics, status, stage);
            return status;
        }

        for (uint32_t i = 0; i < work->bo_count; ++i) {
            const char *failure_stage = NULL;
            status = drm_vmwgfx_exec_attachment_get(work->bos[i].object,
                &work->bos[i].attachment, &failure_stage);
            if (status) {
                drm_vmwgfx_exec_attachment_fail(work, diagnostics, status,
                    failure_stage ? failure_stage : "exec.attach.backend");
                return status;
            }
        }

        struct svga_info info;
        svga_get_info(&info);
        if (!info.available || !info.fifo_ready) {
            drm_vmwgfx_exec_work_fail(work, diagnostics, -LINUX_ENODEV,
                                      "exec.device");
            return -LINUX_ENODEV;
        }
        status = drm_vmwgfx_map_svga_status(svga_batch_init_sized(
            &work->batch, info.generation, work->output,
            work->output_capacity, work->source_bytes));
        if (status) {
            drm_vmwgfx_exec_work_fail(work, diagnostics, status,
                                      "exec.batch_init");
            return status;
        }
        work->batch_open = true;
        for (uint32_t i = 0; i < work->context_count && !status; ++i)
            status = drm_vmwgfx_map_svga_status(svga_batch_retain_context(
                &work->batch, work->contexts[i].backend));
        for (uint32_t i = 0; i < work->surface_count && !status; ++i)
            status = drm_vmwgfx_map_svga_status(svga_batch_retain_surface(
                &work->batch, work->surfaces[i].backend));
        for (uint32_t i = 0; i < work->bo_count && !status; ++i) {
            if (!work->bos[i].attachment || !work->bos[i].attachment->ready)
                status = -LINUX_EBUSY;
            else
                status = drm_vmwgfx_map_svga_status(svga_batch_retain_gmr(
                    &work->batch, work->bos[i].attachment->gmr));
        }
        if (status) {
            drm_vmwgfx_exec_work_fail(work, diagnostics, status,
                                      "exec.batch_retain");
            return status;
        }
        status = drm_vmwgfx_walk_stream(work->snapshot,
            arg->command_size, &work->packet_count,
            drm_vmwgfx_exec_translate_packet, &walk);
        if (status) {
            drm_vmwgfx_exec_work_fail(work, diagnostics, status,
                                      "exec.translation");
            return status;
        }
    }

    const char *fence_failure_stage = NULL;
    struct drm_vmwgfx_fence *fence = drm_vmwgfx_fence_reserve(
        file, arg->fence_rep != 0, &fence_failure_stage);
    if (!fence) {
        drm_vmwgfx_exec_fence_fail(work, diagnostics, -LINUX_ENOMEM,
            fence_failure_stage ? fence_failure_stage : "exec.fence.heap");
        return -LINUX_ENOMEM;
    }
    work->fence_object = fence;
    struct drm_vmwgfx_task_continuation *fault_wait = NULL;
    if (arg->fence_rep) {
        fault_wait = kernel_malloc(sizeof(*fault_wait));
        if (!fault_wait) {
            drm_vmwgfx_exec_fence_fail(work, diagnostics, -LINUX_ENOMEM,
                                      "exec.fence.reply_heap");
            return -LINUX_ENOMEM;
        }
        *fault_wait = (struct drm_vmwgfx_task_continuation){0};
    }

    struct svga_fence_progress before = {0};
    (void)svga_fence_progress_snapshot(&before);
    svga_handle token = 0;
    status = arg->command_size
        ? drm_vmwgfx_map_svga_status(svga_batch_submit(&work->batch, &token))
        : drm_vmwgfx_map_svga_status(svga_fence_try_insert(&token));
    if (status) {
        kernel_free(fault_wait);
        if (arg->command_size)
            drm_vmwgfx_exec_submit_fail(work, diagnostics, status,
                                        "exec.fifo_submit");
        else
            drm_vmwgfx_exec_fence_fail(work, diagnostics, status,
                                       "exec.fifo_submit");
        return status;
    }
    work->batch_open = false;
    work->submitted = true;
    work->fence = token;
    fence->token = token;
    drm_vmwgfx_fence_publish(file, fence);
    uint64_t list_flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_exec_lock, &list_flags);
    drm_vmwgfx_exec_job_enqueue_locked(work);
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_exec_lock, list_flags);

    if (!arg->fence_rep) return 0;
    struct svga_fence_progress progress = before;
    if (!svga_fence_progress_snapshot(&progress)) {
        /* A successful snapshot is actual progress; if the try-lock is busy,
         * the pre-submit snapshot remains a conservative valid sequence. */
    }
    struct drm_vmw_fence_rep result = {
        .handle = fence->handle,
        .mask = 0,
        .seqno = (uint32_t)token,
        .passed_seqno = progress.passed_seq,
        .fd = -1,
        .error = 0,
    };
    if (user_copy_to_task(task, arg->fence_rep, &result, sizeof(result)) < 0) {
        (void)drm_vmwgfx_fence_remove(file, fence->handle);
        drm_vmwgfx_fence_get(fence);
        fault_wait->kind = DRM_VMW_CONT_FENCE_REPLY_FAULT;
        fault_wait->fence = token;
        fault_wait->fence_object = fence;
        fault_wait->deadline = time_ticks() +
                               5u * RELIEFNT_TICK_HZ;
        drm_vmwgfx_continuation_set_key(fault_wait, task, file, command,
                                         user_argument);
        task->drm_vmwgfx_continuation = fault_wait;
        sched_sleep_current_until(time_ticks() + 1u);
        return KERNEL_SYSCALL_BLOCKED;
    }
    kernel_free(fault_wait);
    return 0;
}

static int drm_vmwgfx_fence_signaled(struct drm_file *file,
                                     struct drm_vmw_fence_signaled_arg *arg)
{
    struct drm_vmwgfx_fence *fence = drm_vmwgfx_fence_lookup_get(file,
                                                                 arg->handle);
    if (!fence) return -LINUX_EINVAL;
    bool signaled = false;
    int status = drm_vmwgfx_map_svga_status(
        svga_fence_status(fence->token, &signaled));
    if (status) { drm_vmwgfx_fence_put(fence); return status; }
    struct svga_fence_progress progress;
    status = drm_vmwgfx_map_svga_status(svga_fence_progress_snapshot(&progress));
    if (status) { drm_vmwgfx_fence_put(fence); return status; }
    arg->signaled = signaled ? 1 : 0;
    arg->passed_seqno = progress.passed_seq;
    arg->signaled_flags = arg->flags;
    drm_vmwgfx_fence_put(fence);
    return 0;
}

static uint64_t drm_vmwgfx_timeout_ticks(uint64_t timeout_us)
{
    uint64_t seconds = timeout_us / 1000000u;
    uint64_t remainder = timeout_us % 1000000u;
    if (seconds > UINT64_MAX / RELIEFNT_TICK_HZ) return UINT64_MAX;
    uint64_t ticks = seconds * RELIEFNT_TICK_HZ;
    uint64_t fraction = (remainder * RELIEFNT_TICK_HZ + 999999u) / 1000000u;
    if (ticks > UINT64_MAX - fraction) return UINT64_MAX;
    return ticks + fraction;
}

/**
 * @brief Try a short, unlocked poll for a FENCE_WAIT attempt.
 * @param fence Immutable retained SVGA fence token; no fence object is touched
 *        while execution ownership is suspended.
 * @param signaled Set true when a successful status sample passes. A conclusive
 *        in-flight sample is honored even if its return crosses the budget.
 * @param status Set to the mapped SVGA error if a poll returns a hard error.
 * @param now Updated from the kernel tick clock after execution ownership is
 *        reacquired, before the caller makes timeout or sleep decisions.
 * @return True if the unlocked poll ran and execution ownership was restored;
 *         false if the fine-clock or suspend guard declined it.
 *
 * The 500 us budget limits starting further samples within the polling
 * window; it is not an absolute CPU or wall-time deadline. An in-flight
 * status call, interrupt, or VM deschedule can carry that window past the
 * budget, and a conclusive completion is still accepted. Interrupts remain
 * enabled while global execution ownership is suspended. A finite
 * 8192-sample ceiling also bounds polling if the fine clock stops or moves
 * backward. The suspend token is held while no global execution transaction
 * or SVGA lock is held. A failed resume violates kernel ownership invariants,
 * so shared state must not be touched afterward. Reacquiring the execution
 * transaction can add further contention.
 */
static bool drm_vmwgfx_fence_wait_fast_poll(
    svga_handle fence, bool *signaled, int *status, uint64_t *now)
{
    if (!signaled || !status || !now) return false;

    struct linux_timespec resolution = {0};
    if (time_clock_getres(LINUX_CLOCK_MONOTONIC, &resolution) != 0 ||
        resolution.tv_sec != 0 || resolution.tv_nsec <= 0 ||
        resolution.tv_nsec > DRM_VMW_FENCE_WAIT_FAST_CLOCK_RESOLUTION_NS)
        return false;

    struct kernel_execution_suspend_token token = {0};
    if (kernel_execution_suspend_irqrestore(&token) != 0) return false;

    const uint64_t started_us = time_uptime_us();
    for (uint32_t sample = 0;
         sample < DRM_VMW_FENCE_WAIT_FAST_POLL_SAMPLES; ++sample) {
        uint64_t before_us = time_uptime_us();
        if (before_us < started_us ||
            before_us - started_us >= DRM_VMW_FENCE_WAIT_FAST_POLL_US)
            break;

        bool sample_signaled = false;
        int sample_status = svga_fence_status(fence, &sample_signaled);
        uint64_t after_us = time_uptime_us();
        bool within_budget = after_us >= started_us &&
            after_us - started_us < DRM_VMW_FENCE_WAIT_FAST_POLL_US;

        if (sample_status && sample_status != SVGA_EBUSY) {
            *status = drm_vmwgfx_map_svga_status(sample_status);
            break;
        }
        if (sample_status == 0 && sample_signaled) {
            *signaled = true;
            break;
        }
        if (!within_budget) break;
        __asm__ volatile("pause" ::: "memory");
    }

    if (kernel_execution_resume_irqsave(&token) != 0)
        panic("drm: failed to resume FENCE_WAIT execution ownership");
    *now = time_ticks();
    return true;
}

static void drm_vmwgfx_fence_wait_finish(
    struct task *task, struct drm_vmwgfx_task_continuation *continuation)
{
    if (task && task->drm_vmwgfx_continuation == continuation)
        task->drm_vmwgfx_continuation = NULL;
    drm_vmwgfx_continuation_release(continuation);
}

static int64_t drm_vmwgfx_fence_wait(
    struct task *task, struct drm_file *file, uint32_t command,
    struct drm_vmw_fence_wait_arg *arg, uint64_t user_argument,
    struct drm_faildiag_context *diagnostics)
{
    struct drm_vmwgfx_task_continuation *continuation = task
        ? task->drm_vmwgfx_continuation : NULL;
    bool resumed = continuation &&
        continuation->kind == DRM_VMW_CONT_FENCE_WAIT &&
        drm_vmwgfx_continuation_matches(continuation, task,
            task->syscall_file, file, command, user_argument);
    if (continuation && !resumed) {
        drm_vmwgfx_cancel_task_continuation(task);
        continuation = NULL;
    }

    struct drm_vmwgfx_fence *fence = resumed
        ? continuation->fence_object
        : drm_vmwgfx_fence_lookup_get(file, arg->handle);
    if (!fence) return -LINUX_EINVAL;
    uint64_t now = time_ticks();
    uint64_t deadline;
    if (resumed) {
        *arg = continuation->snapshot.wait;
        deadline = continuation->deadline;
    } else if (!arg->cookie_valid) {
        uint64_t delta = drm_vmwgfx_timeout_ticks(arg->timeout_us);
        deadline = delta > UINT64_MAX - now ? UINT64_MAX : now + delta;
        arg->cookie_valid = 1;
        arg->kernel_cookie = deadline;
    } else {
        deadline = arg->kernel_cookie;
    }

    bool signaled = false;
    int status = drm_vmwgfx_map_svga_status(
        svga_fence_status(fence->token, &signaled));
    if (status == -LINUX_EBUSY) {
        /* The backend uses a nonblocking lock; sample contention is not a
         * completed user wait and must not be reported as fence timeout. */
        signaled = false;
        status = 0;
    }
    if (status) {
        if (resumed) drm_vmwgfx_fence_wait_finish(task, continuation);
        else drm_vmwgfx_fence_put(fence);
        return status;
    }
    if (!resumed && !signaled && now < deadline) {
        /* Only the first blocking attempt owns a fresh lookup_get reference;
         * resumed continuations retain the original fence and cookie while
         * following the same lazy/non-lazy scheduling choice below. */
        (void)drm_vmwgfx_fence_wait_fast_poll(
            fence->token, &signaled, &status, &now);
    } else if (resumed && !arg->lazy && !signaled && now < deadline) {
        /* The continuation owns the primary reference. Pin a separate local
         * reference and copy the immutable status token before dropping
         * execution ownership, then validate the original continuation only
         * after ownership is restored. */
        struct drm_vmwgfx_task_continuation *expected_continuation =
            continuation;
        struct drm_vmwgfx_fence *poll_fence = fence;
        drm_vmwgfx_fence_get(poll_fence);
        const svga_handle poll_token = poll_fence->token;
        const struct drm_vmw_fence_wait_arg poll_snapshot = *arg;
        const uint64_t poll_deadline = deadline;
        bool poll_ran = drm_vmwgfx_fence_wait_fast_poll(
            poll_token, &signaled, &status, &now);

        if (poll_ran) {
            bool continuation_valid = task->state != TASK_EXITED &&
                task->drm_vmwgfx_continuation == expected_continuation;
            if (continuation_valid) {
                const struct drm_vmw_fence_wait_arg *saved =
                    &expected_continuation->snapshot.wait;
                continuation_valid =
                    expected_continuation->kind == DRM_VMW_CONT_FENCE_WAIT &&
                    drm_vmwgfx_continuation_matches(expected_continuation,
                        task, task->syscall_file, file, command,
                        user_argument) &&
                    expected_continuation->fence_object == poll_fence &&
                    expected_continuation->fence == poll_token &&
                    expected_continuation->deadline == poll_deadline &&
                    saved->handle == poll_snapshot.handle &&
                    saved->cookie_valid == poll_snapshot.cookie_valid &&
                    saved->kernel_cookie == poll_snapshot.kernel_cookie &&
                    saved->timeout_us == poll_snapshot.timeout_us &&
                    saved->lazy == poll_snapshot.lazy &&
                    saved->flags == poll_snapshot.flags &&
                    saved->wait_options == poll_snapshot.wait_options &&
                    saved->pad64 == poll_snapshot.pad64;
            }
            drm_vmwgfx_fence_put(poll_fence);
            if (!continuation_valid) {
                /* Cancellation or task exit won while unlocked. Leave any
                 * replacement continuation untouched and return through the
                 * normal scheduler boundary; the next syscall attempt follows
                 * the existing restart/cleanup path. */
                sched_request_reschedule_current();
                return KERNEL_SYSCALL_BLOCKED;
            }
        } else {
            drm_vmwgfx_fence_put(poll_fence);
        }
    }
    if (status) {
        if (resumed) drm_vmwgfx_fence_wait_finish(task, continuation);
        else drm_vmwgfx_fence_put(fence);
        return status;
    }
    if (signaled) {
        if (arg->wait_options & DRM_VMW_WAIT_OPTION_UNREF)
            (void)drm_vmwgfx_fence_remove_retained(file, arg->handle, fence);
        if (resumed) drm_vmwgfx_fence_wait_finish(task, continuation);
        else drm_vmwgfx_fence_put(fence);
        return 0;
    }
    if (now >= deadline) {
        if (resumed) drm_vmwgfx_fence_wait_finish(task, continuation);
        else drm_vmwgfx_fence_put(fence);
        return -LINUX_EBUSY;
    }

    if (!resumed) {
        continuation = kernel_malloc(sizeof(*continuation));
        if (!continuation) {
            drm_vmwgfx_fence_put(fence);
            drm_vmwgfx_fence_diag(diagnostics, -LINUX_ENOMEM,
                                  "fence.wait.heap", 0, 0, 0);
            return -LINUX_ENOMEM;
        }
        *continuation = (struct drm_vmwgfx_task_continuation){
            .kind = DRM_VMW_CONT_FENCE_WAIT,
            .fence_object = fence,
            .fence = fence->token,
            .deadline = deadline,
            .snapshot_bytes = sizeof(*arg),
        };
        drm_vmwgfx_continuation_set_key(continuation, task, file, command,
                                         user_argument);
        task->drm_vmwgfx_continuation = continuation;
    }
    continuation->snapshot.wait = *arg;
    continuation->deadline = deadline;
    if (arg->lazy)
        sched_sleep_current_until(now + 1u);
    else
        sched_request_reschedule_current();
    return KERNEL_SYSCALL_BLOCKED;
}

static int64_t drm_vmwgfx_synccpu(
    struct task *task, struct drm_file *file, uint32_t command,
    struct drm_vmw_synccpu_arg *arg, uint64_t user_argument)
{
    if (arg->op == drm_vmw_synccpu_release)
        return drm_bo_synccpu_ioctl(file, arg);

    struct drm_vmwgfx_task_continuation *continuation = task
        ? task->drm_vmwgfx_continuation : NULL;
    bool resumed = continuation &&
        continuation->kind == DRM_VMW_CONT_SYNCCPU &&
        drm_vmwgfx_continuation_matches(continuation, task,
            task->syscall_file, file, command, user_argument);
    if (continuation && !resumed) {
        drm_vmwgfx_cancel_task_continuation(task);
        continuation = NULL;
    }

    struct drm_bo *bo = NULL;
    if (resumed) {
        *arg = continuation->snapshot.synccpu;
        bo = continuation->bo;
    } else {
        int status = drm_bo_synccpu_lookup_get(file, arg->handle,
                                                arg->flags, &bo);
        if (status) return status;
    }

    bool busy = drm_bo_gpu_busy(bo);
    if (!busy) {
        int status = drm_bo_synccpu_grab_retained(file, arg->handle, bo,
                                                  arg->flags);
        if (!status) {
            if (resumed) drm_vmwgfx_fence_wait_finish(task, continuation);
            else drm_bo_put(bo);
            return 0;
        }
        if (status != -LINUX_EBUSY) {
            if (resumed) drm_vmwgfx_fence_wait_finish(task, continuation);
            else drm_bo_put(bo);
            return status;
        }
        busy = true;
    }
    if (arg->flags & drm_vmw_synccpu_dontblock) {
        if (resumed) drm_vmwgfx_fence_wait_finish(task, continuation);
        else drm_bo_put(bo);
        return -LINUX_EBUSY;
    }

    if (!resumed) {
        continuation = kernel_malloc(sizeof(*continuation));
        if (!continuation) {
            drm_bo_put(bo);
            return -LINUX_ENOMEM;
        }
        *continuation = (struct drm_vmwgfx_task_continuation){
            .kind = DRM_VMW_CONT_SYNCCPU,
            .bo = bo,
            .snapshot_bytes = sizeof(*arg),
        };
        drm_vmwgfx_continuation_set_key(continuation, task, file, command,
                                         user_argument);
        task->drm_vmwgfx_continuation = continuation;
    }
    continuation->snapshot.synccpu = *arg;
    sched_sleep_current_until(time_ticks() + 1u);
    return KERNEL_SYSCALL_BLOCKED;
}

int64_t drm_vmwgfx_execbuf_ioctl(struct task *task, struct drm_file *file,
                                 uint32_t command, void *data,
                                 uint64_t user_argument,
                                 struct drm_faildiag_context *diagnostics)
{
    if (!task || !file || !data) return -LINUX_EFAULT;
    if (_IOC_NR(command) < DRM_COMMAND_BASE) return -LINUX_EINVAL;
    switch (_IOC_NR(command) - DRM_COMMAND_BASE) {
    case DRM_VMW_EXECBUF:
        return drm_vmwgfx_execbuf(task, file, command, data, user_argument,
                                  diagnostics);
    case DRM_VMW_FENCE_SIGNALED:
        if (_IOC_DIR(command) != (_IOC_READ | _IOC_WRITE) ||
            _IOC_SIZE(command) != sizeof(struct drm_vmw_fence_signaled_arg))
            return -LINUX_EINVAL;
        return drm_vmwgfx_fence_signaled(file, data);
    case DRM_VMW_FENCE_WAIT:
        if (_IOC_DIR(command) != (_IOC_READ | _IOC_WRITE) ||
            _IOC_SIZE(command) != sizeof(struct drm_vmw_fence_wait_arg))
            return -LINUX_EINVAL;
        return drm_vmwgfx_fence_wait(task, file, command, data,
                                     user_argument, diagnostics);
    case DRM_VMW_FENCE_UNREF: {
        struct drm_vmw_fence_arg *arg = data;
        if (_IOC_DIR(command) != _IOC_WRITE ||
            _IOC_SIZE(command) != sizeof(*arg)) return -LINUX_EINVAL;
        return drm_vmwgfx_fence_remove(file, arg->handle) ? 0 : -LINUX_EINVAL;
    }
    case DRM_VMW_SYNCCPU:
        if (_IOC_DIR(command) != _IOC_WRITE ||
            _IOC_SIZE(command) != sizeof(struct drm_vmw_synccpu_arg))
            return -LINUX_EINVAL;
        return drm_vmwgfx_synccpu(task, file, command, data, user_argument);
    default:
        return -LINUX_EINVAL;
    }
}

void drm_vmwgfx_execbuf_file_release(struct drm_file *file)
{
    if (!file) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_fence_lock, &flags);
    struct drm_vmwgfx_fence *fences =
        (struct drm_vmwgfx_fence *)file->vmwgfx_fences;
    file->vmwgfx_fences = NULL;
    struct drm_vmwgfx_fence *free_list = NULL;
    while (fences) {
        struct drm_vmwgfx_fence *next = fences->next;
        fences->next = NULL;
        fences->user_linked = false;
        if (fences->references && --fences->references == 0) {
            if (drm_vmwgfx_fence_count) --drm_vmwgfx_fence_count;
            fences->next = free_list;
            free_list = fences;
        }
        fences = next;
    }
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_fence_lock, flags);
    while (free_list) {
        struct drm_vmwgfx_fence *next = free_list->next;
        kernel_free(free_list);
        free_list = next;
    }
}

void drm_vmwgfx_cancel_task_continuation(struct task *task)
{
    if (!task) return;
    struct drm_vmwgfx_task_continuation *continuation =
        task->drm_vmwgfx_continuation;
    if (!continuation) return;
    task->drm_vmwgfx_continuation = NULL;
    drm_vmwgfx_continuation_queue_release(continuation);
}
