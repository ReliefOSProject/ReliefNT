/* SPDX-License-Identifier: Apache-2.0 */
#include "exec.h"
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/futex.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/time.h>

#define BUFFER_BUCKETS 256u

/* A binding or in-flight batch can share a mapping. The hash is weak and
 * never keeps a closed GEM handle resident on its own. */
struct svga_buffer {
    struct svga_buffer *next;
    struct drm_bo *bo;
    struct svga_region *gmr, *mob;
    struct svga_buffer *mapped_next;
    struct svga_region *mapped_mob;
    uint64_t validation_serial;
    uint32_t sequence;
    unsigned references, cpu_grabs;
};

struct svga_cpu_grab {
    struct svga_cpu_grab *next;
    struct svga_buffer *buffer;
    unsigned count;
};

static struct svga_buffer *buffers[BUFFER_BUCKETS];

static struct svga_buffer **buffer_link(struct drm_bo *bo)
{
    uintptr_t hash = (uintptr_t)bo >> 4;
    struct svga_buffer **link = &buffers[(hash ^ (hash >> 8)) % BUFFER_BUCKETS];
    while (*link && (*link)->bo != bo)
        link = &(*link)->next;
    return link;
}

static struct svga_buffer *buffer_create(struct drm_bo *bo)
{
    struct svga_buffer **link = buffer_link(bo);
    if (!*link) {
        struct svga_buffer *buffer = kernel_malloc(sizeof(*buffer));
        if (!buffer)
            return NULL;
        *buffer = (struct svga_buffer){.bo = bo};
        drm_bo_get(bo);
        *link = buffer;
    }
    svga_buffer_get(*link);
    return *link;
}

struct drm_bo *svga_buffer_bo(const struct svga_buffer *buffer)
{
    return buffer ? buffer->bo : NULL;
}

bool svga_buffer_cpu_grabbed(const struct svga_buffer *buffer)
{
    return buffer && buffer->cpu_grabs;
}

void svga_buffer_get(struct svga_buffer *buffer)
{
    if (buffer)
        ++buffer->references;
}

void svga_buffer_put(struct svga_buffer *buffer)
{
    if (!buffer || --buffer->references)
        return;
    *buffer_link(buffer->bo) = buffer->next;
    svga_region_release(buffer->gmr);
    svga_region_release(buffer->mob);
    drm_bo_put(buffer->bo);
    kernel_free(buffer);
}

int svga_buffer_map(struct svga_buffer *buffer, enum svga_region_type type, uint32_t *id)
{
    struct svga_region **region = type == SVGA_REGION_MOB ? &buffer->mob : &buffer->gmr;
    if (!*region) {
        int ret = svga_region_bind(type, buffer->bo, region);
        if (ret)
            return ret;
    }
    *id = svga_region_id(*region);
    return 0;
}

void svga_buffer_fence(struct svga_buffer *buffer, uint32_t sequence)
{
    buffer->sequence = sequence;
}

int svga_buffer_wait(struct drm_bo *bo)
{
    if (svga.fifo.fault)
        return -EIO;
    struct svga_buffer *buffer = *buffer_link(bo);
    if (buffer && drm_bo_gpu_busy(bo)) {
        int ret = svga_submit_wait(buffer->sequence);
        if (ret)
            return ret;
    }
    return svga_query_wait(bo);
}

/* A reservation belongs to the batch and survives user handle closure. */

int svga_batch_bo(struct svga_batch *batch, struct drm_bo *bo, struct svga_buffer **out)
{
    if (!bo)
        return out ? -ENOENT : 0;
    struct svga_buffer *buffer = buffer_create(bo);
    if (!buffer)
        return -ENOMEM;
    int ret = 0;
    if (buffer->cpu_grabs) {
        ret = -EBUSY;
    } else if (buffer->validation_serial != batch->serial) {
        ret = svga_batch_pin_buffer(batch, buffer);
        if (!ret)
            buffer->validation_serial = batch->serial;
    }
    if (!ret && out)
        *out = buffer;
    svga_buffer_put(buffer);
    return ret;
}

int svga_batch_handle(struct svga_batch *batch, uint32_t handle, struct svga_buffer **out)
{
    struct drm_bo *bo = drm_bo_lookup_handle_get(batch->file, handle);
    int ret = svga_batch_bo(batch, bo, out);
    drm_bo_put(bo);
    return ret;
}

int svga_batch_map_mob(struct svga_batch *batch, struct svga_buffer *buffer,
                       struct svga_region **out)
{
    if (!buffer->mapped_mob) {
        int ret = svga_region_bind(SVGA_REGION_MOB, buffer->bo, &buffer->mapped_mob);
        if (ret)
            return ret;
        buffer->mapped_next = batch->mapped_mobs;
        batch->mapped_mobs = buffer;
    }
    *out = buffer->mapped_mob;
    return 0;
}

void svga_batch_unmap_mobs(struct svga_batch *batch)
{
    while (batch->mapped_mobs) {
        struct svga_buffer *buffer = batch->mapped_mobs;
        batch->mapped_mobs = buffer->mapped_next;
        svga_region_release(buffer->mapped_mob);
        buffer->mapped_mob = NULL;
        buffer->mapped_next = NULL;
    }
}

static void release_grab(struct svga_cpu_grab **link, bool all)
{
    struct svga_cpu_grab *grab = *link;
    unsigned count = all ? grab->count : 1;
    grab->buffer->cpu_grabs -= count;
    grab->count -= count;
    if (!grab->count) {
        *link = grab->next;
        svga_buffer_put(grab->buffer);
        kernel_free(grab);
    }
}

void svga_cpu_grabs_release(struct svga_render_file *state)
{
    while (state->grabs)
        release_grab(&state->grabs, true);
}

int svga_synccpu(struct task *task, struct drm_file *file, const struct drm_vmw_synccpu_arg *arg)
{
    const unsigned access = drm_vmw_synccpu_read | drm_vmw_synccpu_write;
    const unsigned allowed = access | drm_vmw_synccpu_dontblock | drm_vmw_synccpu_allow_cs;
    bool allow_cs = arg->flags & drm_vmw_synccpu_allow_cs;
    if (!(arg->flags & access) || (arg->flags & ~allowed) ||
        (arg->op != drm_vmw_synccpu_grab && arg->op != drm_vmw_synccpu_release))
        return -EINVAL;
    struct drm_bo *bo = drm_bo_lookup_handle_get(file, arg->handle);
    if (!bo)
        return -ENOENT;
    struct svga_render_file *state = allow_cs ? NULL : svga_render_file_get(file);
    int ret = 0;
    (void)task;
    if (!allow_cs && !state) {
        ret = -ENOMEM;
        goto out;
    }
    struct svga_cpu_grab **link = state ? &state->grabs : NULL;
    while (link && *link && (*link)->buffer->bo != bo)
        link = &(*link)->next;
    if (arg->op == drm_vmw_synccpu_release) {
        if (!allow_cs) {
            if (!*link)
                ret = -EINVAL;
            else
                release_grab(link, false);
        }
        goto out;
    }
    (void)svga_render_retire(128);
    if (svga.fifo.fault) {
        ret = -EIO;
        goto out;
    }
    bool nowait = arg->flags & drm_vmw_synccpu_dontblock;
    int ready = drm_bo_gpu_busy(bo) ? 0 : svga_query_poll(bo, nowait);
    if (ready <= 0) {
        ret = ready ? ready : -EBUSY;
        if (!ready && !nowait) {
            sched_sleep_current_until(time_ticks() + 1);
            ret = KERNEL_SYSCALL_BLOCKED;
        }
        goto out;
    }
    if (allow_cs)
        goto out;
    if (!*link) {
        struct svga_cpu_grab *grab = kernel_malloc(sizeof(*grab));
        if (!grab) {
            ret = -ENOMEM;
            goto out;
        }
        struct svga_buffer *buffer = buffer_create(bo);
        if (!buffer) {
            kernel_free(grab);
            ret = -ENOMEM;
            goto out;
        }
        *grab = (struct svga_cpu_grab){.buffer = buffer};
        *link = grab;
    }
    if ((*link)->buffer->cpu_grabs == UINT32_MAX) {
        ret = -EBUSY;
        goto out;
    }
    ++(*link)->buffer->cpu_grabs;
    ++(*link)->count;
out:
    drm_bo_put(bo);
    return ret;
}
