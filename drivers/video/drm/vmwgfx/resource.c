#include "../drm_internal.h"

#include <drm/vmwgfx_drm.h>
#include <linux/errno.h>
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/usercopy.h>

#include "../../svga/svga_protocol.h"

#define DRM_VMW_RESOURCE_HANDLE_MIN 0x01000000u
#define DRM_VMW_RESOURCE_HANDLE_MAX 0x7fffffffu
#define DRM_VMW_SURFACE_MAX_MIPS_PER_FACE 16u
#define DRM_VMW_SURFACE_COPY_MAX_BYTES \
    (DRM_VMW_MAX_SURFACE_FACES * DRM_VMW_SURFACE_MAX_MIPS_PER_FACE * \
     sizeof(struct drm_vmw_size))
#define DRM_VMW_RESOURCE_RETIRE_BUDGET 2u

enum drm_vmwgfx_resource_type {
    DRM_VMW_RESOURCE_CONTEXT = 1,
    DRM_VMW_RESOURCE_SURFACE = 2,
};

enum drm_vmwgfx_resource_state {
    DRM_VMW_RESOURCE_CREATING,
    DRM_VMW_RESOURCE_LIVE,
    DRM_VMW_RESOURCE_RETIRING,
    DRM_VMW_RESOURCE_PROCESSING,
};

struct drm_vmwgfx_resource {
    struct drm_vmwgfx_resource *all_next;
    struct drm_vmwgfx_resource *retire_next;
    uint32_t id;
    uint32_t logical_references;
    uint32_t retained_references;
    /* One live same-device PRIME OFD is an importable object reference, not
     * merely an internal retained pointer. It keeps the typed handle live
     * after the exporter drops its per-file handle. */
    uint32_t prime_references;
    uint8_t type;
    uint8_t state;
    bool queued;
    svga_handle backend;
    struct drm_master *creator_master;
};

struct drm_vmwgfx_context {
    struct drm_vmwgfx_resource resource;
};

struct drm_vmwgfx_surface {
    struct drm_vmwgfx_resource resource;
    struct drm_vmwgfx_surface_metadata metadata;
    struct task_file *prime_description; /* weak; protected by resource_lock */
};

struct drm_vmwgfx_file_ref {
    struct drm_vmwgfx_file_ref *next;
    struct drm_vmwgfx_resource *resource;
    uint32_t references;
};

static struct kernel_spinlock drm_vmwgfx_resource_lock = KERNEL_SPINLOCK_INIT;
static struct drm_vmwgfx_resource *drm_vmwgfx_all_resources;
static struct drm_vmwgfx_resource *drm_vmwgfx_retire_head;
static struct drm_vmwgfx_resource *drm_vmwgfx_retire_tail;
/* Published under resource_lock. Only the empty observation is lock-free;
 * popping, requeuing and all resource lifetimes still use that lock. */
static bool drm_vmwgfx_retire_pending;
static uint32_t drm_vmwgfx_next_handle = DRM_VMW_RESOURCE_HANDLE_MIN;

static struct drm_vmwgfx_file_ref *drm_vmwgfx_file_ref_find_locked(
    const struct drm_file *file, uint32_t handle,
    enum drm_vmwgfx_resource_type type)
{
    for (struct drm_vmwgfx_file_ref *ref = file ? file->vmwgfx_resources : NULL;
         ref; ref = ref->next) {
        if (ref->resource->id == handle && ref->resource->type == type)
            return ref;
    }
    return NULL;
}

static struct drm_vmwgfx_resource *drm_vmwgfx_find_locked(
    uint32_t handle, enum drm_vmwgfx_resource_type type)
{
    for (struct drm_vmwgfx_resource *resource = drm_vmwgfx_all_resources;
         resource; resource = resource->all_next) {
        if (resource->id == handle && resource->type == type &&
            resource->state == DRM_VMW_RESOURCE_LIVE)
            return resource;
    }
    return NULL;
}

static bool drm_vmwgfx_handle_in_use_locked(uint32_t handle)
{
    for (struct drm_vmwgfx_resource *resource = drm_vmwgfx_all_resources;
         resource; resource = resource->all_next) {
        if (resource->id == handle) return true;
    }
    return false;
}

static int drm_vmwgfx_reserve_resource(struct drm_vmwgfx_resource *resource)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
    uint32_t candidate = drm_vmwgfx_next_handle;
    bool found = false;
    do {
        if (candidate < DRM_VMW_RESOURCE_HANDLE_MIN ||
            candidate > DRM_VMW_RESOURCE_HANDLE_MAX)
            candidate = DRM_VMW_RESOURCE_HANDLE_MIN;
        if (!drm_vmwgfx_handle_in_use_locked(candidate)) {
            found = true;
            break;
        }
        ++candidate;
    } while (candidate != drm_vmwgfx_next_handle);
    if (!found) {
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
        return -LINUX_ENOMEM;
    }
    resource->id = candidate;
    resource->state = DRM_VMW_RESOURCE_CREATING;
    resource->all_next = drm_vmwgfx_all_resources;
    drm_vmwgfx_all_resources = resource;
    drm_vmwgfx_next_handle = candidate == DRM_VMW_RESOURCE_HANDLE_MAX
        ? DRM_VMW_RESOURCE_HANDLE_MIN : candidate + 1u;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
    return 0;
}

static void drm_vmwgfx_remove_all_locked(struct drm_vmwgfx_resource *resource)
{
    struct drm_vmwgfx_resource **link = &drm_vmwgfx_all_resources;
    while (*link && *link != resource) link = &(*link)->all_next;
    if (*link == resource) *link = resource->all_next;
    resource->all_next = NULL;
}

static void drm_vmwgfx_queue_locked(struct drm_vmwgfx_resource *resource)
{
    if (resource->queued) return;
    if (resource->logical_references || resource->prime_references) return;
    resource->state = DRM_VMW_RESOURCE_RETIRING;
    resource->queued = true;
    resource->retire_next = NULL;
    if (drm_vmwgfx_retire_tail)
        drm_vmwgfx_retire_tail->retire_next = resource;
    else
        drm_vmwgfx_retire_head = resource;
    drm_vmwgfx_retire_tail = resource;
    __atomic_store_n(&drm_vmwgfx_retire_pending, true, __ATOMIC_RELEASE);
}

static void drm_vmwgfx_queue_resource(struct drm_vmwgfx_resource *resource)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
    drm_vmwgfx_queue_locked(resource);
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
}

static void drm_vmwgfx_discard_unpublished(
    struct drm_vmwgfx_resource *resource)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
    drm_vmwgfx_remove_all_locked(resource);
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
    drm_master_put(resource->creator_master);
    kernel_free(resource);
}

static struct drm_vmwgfx_resource *drm_vmwgfx_resource_allocate(
    enum drm_vmwgfx_resource_type type, size_t size,
    struct drm_master *creator_master)
{
    struct drm_vmwgfx_resource *resource = kernel_malloc(size);
    if (!resource) return NULL;
    __builtin_memset(resource, 0, size);
    resource->type = (uint8_t)type;
    resource->creator_master = creator_master;
    drm_master_get(creator_master);
    return resource;
}

static void drm_vmwgfx_create_failed(struct drm_vmwgfx_resource *resource,
                                     int backend_status)
{
    if (resource->backend) {
        /* A typed try-create may retain a partial resource on failure. Keep
         * its record and creator-master reference until nonblocking retirement
         * confirms that the backend no longer owns it. */
        drm_vmwgfx_queue_resource(resource);
        return;
    }
    (void)backend_status;
    drm_vmwgfx_discard_unpublished(resource);
}

static void drm_vmwgfx_publish_resource(struct drm_file *file,
                                        struct drm_vmwgfx_resource *resource,
                                        struct drm_vmwgfx_file_ref *ref)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
    resource->state = DRM_VMW_RESOURCE_LIVE;
    resource->logical_references = 1;
    ref->resource = resource;
    ref->references = 1;
    ref->next = file->vmwgfx_resources;
    file->vmwgfx_resources = ref;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
}

static int drm_vmwgfx_add_surface_ref(struct drm_file *file,
                                      struct drm_vmwgfx_resource *resource,
                                      struct drm_vmwgfx_rollback_receipt *receipt)
{
    struct drm_vmwgfx_file_ref *new_ref = NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
    struct drm_vmwgfx_file_ref *ref =
        drm_vmwgfx_file_ref_find_locked(file, resource->id,
                                        DRM_VMW_RESOURCE_SURFACE);
    if (resource->state != DRM_VMW_RESOURCE_LIVE) {
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
        return -LINUX_EINVAL;
    }
    if (resource->logical_references == UINT32_MAX ||
        (ref && ref->references == UINT32_MAX)) {
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
        return -LINUX_EOVERFLOW;
    }
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);

    if (!ref) {
        new_ref = kernel_malloc(sizeof(*new_ref));
        if (!new_ref) return -LINUX_ENOMEM;
        *new_ref = (struct drm_vmwgfx_file_ref){0};
    }

    kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
    ref = drm_vmwgfx_file_ref_find_locked(file, resource->id,
                                          DRM_VMW_RESOURCE_SURFACE);
    if (resource->state != DRM_VMW_RESOURCE_LIVE) {
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
        kernel_free(new_ref);
        return -LINUX_EINVAL;
    }
    if (resource->logical_references == UINT32_MAX ||
        (ref && ref->references == UINT32_MAX)) {
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
        kernel_free(new_ref);
        return -LINUX_EOVERFLOW;
    }
    if (ref) {
        ++ref->references;
    } else {
        new_ref->resource = resource;
        new_ref->references = 1;
        new_ref->next = file->vmwgfx_resources;
        file->vmwgfx_resources = new_ref;
    }
    ++resource->logical_references;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
    if (receipt) {
        *receipt = (struct drm_vmwgfx_rollback_receipt){
            .handle = resource->id,
            .type = DRM_VMW_RESOURCE_SURFACE,
            .active = true,
        };
    }
    return 0;
}

int drm_vmwgfx_surface_add_file_reference(
    struct drm_file *file, struct drm_vmwgfx_surface *surface,
    struct drm_vmwgfx_rollback_receipt *receipt)
{
    if (!file || !surface || surface->resource.type != DRM_VMW_RESOURCE_SURFACE)
        return -LINUX_EINVAL;
    return drm_vmwgfx_add_surface_ref(file, &surface->resource, receipt);
}

static int drm_vmwgfx_drop_file_reference(
    struct drm_file *file, uint32_t handle,
    enum drm_vmwgfx_resource_type type)
{
    struct drm_vmwgfx_file_ref *removed = NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
    struct drm_vmwgfx_file_ref **link = &file->vmwgfx_resources;
    while (*link && ((*link)->resource->id != handle ||
                     (*link)->resource->type != type))
        link = &(*link)->next;
    if (!*link || (*link)->resource->state != DRM_VMW_RESOURCE_LIVE ||
        !(*link)->references || !(*link)->resource->logical_references) {
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
        return -LINUX_EINVAL;
    }
    struct drm_vmwgfx_resource *resource = (*link)->resource;
    --(*link)->references;
    --resource->logical_references;
    if (!(*link)->references) {
        removed = *link;
        *link = removed->next;
    }
    if (!resource->logical_references && !resource->prime_references)
        drm_vmwgfx_queue_locked(resource);
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
    kernel_free(removed);
    return 0;
}

static bool drm_vmwgfx_mips_are_canonical(
    const struct drm_vmw_size *sizes, uint32_t levels,
    uint32_t width, uint32_t height, uint32_t depth)
{
    if (!sizes || !levels || !width || !height || !depth) return false;
    for (uint32_t mip = 0; mip < levels; ++mip) {
        if (sizes[mip].width != width || sizes[mip].height != height ||
            sizes[mip].depth != depth)
            return false;
        if (width > 1u) width >>= 1;
        if (height > 1u) height >>= 1;
        if (depth > 1u) depth >>= 1;
    }
    return true;
}

static int drm_vmwgfx_validate_surface(
    struct task *task, const struct drm_vmw_surface_create_req *request,
    struct drm_vmwgfx_surface_metadata *metadata)
{
    const uint32_t allowed_flags = SVGA3D_SURFACE_CUBEMAP |
        SVGA3D_SURFACE_HINT_STATIC | SVGA3D_SURFACE_HINT_DYNAMIC |
        SVGA3D_SURFACE_HINT_INDEXBUFFER | SVGA3D_SURFACE_HINT_VERTEXBUFFER |
        SVGA3D_SURFACE_HINT_TEXTURE | SVGA3D_SURFACE_HINT_RENDERTARGET |
        SVGA3D_SURFACE_HINT_DEPTHSTENCIL | SVGA3D_SURFACE_HINT_WRITEONLY |
        SVGA3D_SURFACE_MASKABLE_ANTIALIAS | SVGA3D_SURFACE_AUTOGENMIPMAPS |
        SVGA3D_SURFACE_BIND_VERTEX_BUFFER |
        SVGA3D_SURFACE_BIND_INDEX_BUFFER |
        SVGA3D_SURFACE_BIND_CONSTANT_BUFFER |
        SVGA3D_SURFACE_BIND_SHADER_RESOURCE |
        SVGA3D_SURFACE_BIND_RENDER_TARGET |
        SVGA3D_SURFACE_BIND_DEPTH_STENCIL;
    const uint32_t faces = (request->flags & SVGA3D_SURFACE_CUBEMAP) ? 6u : 1u;
    uint64_t total_levels = 0;
    uint32_t levels = 0;
    uint32_t size_bytes;
    struct drm_vmw_size *sizes;
    uint32_t width, height, depth;
    if (request->flags & ~allowed_flags)
        return -LINUX_EINVAL;

    for (uint32_t face = 0; face < DRM_VMW_MAX_SURFACE_FACES; ++face) {
        uint32_t count = request->mip_levels[face];
        if (face < faces) {
            if (!count || count > DRM_VMW_SURFACE_MAX_MIPS_PER_FACE ||
                (levels && count != levels))
                return -LINUX_EINVAL;
            levels = count;
            if (total_levels > UINT64_MAX - count) return -LINUX_EINVAL;
            total_levels += count;
        } else if (count) {
            return -LINUX_EINVAL;
        }
    }
    if (!total_levels || total_levels > UINT32_MAX ||
        total_levels > DRM_VMW_SURFACE_COPY_MAX_BYTES /
                       sizeof(struct drm_vmw_size))
        return -LINUX_EINVAL;
    if (total_levels > UINT32_MAX / sizeof(struct drm_vmw_size))
        return -LINUX_EINVAL;
    size_bytes = (uint32_t)total_levels * sizeof(struct drm_vmw_size);
    if (!request->size_addr || !user_range_ok(request->size_addr, size_bytes))
        return -LINUX_EFAULT;
    sizes = kernel_malloc(DRM_VMW_SURFACE_COPY_MAX_BYTES);
    if (!sizes) return -LINUX_ENOMEM;
    if (user_copy_from_task(task, sizes, request->size_addr, size_bytes) < 0) {
        kernel_free(sizes);
        return -LINUX_EFAULT;
    }

    width = sizes[0].width;
    height = sizes[0].height;
    depth = sizes[0].depth;
    bool byte_buffer = request->format == SVGA3D_BUFFER;
    if (!width || !height || !depth ||
        (!byte_buffer && (width > 16384u || height > 16384u || depth > 256u)) ||
        ((request->flags & SVGA3D_SURFACE_CUBEMAP) &&
         (width != height || depth != 1u)) ||
        (byte_buffer &&
         (height != 1u || depth != 1u || levels != 1u || faces != 1u))) {
        kernel_free(sizes);
        return -LINUX_EINVAL;
    }
    uint32_t largest = width > height ? width : height;
    if (depth > largest) largest = depth;
    uint32_t max_levels = 1u;
    while (largest > 1u) {
        largest >>= 1;
        ++max_levels;
    }
    if (levels > max_levels) {
        kernel_free(sizes);
        return -LINUX_EINVAL;
    }

    uint32_t face_offset = 0;
    for (uint32_t face = 0; face < faces; ++face) {
        if (!drm_vmwgfx_mips_are_canonical(sizes + face_offset, levels,
                                          width, height, depth)) {
            kernel_free(sizes);
            return -LINUX_EINVAL;
        }
        face_offset += levels;
    }
    kernel_free(sizes);

    *metadata = (struct drm_vmwgfx_surface_metadata){
        .desc = {
            .format = request->format,
            .flags = request->flags,
            .width = width,
            .height = height,
            .depth = depth,
            .mip_levels = levels,
        },
        .mip_levels = {levels, faces > 1u ? levels : 0u,
                       faces > 2u ? levels : 0u,
                       faces > 3u ? levels : 0u,
                       faces > 4u ? levels : 0u,
                       faces > 5u ? levels : 0u},
        .shareable = request->shareable != 0,
        .scanout = request->scanout != 0,
    };
    return 0;
}

static int drm_vmwgfx_create_context(struct drm_file *file,
                                     struct drm_vmw_context_arg *argument,
                                     struct drm_vmwgfx_rollback_receipt *receipt)
{
    struct drm_vmwgfx_context *context =
        (struct drm_vmwgfx_context *)drm_vmwgfx_resource_allocate(
            DRM_VMW_RESOURCE_CONTEXT, sizeof(*context), file->master);
    struct drm_vmwgfx_file_ref *ref = kernel_malloc(sizeof(*ref));
    if (!context || !ref) {
        if (context) drm_master_put(context->resource.creator_master);
        kernel_free(context);
        kernel_free(ref);
        return -LINUX_ENOMEM;
    }
    *ref = (struct drm_vmwgfx_file_ref){0};
    int ret = drm_vmwgfx_reserve_resource(&context->resource);
    if (ret) {
        drm_master_put(context->resource.creator_master);
        kernel_free(context);
        kernel_free(ref);
        return ret;
    }
    ret = svga3d_context_try_create(&context->resource.backend);
    if (ret) {
        drm_vmwgfx_create_failed(&context->resource, ret);
        kernel_free(ref);
        return ret;
    }
    drm_vmwgfx_publish_resource(file, &context->resource, ref);
    argument->cid = (int32_t)context->resource.id;
    argument->pad64 = 0;
    *receipt = (struct drm_vmwgfx_rollback_receipt){
        .handle = context->resource.id,
        .type = DRM_VMW_RESOURCE_CONTEXT,
        .active = true,
    };
    return 0;
}

static int drm_vmwgfx_create_surface(
    struct task *task, struct drm_file *file,
    union drm_vmw_surface_create_arg *argument,
    struct drm_vmwgfx_rollback_receipt *receipt)
{
    struct drm_vmwgfx_surface_metadata metadata;
    int ret = drm_vmwgfx_validate_surface(task, &argument->req, &metadata);
    if (ret) return ret;

    struct drm_vmwgfx_surface *surface =
        (struct drm_vmwgfx_surface *)drm_vmwgfx_resource_allocate(
            DRM_VMW_RESOURCE_SURFACE, sizeof(*surface), file->master);
    struct drm_vmwgfx_file_ref *ref = kernel_malloc(sizeof(*ref));
    if (!surface || !ref) {
        if (surface) drm_master_put(surface->resource.creator_master);
        kernel_free(surface);
        kernel_free(ref);
        return -LINUX_ENOMEM;
    }
    *ref = (struct drm_vmwgfx_file_ref){0};
    surface->metadata = metadata;
    ret = drm_vmwgfx_reserve_resource(&surface->resource);
    if (ret) {
        drm_master_put(surface->resource.creator_master);
        kernel_free(surface);
        kernel_free(ref);
        return ret;
    }
    ret = svga3d_surface_try_create(&metadata.desc, &surface->resource.backend);
    if (ret) {
        drm_vmwgfx_create_failed(&surface->resource, ret);
        kernel_free(ref);
        return ret;
    }
    surface->metadata.handle = surface->resource.id;
    drm_vmwgfx_publish_resource(file, &surface->resource, ref);
    argument->rep.sid = (int32_t)surface->resource.id;
    argument->rep.handle_type = DRM_VMW_HANDLE_LEGACY;
    *receipt = (struct drm_vmwgfx_rollback_receipt){
        .handle = surface->resource.id,
        .type = DRM_VMW_RESOURCE_SURFACE,
        .active = true,
    };
    return 0;
}

static int drm_vmwgfx_create_surface_reply(
    struct drm_vmw_surface_create_req *reply,
    const struct drm_vmwgfx_surface_metadata *metadata)
{
    reply->flags = metadata->desc.flags;
    reply->format = metadata->desc.format;
    __builtin_memcpy(reply->mip_levels, metadata->mip_levels,
                     sizeof(reply->mip_levels));
    reply->shareable = metadata->shareable ? 1 : 0;
    reply->scanout = metadata->scanout ? 1 : 0;
    return 0;
}

static int drm_vmwgfx_ref_surface(
    struct task *task, struct drm_file *file,
    union drm_vmw_surface_reference_arg *argument,
    struct drm_vmwgfx_rollback_receipt *receipt)
{
    if (argument->req.handle_type == DRM_VMW_HANDLE_PRIME) {
        struct drm_vmwgfx_surface *surface = NULL;
        int ret = drm_prime_surface_from_fd(task, argument->req.sid,
                                             file->device, &surface);
        if (ret) return ret;
        ret = drm_vmwgfx_surface_add_file_reference(file, surface, receipt);
        if (ret) {
            drm_vmwgfx_surface_put(surface);
            return ret;
        }
        uint64_t size_address = argument->rep.size_addr;
        drm_vmwgfx_create_surface_reply(&argument->rep, &surface->metadata);
        if (size_address) {
            struct drm_vmw_size base_size = {
                .width = surface->metadata.desc.width,
                .height = surface->metadata.desc.height,
                .depth = surface->metadata.desc.depth,
                .pad64 = 0,
            };
            if (!user_range_writable(size_address, sizeof(base_size)) ||
                user_copy_to_task(task, size_address, &base_size,
                                  sizeof(base_size)) < 0) {
                (void)drm_vmwgfx_drop_file_reference(
                    file, surface->resource.id, DRM_VMW_RESOURCE_SURFACE);
                if (receipt) receipt->active = false;
                drm_vmwgfx_surface_put(surface);
                return -LINUX_EFAULT;
            }
        }
        drm_vmwgfx_surface_put(surface);
        return 0;
    }
    if (argument->req.handle_type != DRM_VMW_HANDLE_LEGACY ||
        argument->req.sid <= 0)
        return -LINUX_EINVAL;

    uint32_t handle = (uint32_t)argument->req.sid;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
    struct drm_vmwgfx_resource *resource =
        drm_vmwgfx_find_locked(handle, DRM_VMW_RESOURCE_SURFACE);
    if (!resource) {
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
        return -LINUX_EINVAL;
    }
    bool already_held = drm_vmwgfx_file_ref_find_locked(
        file, handle, DRM_VMW_RESOURCE_SURFACE) != NULL;
    bool same_master = file->master && resource->creator_master &&
                       file->master == resource->creator_master;
    bool authorized = file->is_render
        ? already_held
        : file->authenticated && (same_master || already_held);
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
    if (!authorized) return -LINUX_EACCES;

    int ret = drm_vmwgfx_add_surface_ref(file, resource, receipt);
    if (ret) return ret;
    struct drm_vmwgfx_surface *surface =
        (struct drm_vmwgfx_surface *)resource;
    uint64_t size_address = argument->rep.size_addr;
    drm_vmwgfx_create_surface_reply(&argument->rep, &surface->metadata);
    if (size_address) {
        struct drm_vmw_size base_size = {
            .width = surface->metadata.desc.width,
            .height = surface->metadata.desc.height,
            .depth = surface->metadata.desc.depth,
            .pad64 = 0,
        };
        if (!user_range_writable(size_address, sizeof(base_size)) ||
            user_copy_to_task(task, size_address, &base_size,
                              sizeof(base_size)) < 0) {
            (void)drm_vmwgfx_drop_file_reference(
                file, handle, DRM_VMW_RESOURCE_SURFACE);
            if (receipt) receipt->active = false;
            return -LINUX_EFAULT;
        }
    }
    return 0;
}

int drm_vmwgfx_resource_ioctl(struct task *task, struct drm_file *file,
                              uint32_t command, void *data,
    struct drm_vmwgfx_rollback_receipt *receipt)
{
    if (!task || !file || !data || !receipt) return -LINUX_EINVAL;
    *receipt = (struct drm_vmwgfx_rollback_receipt){0};
    switch (_IOC_NR(command) - DRM_COMMAND_BASE) {
    case DRM_VMW_CREATE_CONTEXT:
        return drm_vmwgfx_create_context(file, data, receipt);
    case DRM_VMW_UNREF_CONTEXT: {
        const struct drm_vmw_context_arg *argument = data;
        if (argument->cid <= 0) return -LINUX_EINVAL;
        return drm_vmwgfx_drop_file_reference(
            file, (uint32_t)argument->cid, DRM_VMW_RESOURCE_CONTEXT);
    }
    case DRM_VMW_CREATE_SURFACE:
        return drm_vmwgfx_create_surface(task, file, data, receipt);
    case DRM_VMW_UNREF_SURFACE: {
        const struct drm_vmw_surface_arg *argument = data;
        if (argument->sid <= 0) return -LINUX_EINVAL;
        /* Linux unrefs by the sid alone; handle_type is ignored here. */
        return drm_vmwgfx_drop_file_reference(
            file, (uint32_t)argument->sid, DRM_VMW_RESOURCE_SURFACE);
    }
    case DRM_VMW_REF_SURFACE:
        return drm_vmwgfx_ref_surface(task, file, data, receipt);
    default:
        return -LINUX_EINVAL;
    }
}

void drm_vmwgfx_resource_rollback(
    struct drm_file *file, struct drm_vmwgfx_rollback_receipt *receipt)
{
    if (!file || !receipt || !receipt->active) return;
    enum drm_vmwgfx_resource_type type =
        (enum drm_vmwgfx_resource_type)receipt->type;
    if (type == DRM_VMW_RESOURCE_CONTEXT || type == DRM_VMW_RESOURCE_SURFACE)
        (void)drm_vmwgfx_drop_file_reference(file, receipt->handle, type);
    receipt->active = false;
}

void drm_vmwgfx_resource_file_release(struct drm_file *file)
{
    if (!file) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
    struct drm_vmwgfx_file_ref *refs = file->vmwgfx_resources;
    file->vmwgfx_resources = NULL;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);

    while (refs) {
        struct drm_vmwgfx_file_ref *next = refs->next;
        struct drm_vmwgfx_resource *resource = refs->resource;
        kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
        if (resource->state == DRM_VMW_RESOURCE_LIVE &&
            refs->references <= resource->logical_references) {
            resource->logical_references -= refs->references;
            if (!resource->logical_references && !resource->prime_references)
                drm_vmwgfx_queue_locked(resource);
        }
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
        kernel_free(refs);
        refs = next;
    }
}

static struct drm_vmwgfx_resource *drm_vmwgfx_retire_pop_locked(void)
{
    struct drm_vmwgfx_resource *resource = drm_vmwgfx_retire_head;
    if (!resource) return NULL;
    drm_vmwgfx_retire_head = resource->retire_next;
    if (!drm_vmwgfx_retire_head) drm_vmwgfx_retire_tail = NULL;
    __atomic_store_n(&drm_vmwgfx_retire_pending,
                     drm_vmwgfx_retire_head != NULL, __ATOMIC_RELEASE);
    resource->retire_next = NULL;
    resource->queued = false;
    return resource;
}

uint32_t drm_vmwgfx_process_retirements(uint32_t budget)
{
    if (budget > DRM_VMW_RESOURCE_RETIRE_BUDGET)
        budget = DRM_VMW_RESOURCE_RETIRE_BUDGET;
    /* A retained surface DIRTYFB may be the only pending GPU work. Service
     * it before the resource queue's empty fast path, on the scheduler's
     * existing task-context wakeup. */
    drm_kms_process_retirements(budget);
    uint32_t retired = 0;
    uint32_t shader_reaped = 0;
    (void)svga_backend_reap_retired(budget, &shader_reaped);
    drm_vmwgfx_execbuf_process_retirements(budget);
    if (!__atomic_load_n(&drm_vmwgfx_retire_pending, __ATOMIC_ACQUIRE))
        return 0;
    for (uint32_t attempt = 0; attempt < budget; ++attempt) {
        uint64_t flags;
        kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
        struct drm_vmwgfx_resource *resource =
            drm_vmwgfx_retire_pop_locked();
        if (!resource) {
            kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
            return retired;
        }
        if (resource->state != DRM_VMW_RESOURCE_RETIRING ||
            resource->logical_references || resource->prime_references ||
            resource->retained_references) {
            drm_vmwgfx_queue_locked(resource);
            kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
            continue;
        }
        resource->state = DRM_VMW_RESOURCE_PROCESSING;
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);

        int ret = 0;
        if (resource->backend) {
            if (resource->type == DRM_VMW_RESOURCE_CONTEXT)
                ret = svga3d_context_try_destroy(resource->backend);
            else if (resource->type == DRM_VMW_RESOURCE_SURFACE)
                ret = svga3d_surface_try_destroy(resource->backend);
            else
                ret = -LINUX_EINVAL;
        }

        kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
        if (ret) {
            /* Rotate every retry to the tail. A busy first object cannot
             * starve later resources when unrelated syscalls advance reaping. */
            resource->state = DRM_VMW_RESOURCE_RETIRING;
            drm_vmwgfx_queue_locked(resource);
            kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
            continue;
        }
        drm_vmwgfx_remove_all_locked(resource);
        resource->state = DRM_VMW_RESOURCE_CREATING;
        kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
        drm_master_put(resource->creator_master);
        kernel_free(resource);
        ++retired;
    }
    return retired;
}

static struct drm_vmwgfx_resource *drm_vmwgfx_lookup_get(
    struct drm_file *file, uint32_t handle,
    enum drm_vmwgfx_resource_type type)
{
    if (!file || handle < DRM_VMW_RESOURCE_HANDLE_MIN) return NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
    struct drm_vmwgfx_file_ref *ref =
        drm_vmwgfx_file_ref_find_locked(file, handle, type);
    struct drm_vmwgfx_resource *resource = ref ? ref->resource : NULL;
    if (!resource || resource->state != DRM_VMW_RESOURCE_LIVE ||
        resource->retained_references == UINT32_MAX)
        resource = NULL;
    else
        ++resource->retained_references;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
    return resource;
}

static int drm_vmwgfx_resource_get(struct drm_vmwgfx_resource *resource,
                                   enum drm_vmwgfx_resource_type type)
{
    if (!resource || resource->type != type) return -LINUX_EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
    int ret = 0;
    if (resource->state != DRM_VMW_RESOURCE_LIVE ||
        resource->retained_references == UINT32_MAX)
        ret = -LINUX_EINVAL;
    else
        ++resource->retained_references;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
    return ret;
}

static void drm_vmwgfx_resource_put(struct drm_vmwgfx_resource *resource,
                                    enum drm_vmwgfx_resource_type type)
{
    if (!resource || resource->type != type) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
    if (resource->retained_references) --resource->retained_references;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
}

struct drm_vmwgfx_context *drm_vmwgfx_context_lookup_get(
    struct drm_file *file, uint32_t handle)
{
    return (struct drm_vmwgfx_context *)drm_vmwgfx_lookup_get(
        file, handle, DRM_VMW_RESOURCE_CONTEXT);
}

int drm_vmwgfx_context_get(struct drm_vmwgfx_context *context)
{
    return drm_vmwgfx_resource_get(context ? &context->resource : NULL,
                                   DRM_VMW_RESOURCE_CONTEXT);
}

void drm_vmwgfx_context_put(struct drm_vmwgfx_context *context)
{
    drm_vmwgfx_resource_put(context ? &context->resource : NULL,
                            DRM_VMW_RESOURCE_CONTEXT);
}

svga_handle drm_vmwgfx_context_backend_handle(
    const struct drm_vmwgfx_context *context)
{
    return context ? context->resource.backend : SVGA_INVALID_HANDLE;
}

uint32_t drm_vmwgfx_context_backend_id(
    const struct drm_vmwgfx_context *context)
{
    return context ? svga_resource_id(context->resource.backend) : UINT32_MAX;
}

struct drm_vmwgfx_surface *drm_vmwgfx_surface_lookup_get(
    struct drm_file *file, uint32_t handle)
{
    return (struct drm_vmwgfx_surface *)drm_vmwgfx_lookup_get(
        file, handle, DRM_VMW_RESOURCE_SURFACE);
}

int drm_vmwgfx_surface_get(struct drm_vmwgfx_surface *surface)
{
    return drm_vmwgfx_resource_get(surface ? &surface->resource : NULL,
                                   DRM_VMW_RESOURCE_SURFACE);
}

void drm_vmwgfx_surface_put(struct drm_vmwgfx_surface *surface)
{
    drm_vmwgfx_resource_put(surface ? &surface->resource : NULL,
                            DRM_VMW_RESOURCE_SURFACE);
}

svga_handle drm_vmwgfx_surface_backend_handle(
    const struct drm_vmwgfx_surface *surface)
{
    return surface ? surface->resource.backend : SVGA_INVALID_HANDLE;
}

uint32_t drm_vmwgfx_surface_backend_id(
    const struct drm_vmwgfx_surface *surface)
{
    return surface ? svga_resource_id(surface->resource.backend) : UINT32_MAX;
}

struct task_file *drm_vmwgfx_surface_prime_description(
    struct drm_vmwgfx_surface *surface)
{
    if (!surface) return NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
    struct task_file *description =
        surface->resource.state == DRM_VMW_RESOURCE_LIVE &&
        surface->metadata.shareable ? surface->prime_description : NULL;
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
    return description;
}

int drm_vmwgfx_surface_prime_bind(struct drm_vmwgfx_surface *surface,
                                  struct task_file *description)
{
    if (!surface || !description) return -LINUX_EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
    int ret = 0;
    if (surface->resource.state != DRM_VMW_RESOURCE_LIVE ||
        !surface->metadata.shareable)
        ret = -LINUX_EINVAL;
    else if (surface->prime_description)
        ret = -LINUX_EEXIST;
    else if (surface->resource.prime_references == UINT32_MAX)
        ret = -LINUX_EOVERFLOW;
    else {
        surface->prime_description = description;
        ++surface->resource.prime_references;
    }
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
    return ret;
}

void drm_vmwgfx_surface_prime_unbind(struct drm_vmwgfx_surface *surface,
                                     struct task_file *description)
{
    if (!surface || !description) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_vmwgfx_resource_lock, &flags);
    if (surface->prime_description == description) {
        surface->prime_description = NULL;
        if (surface->resource.prime_references)
            --surface->resource.prime_references;
        if (!surface->resource.logical_references &&
            !surface->resource.prime_references)
            drm_vmwgfx_queue_locked(&surface->resource);
    }
    kernel_spin_unlock_irqrestore(&drm_vmwgfx_resource_lock, flags);
}

int drm_vmwgfx_surface_get_metadata(
    const struct drm_vmwgfx_surface *surface,
    struct drm_vmwgfx_surface_metadata *metadata)
{
    if (!surface || !metadata) return -LINUX_EINVAL;
    *metadata = surface->metadata;
    return 0;
}

int drm_vmwgfx_surface_get_subresource(
    const struct drm_vmwgfx_surface *surface, uint32_t face, uint32_t mip,
    struct drm_vmwgfx_surface_subresource *subresource)
{
    if (!surface || !subresource || face >= DRM_VMW_MAX_SURFACE_FACES ||
        mip >= surface->metadata.mip_levels[face])
        return -LINUX_EINVAL;
    uint32_t width = surface->metadata.desc.width;
    uint32_t height = surface->metadata.desc.height;
    uint32_t depth = surface->metadata.desc.depth;
    for (uint32_t level = 0; level < mip; ++level) {
        if (width > 1u) width >>= 1;
        if (height > 1u) height >>= 1;
        if (depth > 1u) depth >>= 1;
    }
    *subresource = (struct drm_vmwgfx_surface_subresource){
        .width = width, .height = height, .depth = depth,
    };
    return 0;
}
