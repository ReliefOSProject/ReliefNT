/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../drivers/video/drm/svga/exec.h"
#include "../../drivers/video/drm/svga/kms.h"
#include <reliefnt/futex.h>
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/time.h>
#include <linux/time.h>

struct svga_device svga;
static struct svga_fence fence = {.object.handle = 11, .sequence = 7};
static struct task task;
static uint64_t now_us = 10, local_counter_offset;
static uint32_t sequence, completed;
static unsigned sleeps, unrefs;
static struct {
    struct svga_buffer *entries[4];
    unsigned count;
} pins;
struct drm_bo { unsigned references; };
struct svga_region { SVGA3dQueryResult result; bool live; };
static struct drm_bo buffers[2];
static struct svga_region dummies[2];
static unsigned barriers, tail_barriers, waits;

uint64_t time_work_counter_us(void) { return now_us + local_counter_offset; }
uint64_t time_ticks(void) { return now_us / (1000000 / RELIEFNT_TICK_HZ); }
int time_clock_get(int32_t clock, struct linux_timespec *now)
{
    assert(clock == LINUX_CLOCK_MONOTONIC);
    *now = (struct linux_timespec){now_us / 1000000, now_us % 1000000 * 1000};
    return 0;
}
void sched_sleep_current_until(uint64_t tick)
{
    assert(tick == time_ticks() + 1);
    ++sleeps;
}

void *kernel_malloc(size_t bytes) { return malloc(bytes); }
void kernel_free(void *ptr) { free(ptr); }

int svga_submit_poll(uint32_t value)
{
    return value && value <= completed ? 1 : svga.fifo.fault ? -EIO : 0;
}
uint32_t svga_submit_completed(void) { return completed; }
int svga_submit_checkpoint(uint32_t *value) { *value = sequence; return 0; }
uint32_t svga_render_retire(uint32_t budget) { (void)budget; return 0; }

struct drm_bo *drm_bo_lookup_handle_get(struct drm_file *file, uint32_t handle)
{
    (void)file;
    assert(handle > 0 && handle <= 2);
    struct drm_bo *bo = &buffers[handle - 1];
    drm_bo_get(bo);
    return bo;
}
uint64_t drm_bo_size(const struct drm_bo *bo) { assert(bo); return SVGA_PAGE_SIZE; }
void drm_bo_get(struct drm_bo *bo) { ++bo->references; }
void drm_bo_put(struct drm_bo *bo) { assert(bo->references); --bo->references; }
bool drm_bo_gpu_busy(struct drm_bo *bo) { assert(bo->references); return false; }
int svga_batch_pin_buffer(struct svga_batch *batch, struct svga_buffer *buffer)
{
    (void)batch;
    assert(pins.count < 4);
    svga_buffer_get(buffer);
    pins.entries[pins.count++] = buffer;
    return 0;
}
int svga_batch_pin_object(struct svga_batch *batch, struct svga_object *object)
{
    (void)batch;
    assert(object->type == SVGA_CONTEXT);
    return 0;
}
static void retire_pins(void)
{
    while (pins.count)
        svga_buffer_put(pins.entries[--pins.count]);
}

int svga_region_bind(enum svga_region_type type, struct drm_bo *bo, struct svga_region **out)
{
    (void)type; (void)bo; (void)out;
    abort();
}
int svga_region_create(enum svga_region_type type, uint32_t bytes, struct svga_region **out)
{
    assert(bytes == SVGA_PAGE_SIZE);
    struct svga_region *dummy = &dummies[type == SVGA_REGION_MOB];
    assert(!dummy->live);
    dummy->live = true;
    *out = dummy;
    return 0;
}
uint32_t svga_region_id(const struct svga_region *region)
{
    assert(region->live);
    return region == &dummies[0] ? 31 : 32;
}
void *svga_region_pixels(const struct svga_region *region) { return (void *)&region->result; }
uint32_t svga_region_release(struct svga_region *region)
{
    if (region) {
        assert(region->live);
        region->live = false;
        return 1;
    }
    return 0;
}
int svga_submit_commands(const void *commands, uint32_t bytes, uint32_t dx_context,
                         uint32_t *out)
{
    const SVGA3dCmdHeader *header = commands;
    assert(dx_context == SVGA3D_INVALID_ID);
    assert(header->id == SVGA_3D_CMD_WAIT_FOR_QUERY || header->id == SVGA_3D_CMD_WAIT_FOR_GB_QUERY);
    assert(bytes == sizeof(*header) + sizeof(SVGA3dCmdWaitForQuery));
    ++barriers;
    ++sequence;
    if (out)
        *out = sequence;
    return 0;
}
int svga_submit_try_fenced(const void *commands, uint32_t bytes, uint32_t *out)
{
    return svga_submit_commands(commands, bytes, SVGA3D_INVALID_ID, out);
}
int svga_submit_wait(uint32_t value)
{
    assert(value == sequence);
    ++waits;
    completed = value;
    return 0;
}
int svga_batch_query_wait(struct svga_batch *batch, uint32_t command, const void *body,
                          uint32_t bytes)
{
    (void)batch;
    const SVGA3dCmdWaitForQuery *wait = body;
    assert(command == SVGA_3D_CMD_WAIT_FOR_QUERY && bytes == sizeof(*wait));
    assert(wait->guestResult.gmrId == 31 && wait->cid == 17);
    ++tail_barriers;
    return 0;
}

struct svga_object *svga_object_lookup(const struct drm_file *file, enum svga_object_type type,
                                       uint32_t handle)
{
    (void)file;
    assert(type == SVGA_FENCE);
    return handle == fence.object.handle ? &fence.object : NULL;
}

int svga_object_unref(struct drm_file *file, enum svga_object_type type, uint32_t handle)
{
    assert(svga_object_lookup(file, type, handle));
    ++unrefs;
    return 0;
}

/* This fixture exercises wait/status and leaves event construction unused. */
struct svga_object *svga_object_create(struct drm_file *file, enum svga_object_type type, size_t size)
{
    (void)file; (void)type; (void)size;
    abort();
}
void svga_object_put(struct svga_object *object) { (void)object; abort(); }
void svga_object_get(struct svga_object *object) { (void)object; abort(); }
int svga_event_reserve(struct drm_file *file) { (void)file; abort(); }
void svga_event_cancel(struct drm_file *file) { (void)file; abort(); }
void svga_event_publish(struct drm_file *file, const struct drm_event *event)
{
    (void)file; (void)event;
    abort();
}
struct svga_render_file *svga_render_file_get(struct drm_file *file) { (void)file; abort(); }
int user_copy_to_task(struct task *caller, uint64_t address, const void *data, uint64_t size)
{
    (void)caller; (void)address; (void)data; (void)size;
    abort();
}

bool user_range_writable(uint64_t address, uint64_t size)
{
    (void)address; (void)size;
    abort();
}

static void fence_reply(void)
{
    completed = fence.sequence - 1;
    struct drm_vmw_fence_rep reply;
    svga_fence_fill(&fence, &reply);
    assert(reply.handle == fence.object.handle);
    assert(reply.seqno == fence.sequence && reply.passed_seqno == completed);
    /* A nonzero EXEC mask makes Mesa skip waiting on this pending fence. */
    assert(!reply.mask && !reply.error && reply.fd == -1);
}

static void fence_deadline(void)
{
    task.lookup_scope.armed = true;
    const struct drm_vmw_fence_wait_arg input = {
        .handle = fence.object.handle, .timeout_us = 10000,
        .wait_options = DRM_VMW_WAIT_OPTION_UNREF,
    };
    struct drm_vmw_fence_wait_arg arg = input;
    assert(svga_fence_ioctl(&task, NULL, VMW_FENCE_WAIT, &arg) == KERNEL_SYSCALL_BLOCKED);
    assert(arg.cookie_valid && arg.kernel_cookie == 10010 && sleeps == 1);
    /* Replay can move to another CPU and import the original ioctl snapshot.
     * The new CPU's work counter must not expire the pending fence. */
    local_counter_offset = 2000000;
    ++now_us;
    arg = input;
    assert(svga_fence_ioctl(&task, NULL, VMW_FENCE_WAIT, &arg) == KERNEL_SYSCALL_BLOCKED);
    assert(arg.kernel_cookie == 10010 && sleeps == 2);
    now_us = 10010;
    arg = input;
    assert(svga_fence_ioctl(&task, NULL, VMW_FENCE_WAIT, &arg) == -EBUSY);
    assert(arg.kernel_cookie == 10010 && sleeps == 2 && !unrefs);
    completed = fence.sequence;
    svga.fifo.fault = true;
    arg = input;
    assert(!svga_fence_ioctl(&task, NULL, VMW_FENCE_WAIT, &arg) && unrefs == 1);
}

static void end_query(struct svga_batch *batch, struct svga_context *context, struct drm_bo *bo)
{
    struct svga_buffer *buffer;
    assert(!svga_batch_bo(batch, bo, &buffer));
    assert(!svga_query_end(batch, context, buffer));
}

static void query_synchronization(void)
{
    svga.fifo.fault = false;
    sequence = completed;
    struct svga_context host = {.object = {.type = SVGA_CONTEXT, .id = 17}, .type = SVGA_CONTEXT_HOST};
    struct svga_context gb = {.object = {.type = SVGA_CONTEXT, .id = 18}, .type = SVGA_CONTEXT_GB};
    struct svga_batch batch = {.serial = 1};
    end_query(&batch, &host, &buffers[0]);
    svga_query_finish(&batch, true);
    retire_pins();
    assert(buffers[0].references == 1);
    struct drm_vmw_synccpu_arg arg = {
        .op = drm_vmw_synccpu_grab, .handle = 1,
        .flags = drm_vmw_synccpu_allow_cs | drm_vmw_synccpu_dontblock | drm_vmw_synccpu_read,
    };
    assert(svga_synccpu(&task, NULL, &arg) == -EBUSY && barriers == 1);
    /* A different CPU has a smaller counter: do not interpret it as an
     * expired query and quarantine a live device. */
    local_counter_offset = 0;
    assert(svga_synccpu(&task, NULL, &arg) == -EBUSY && barriers == 1 && !svga.fifo.fault);
    completed = sequence;
    assert(!svga_synccpu(&task, NULL, &arg));

    batch = (struct svga_batch){.serial = 2};
    end_query(&batch, &gb, &buffers[1]);
    svga_query_finish(&batch, false);
    retire_pins();
    assert(buffers[0].references == 1 && !buffers[1].references && tail_barriers == 1);
    assert(!svga_synccpu(&task, NULL, &arg));
    batch = (struct svga_batch){.serial = 3};
    end_query(&batch, &gb, &buffers[1]);
    svga_query_finish(&batch, true);
    retire_pins();
    assert(!buffers[0].references && buffers[1].references == 1 && tail_barriers == 2);
    svga_query_release(&gb);
    assert(!buffers[1].references && !dummies[0].live && !dummies[1].live);
    assert(barriers == 2 && waits == 1);
}

int main(void)
{
    fence_reply();
    fence_deadline();
    query_synchronization();
    puts("PASS SVGA fence reply and deadline, legacy query synchronization, rollback and teardown");
}
