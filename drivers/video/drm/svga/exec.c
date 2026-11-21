/* SPDX-License-Identifier: Apache-2.0 */
#include "exec.h"
#include "../memory.h"
#include "../sync.h"
#include <reliefnt/heap.h>
#include <reliefnt/device_mapping.h>
#include <reliefnt/sched.h>
#include <reliefnt/futex.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/time.h>

#define BATCH_REFS 32u
#define MAX_PENDING_BATCHES 128u
#define MAX_BATCH_AGE_TICKS (5 * RELIEFNT_TICK_HZ)

struct svga_validation_ref {
    struct svga_object *object;
    struct svga_buffer *buffer;
};

struct svga_validation_refs {
    struct svga_validation_refs *next;
    unsigned count;
    struct svga_validation_ref entries[BATCH_REFS];
};

static struct svga_batch *pending, **pending_tail = &pending, *quarantined;
static uint64_t validation_serial, surface_write_serial;
static unsigned pending_count;

/* FIFO submission needs a private CPU copy for validation. Command buffer
 * devices validate directly in the unpublished DMA payload instead. */
static void *command_bounce;
static uint32_t command_capacity;

static int reserve_bounce(uint32_t bytes, uint32_t used)
{
    if (bytes <= command_capacity)
        return 0;
    uint32_t limit = svga_submit_limit();
    if (bytes > limit)
        return -E2BIG;
    uint32_t capacity = command_capacity ? command_capacity : 4096;
    if (capacity > limit)
        capacity = limit;
    while (capacity < bytes)
        capacity = capacity <= limit / 2 ? capacity * 2 : limit;

    void *storage = kernel_malloc(capacity);
    if (!storage)
        return -ENOMEM;
    if (used)
        __builtin_memcpy(storage, command_bounce, used);
    kernel_free(command_bounce);
    command_bounce = storage;
    command_capacity = capacity;
    return 0;
}

static int reserve_commands(uint32_t bytes, uint32_t used, bool staged, void **commands)
{
    if (staged)
        return svga_submit_stage(bytes, used, commands);
    int ret = reserve_bounce(bytes, used);
    if (!ret)
        *commands = command_bounce;
    return ret;
}

void svga_execbuf_fini(void)
{
    kernel_free(command_bounce);
    command_bounce = NULL;
    command_capacity = 0;
}

static struct svga_validation_ref *add_ref(struct svga_batch *batch)
{
    struct svga_validation_refs *refs = batch->refs;
    if (!refs || refs->count == BATCH_REFS) {
        refs = kernel_malloc(sizeof(*refs));
        if (!refs)
            return NULL;
        refs->next = batch->refs;
        refs->count = 0;
        batch->refs = refs;
    }
    return &refs->entries[refs->count++];
}

int svga_batch_pin_object(struct svga_batch *batch, struct svga_object *object)
{
    struct svga_validation_ref *ref = add_ref(batch);
    if (!ref)
        return -ENOMEM;
    svga_object_get(object);
    *ref = (struct svga_validation_ref){.object = object};
    return 0;
}

int svga_batch_object(struct svga_batch *batch, struct svga_object *object)
{
    if (object->validation_serial == batch->serial)
        return 0;
    int ret = svga_batch_pin_object(batch, object);
    if (ret)
        return ret;
    object->validation_serial = batch->serial;
    if (object->type == SVGA_SURFACE) {
        struct svga_surface *surface = (struct svga_surface *)object;
        surface->validation_bo = surface->bo;
        surface->validation_dirty = surface->dirty_serial;
        surface->validation_upload = 0;
        ret = svga_batch_bo(batch, surface->bo, NULL);
        return ret ? ret : svga_surface_validate_residency(surface);
    }
    if (object->type == SVGA_SHADER)
        return svga_batch_bo(batch, svga_region_bo(((struct svga_shader *)object)->backing), NULL);
    if (object->type == SVGA_CONTEXT) {
        struct svga_context *context = (struct svga_context *)object;
        ret = svga_context_validate_bindings(batch, context);
        return ret ? ret : svga_context_validate_residency(context);
    }
    return 0;
}

void svga_batch_surface_written(struct svga_surface *surface)
{
    if (surface && surface->gb)
        surface->validation_dirty = ++surface_write_serial;
}

void svga_batch_surface_cleaned(struct svga_surface *surface)
{
    surface->validation_dirty = 0;
}

int svga_batch_pin_buffer(struct svga_batch *batch, struct svga_buffer *buffer)
{
    struct svga_validation_ref *ref = add_ref(batch);
    if (!ref)
        return -ENOMEM;
    int ret = drm_bo_gpu_reserve(svga_buffer_bo(buffer));
    if (ret) {
        --batch->refs->count;
        return ret;
    }
    svga_buffer_get(buffer);
    *ref = (struct svga_validation_ref){.buffer = buffer};
    return 0;
}

int svga_batch_context(struct svga_batch *batch, uint32_t *id, struct svga_context **out)
{
    struct svga_context *context = (struct svga_context *)svga_object_lookup(batch->file, SVGA_CONTEXT, *id);
    if (!context)
        return -ENOENT;
    int ret = svga_batch_object(batch, &context->object);
    if (!ret) {
        *id = context->object.id;
        *out = context;
    }
    return ret;
}

int svga_batch_surface(struct svga_batch *batch, uint32_t *id, bool optional,
                        struct svga_surface **out)
{
    struct svga_surface *surface = NULL;
    if (!optional || *id != SVGA3D_INVALID_ID) {
        surface = (struct svga_surface *)svga_object_lookup(batch->file, SVGA_SURFACE, *id);
        if (!surface)
            return -ENOENT;
        int ret = svga_batch_object(batch, &surface->object);
        if (ret)
            return ret;
        *id = surface->object.id;
    }
    if (out)
        *out = surface;
    return 0;
}

int svga_batch_image(struct svga_batch *batch, SVGA3dSurfaceImageId *id, bool optional,
                      struct svga_surface **out, SVGA3dSize *size)
{
    uint32_t sid = id->sid;
    struct svga_surface *surface;
    int ret = svga_batch_surface(batch, &sid, optional, &surface);
    if (ret)
        return ret;
    if (surface) {
        SVGA3dSize temporary;
        const SVGA3dSize *image = svga_surface_image(surface, id->face, id->mipmap, &temporary);
        if (!image)
            return -EINVAL;
        if (size)
            *size = *image;
    }
    id->sid = sid;
    if (out)
        *out = surface;
    return 0;
}

struct svga_backing_change {
    struct svga_backing_change *next;
    struct svga_region **slot, *old;
    struct svga_surface *surface;
    struct drm_bo *old_bo, *new_bo;
    uint32_t *offset_slot, old_offset;
};

int svga_batch_rebind(struct svga_batch *batch, struct svga_region **slot,
                      struct svga_surface *surface, uint32_t *offset_slot,
                      struct svga_buffer *buffer, uint32_t offset, uint32_t *mob)
{
    struct drm_bo *bo = svga_buffer_bo(buffer);
    if (surface && (!bo || drm_bo_is_dumb(bo)))
        return -EINVAL;
    if (*slot && svga_region_bo(*slot) == bo && (!offset_slot || *offset_slot == offset)) {
        *mob = svga_region_id(*slot);
        return 0;
    }
    struct svga_backing_change *change = kernel_malloc(sizeof(*change));
    if (!change)
        return -ENOMEM;
    struct svga_region *region = NULL;
    int ret = bo ? svga_region_bind(SVGA_REGION_MOB, bo, &region) : 0;
    if (ret) {
        kernel_free(change);
        return ret;
    }
    *change = (struct svga_backing_change){
        .next = batch->backing_changes, .slot = slot, .old = *slot,
        .surface = surface, .old_bo = surface ? surface->bo : NULL,
        .new_bo = surface ? bo : NULL,
        .offset_slot = offset_slot, .old_offset = offset_slot ? *offset_slot : 0,
    };
    if (surface) {
        drm_bo_get(bo);
        svga_surface_set_bo(surface, bo);
    }
    if (offset_slot)
        *offset_slot = offset;
    *slot = region;
    batch->backing_changes = change;
    *mob = region ? svga_region_id(region) : SVGA3D_INVALID_ID;
    return 0;
}

static void finish_bindings(struct svga_batch *batch, bool commit)
{
    svga_context_finish(batch, commit);
    while (batch->backing_changes) {
        struct svga_backing_change *change = batch->backing_changes;
        batch->backing_changes = change->next;
        if (commit) {
            if (change->surface && change->surface->bo == change->new_bo) {
                struct svga_surface *surface = change->surface;
                surface->uploaded_epoch = drm_bo_mapping_access(change->new_bo)->write_epoch;
            }
            svga_region_release(change->old);
            drm_bo_put(change->old_bo);
        } else {
            svga_region_release(*change->slot);
            *change->slot = change->old;
            if (change->surface) {
                struct drm_bo *bo = change->surface->bo;
                svga_surface_set_bo(change->surface, change->old_bo);
                drm_bo_put(bo);
            }
            if (change->offset_slot)
                *change->offset_slot = change->old_offset;
        }
        kernel_free(change);
    }
    svga_batch_unmap_mobs(batch);
}

/* Append a kernel query barrier before the batch's completion marker. */
int svga_batch_query_wait(struct svga_batch *batch, uint32_t command, const void *body,
                           uint32_t bytes)
{
    SVGA3dCmdHeader header = {command, bytes};
    uint32_t end = batch->query_tail_bytes + sizeof(header) + bytes;
    if (end > svga_submit_limit())
        return -E2BIG;
    if (end > batch->query_tail_capacity) {
        uint32_t capacity = batch->query_tail_capacity ? 2 * batch->query_tail_capacity : 256;
        if (capacity < end)
            capacity = end;
        void *tail = kernel_malloc(capacity);
        if (!tail)
            return -ENOMEM;
        if (batch->query_tail_bytes)
            __builtin_memcpy(tail, batch->query_tail, batch->query_tail_bytes);
        kernel_free(batch->query_tail);
        batch->query_tail = tail;
        batch->query_tail_capacity = capacity;
    }
    uint8_t *where = (uint8_t *)batch->query_tail + batch->query_tail_bytes;
    __builtin_memcpy(where, &header, sizeof(header));
    __builtin_memcpy(where + sizeof(header), body, bytes);
    batch->query_tail_bytes = end;
    return 0;
}

static void release_batch(struct svga_batch *batch)
{
    while (batch->refs) {
        struct svga_validation_refs *refs = batch->refs;
        batch->refs = refs->next;
        for (unsigned i = 0; i < refs->count; ++i) {
            struct svga_validation_ref *ref = &refs->entries[i];
            if (ref->object) {
                svga_object_put(ref->object);
            } else {
                drm_bo_gpu_release(svga_buffer_bo(ref->buffer));
                svga_buffer_put(ref->buffer);
            }
        }
        kernel_free(refs);
    }
    if (batch->fence)
        svga_object_put(&batch->fence->object);
    drm_sync_fence_put(batch->sync);
    drm_async_free(batch);
}

static uint32_t retire_batches(uint32_t budget)
{
    uint32_t count = 0;
    while (pending && count < budget) {
        struct svga_batch *batch = pending;
        int status = svga_submit_poll(batch->sequence);
        if (!status && time_ticks() - batch->submitted_tick >= MAX_BATCH_AGE_TICKS) {
            svga.fifo.fault = true;
            status = -ETIMEDOUT;
        }
        if (!status)
            break;
        pending = batch->next;
        if (!pending)
            pending_tail = &pending;
        --pending_count;
        if (batch->fence)
            batch->fence->status = status;
        drm_sync_fence_signal(batch->sync, status < 0 ? status : 0);
        if (status < 0 && batch->refs) {
            /* Keep DMA operands pinned until quiescence is proven. The
             * terminal software fences no longer need batch ownership. */
            if (batch->fence) {
                svga_object_put(&batch->fence->object);
                batch->fence = NULL;
            }
            drm_sync_fence_put(batch->sync);
            batch->sync = NULL;
            batch->next = quarantined;
            quarantined = batch;
        } else {
            release_batch(batch);
        }
        ++count;
    }
    return count;
}

uint32_t svga_render_retire(uint32_t budget)
{
    uint32_t count = retire_batches(budget);
    count += svga_fence_events_retire(budget - count);
    return count + svga_dma_retire(budget - count);
}

static void enqueue_batch(struct svga_batch *batch)
{
    for (struct svga_validation_refs *refs = batch->refs; refs; refs = refs->next) {
        for (unsigned i = 0; i < refs->count; ++i)
            if (refs->entries[i].buffer)
                svga_buffer_fence(refs->entries[i].buffer, batch->sequence);
    }
    batch->submitted_tick = time_ticks();
    batch->file = NULL;
    *pending_tail = batch;
    pending_tail = &batch->next;
    ++pending_count;
}

/* A MOB remains shared with userspace across execbuf calls. Revoke CPU
 * aliases once per handoff, then prepend uploads for surfaces whose backing
 * was writable since their previous upload. A rebind in the user stream
 * sees the old MOB here, just like resource validation in vmwgfx. */
static int submit_batch(struct svga_batch *batch, const void *commands, uint32_t bytes,
                        uint32_t dx_context, bool staged)
{
    /* A buffer used between two binds is no longer visible in the BO's
     * surface index. Hand off every coherent bind while its BO is retained. */
    for (struct svga_backing_change *change = batch->backing_changes; change; change = change->next) {
        if (change->surface && svga_surface_is_coherent(change->surface)) {
            int ret = device_mapping_revoke_cpu(drm_bo_mapping_access(change->new_bo));
            if (ret)
                return ret;
        }
    }
    uint32_t uploads = 0;
    for (struct svga_validation_refs *refs = batch->refs; refs; refs = refs->next) {
        for (unsigned i = 0; i < refs->count; ++i) {
            struct svga_validation_ref *ref = &refs->entries[i];
            if (ref->buffer) {
                struct drm_bo *bo = svga_buffer_bo(ref->buffer);
                struct device_mapping_access *access = drm_bo_mapping_access(bo);
                if ((access->exposed || access->aliases) && svga_bo_is_coherent(bo)) {
                    int ret = device_mapping_revoke_cpu(access);
                    if (ret)
                        return ret;
                }
                continue;
            }
            if (ref->object->type != SVGA_SURFACE)
                continue;
            struct svga_surface *surface = (struct svga_surface *)ref->object;
            struct drm_bo *bo = surface->validation_bo;
            if (!surface->gb || !bo)
                continue;
            struct device_mapping_access *access = drm_bo_mapping_access(bo);
            bool coherent = svga_surface_is_coherent(surface);
            if (!coherent && (!access->write_epoch || !svga_bo_is_coherent(bo)))
                continue;
            /* A staged rebind may already have removed the old BO's last
             * coherent surface from the weak index. */
            if (coherent && bo != surface->bo) {
                int ret = device_mapping_revoke_cpu(access);
                if (ret)
                    return ret;
            }
            if (access->write_epoch != surface->uploaded_epoch && access->write_epoch) {
                surface->validation_upload = access->write_epoch;
                ++uploads;
            }
        }
    }

    typedef SVGA_PACKET(SVGA_3D_CMD_UPDATE_GB_SURFACE, SVGA3dCmdUpdateGBSurface) upload_packet;
    uint32_t prefix = uploads * sizeof(upload_packet);
    if (bytes > svga_submit_limit() || prefix > svga_submit_limit() - bytes)
        return -E2BIG;
    void *stream = NULL;
    if (prefix) {
        if (staged) {
            int ret = svga_submit_stage(prefix + bytes, bytes, &stream);
            if (ret)
                return ret;
            __builtin_memmove((uint8_t *)stream + prefix, stream, bytes);
        } else {
            stream = kernel_malloc(prefix + bytes);
            if (!stream)
                return -ENOMEM;
            if (bytes)
                __builtin_memcpy((uint8_t *)stream + prefix, commands, bytes);
        }
        upload_packet *packet = stream;
        for (struct svga_validation_refs *refs = batch->refs; refs; refs = refs->next) {
            for (unsigned i = 0; i < refs->count; ++i) {
                struct svga_object *object = refs->entries[i].object;
                if (!object || object->type != SVGA_SURFACE)
                    continue;
                struct svga_surface *surface = (struct svga_surface *)object;
                if (surface->validation_upload) {
                    *packet++ = (upload_packet){
                        SVGA_HEADER(SVGA_3D_CMD_UPDATE_GB_SURFACE, SVGA3dCmdUpdateGBSurface),
                        {.sid = surface->object.id},
                    };
                }
            }
        }
    }
    int ret;
    if (!prefix && !bytes)
        ret = svga_submit_fenced(NULL, 0, &batch->sequence);
    else if (staged)
        ret = svga_submit_staged(prefix + bytes, dx_context, &batch->sequence);
    else
        ret = svga_submit_commands(stream ? stream : commands, prefix + bytes,
                                   dx_context, &batch->sequence);
    if (!staged)
        kernel_free(stream);
    if (ret)
        return ret;

    for (struct svga_validation_refs *refs = batch->refs; refs; refs = refs->next) {
        for (unsigned i = 0; i < refs->count; ++i) {
            struct svga_object *object = refs->entries[i].object;
            if (!object || object->type != SVGA_SURFACE)
                continue;
            struct svga_surface *surface = (struct svga_surface *)object;
            if (surface->validation_upload) {
                surface->uploaded_epoch = surface->validation_upload;
                if (surface->validation_dirty == surface->dirty_serial)
                    surface->validation_dirty = 0;
            }
            surface->dirty_serial = surface->validation_dirty;
        }
    }
    return 0;
}

int svga_display_submit(const void *commands, uint32_t bytes, struct svga_surface *source,
                        struct svga_surface *target, struct drm_bo *bo, uint32_t *sequence)
{
    (void)svga_render_retire(MAX_PENDING_BATCHES);
    if (pending_count == MAX_PENDING_BATCHES) {
        int ret = svga_submit_wait(pending->sequence);
        if (ret)
            return ret;
        (void)svga_render_retire(MAX_PENDING_BATCHES);
    }
    if (svga.fifo.fault)
        return -EIO;
    struct svga_batch *batch = drm_async_alloc(sizeof(*batch));
    if (!batch)
        return -ENOMEM;
    *batch = (struct svga_batch){.serial = ++validation_serial};
    int ret = source ? svga_batch_object(batch, &source->object) : 0;
    if (!ret && target) {
        ret = svga_batch_object(batch, &target->object);
        if (!ret)
            svga_batch_surface_written(target);
    }
    if (!ret)
        ret = svga_batch_bo(batch, bo, NULL);
    if (!ret)
        ret = submit_batch(batch, commands, bytes, SVGA3D_INVALID_ID, false);
    if (ret) {
        release_batch(batch);
        return ret;
    }
    if (sequence)
        *sequence = batch->sequence;
    enqueue_batch(batch);
    return 0;
}

static int validate_commands(struct svga_batch *batch, void *commands, uint32_t bytes)
{
    uint8_t *end = (uint8_t *)commands + bytes;
    uint8_t *pos = commands;
    while (pos != end) {
        struct svga_packet packet;
        int ret = svga_packet_read(pos, end - pos, &packet);
        if (ret)
            return ret;
        ret = svga_validate_legacy(batch, &packet);
        if (ret == -ENOTTY)
            ret = svga_validate_dx(batch, &packet);
        if (ret)
            return ret == -ENOTTY ? -EINVAL : ret;
        pos = packet.next;
    }
    return 0;
}

int svga_execbuf(struct task *task, struct drm_file *file, const struct drm_vmw_execbuf_arg *arg)
{
    if (!arg->version || arg->version > DRM_VMW_EXECBUF_VERSION || (arg->command_size & 3) ||
        arg->command_size > SVGA_CB_MAX_SIZE)
        return -EINVAL;
    if (arg->command_size > svga_submit_limit())
        return -E2BIG;
    (void)svga_render_retire(MAX_PENDING_BATCHES);
    if (svga.fifo.fault)
        return -EIO;
    if (pending_count >= MAX_PENDING_BATCHES) {
        sched_sleep_current_until(time_ticks() + 1);
        return KERNEL_SYSCALL_BLOCKED;
    }
    int ret;
    if (arg->flags & DRM_VMW_EXECBUF_FLAG_IMPORT_FENCE_FD) {
        ret = drm_sync_wait_fd(task, arg->imported_fence_fd);
        if (ret)
            return ret;
    }
    if (arg->fence_rep) {
        ret = svga_prepare_output(task, arg->fence_rep, sizeof(struct drm_vmw_fence_rep));
        if (ret)
            return ret;
    }
    /* Mesa submits an empty stream when flushing for a fence. Do not read
     * the command pointer or allocate any staging storage in that case. */
    bool staged = arg->command_size && svga_submit_has_context();
    void *commands = NULL;
    if (arg->command_size) {
        ret = reserve_commands(arg->command_size, 0, staged, &commands);
        if (ret)
            return ret;
        ret = svga_import(task, commands, arg->commands, arg->command_size);
        if (ret)
            return ret;
    }
    struct svga_batch *batch = drm_async_alloc(sizeof(*batch));
    if (!batch)
        return -ENOMEM;
    *batch = (struct svga_batch){.file = file, .serial = ++validation_serial};
    uint32_t context = arg->version == 1 ? SVGA3D_INVALID_ID : arg->context_handle;
    int fd = -1;
    if (context != SVGA3D_INVALID_ID) {
        ret = svga_batch_context(batch, &context, &batch->dx);
        if (ret)
            goto abort;
        if (batch->dx->type != SVGA_CONTEXT_DX) {
            ret = -EINVAL;
            goto abort;
        }
    }
    uint32_t bytes = arg->command_size;
    ret = bytes ? validate_commands(batch, commands, bytes) : 0;
    if (!ret && batch->query_tail_bytes) {
        if (batch->query_tail_bytes > svga_submit_limit() - bytes) {
            ret = -E2BIG;
        } else {
            uint32_t grown = bytes + batch->query_tail_bytes;
            ret = reserve_commands(grown, bytes, staged, &commands);
            if (!ret) {
                __builtin_memcpy((uint8_t *)commands + bytes, batch->query_tail,
                                 batch->query_tail_bytes);
                bytes = grown;
            }
        }
    }
    kernel_free(batch->query_tail);
    batch->query_tail = NULL;
    if (!ret)
        ret = svga_context_prepare(batch);
    if (ret)
        goto abort;
    if (arg->fence_rep) {
        batch->fence = (struct svga_fence *)svga_object_create(file, SVGA_FENCE, sizeof(*batch->fence));
        if (!batch->fence) {
            ret = -ENOMEM;
            goto abort;
        }
        svga_object_get(&batch->fence->object);
    }
    if (arg->fence_rep && (arg->flags & DRM_VMW_EXECBUF_FLAG_EXPORT_FENCE_FD)) {
        batch->sync = drm_sync_fence_create("vmwgfx", "svga");
        ret = batch->sync ? drm_sync_export(task, batch->sync, &fd) : -ENOMEM;
        if (ret)
            goto abort;
    }
    ret = submit_batch(batch, commands, bytes, context, staged);
    if (ret)
        goto abort;
    finish_bindings(batch, true);
    svga_query_finish(batch, true);
    enqueue_batch(batch);
    if (batch->fence) {
        batch->fence->sequence = batch->sequence;
        struct drm_vmw_fence_rep reply;
        svga_fence_fill(batch->fence, &reply);
        reply.fd = fd;
        if (user_copy_to_task(task, arg->fence_rep, &reply, sizeof(reply)) < 0) {
            if (fd >= 0)
                task_discard_file_fd(task, fd);
            (void)svga_object_unref(file, SVGA_FENCE, reply.handle);
            /* The submission already owns its references. Satisfy the ABI's
             * failed-reply synchronization contract before returning. */
            (void)svga_submit_wait(batch->sequence);
            (void)svga_render_retire(MAX_PENDING_BATCHES);
            return -EFAULT;
        }
    }
    return 0;

abort:
    if (fd >= 0)
        task_discard_file_fd(task, fd);
    if (batch->fence)
        (void)svga_object_unref(file, SVGA_FENCE, batch->fence->object.handle);
    finish_bindings(batch, false);
    svga_query_finish(batch, false);
    release_batch(batch);
    return ret;
}
