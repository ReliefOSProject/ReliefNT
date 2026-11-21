/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../drivers/video/drm/svga/exec.h"
#include "../../drivers/video/drm/memory.h"
#include "../../drivers/video/drm/sync.h"
#include <reliefnt/device_mapping.h>
#include <reliefnt/heap.h>
#include <reliefnt/syscall_internal.h>

#define REPLY_ADDRESS 0x4000u
#define COMMAND_ADDRESS 0x8000u
#define FENCE_HANDLE 13u
#define FENCE_FD 7

struct svga_device svga;
struct drm_bo { struct device_mapping_access access; unsigned references, reservations; };
struct drm_sync_fence { unsigned references; int status; };
static struct drm_bo bo;
static struct drm_sync_fence sync;
static struct svga_fence fence;
static struct drm_file file;
static struct task task;
static uint32_t sent, completed;
static unsigned waits, copyouts, batches;
static bool handle_live, fd_live, timeout;
static const uint32_t command[] = {SVGA_3D_CMD_NOP, 0};

void *kernel_malloc(size_t bytes) { return malloc(bytes); }
void kernel_free(void *ptr) { free(ptr); }
void *drm_async_alloc(size_t bytes) { ++batches; return malloc(bytes); }
void drm_async_free(void *ptr) { assert(batches); --batches; free(ptr); }
uint64_t time_ticks(void) { return 0; }
void sched_sleep_current_until(uint64_t tick) { (void)tick; abort(); }

bool user_range_writable(uint64_t address, uint64_t bytes)
{
    return address == REPLY_ADDRESS && bytes == sizeof(struct drm_vmw_fence_rep);
}

int user_copy_from_task(struct task *caller, void *dst, uint64_t address, uint64_t bytes)
{
    assert(caller == &task && address == COMMAND_ADDRESS && bytes == sizeof(command));
    __builtin_memcpy(dst, command, bytes);
    return 0;
}

int user_copy_to_task(struct task *caller, uint64_t address, const void *data, uint64_t bytes)
{
    const struct drm_vmw_fence_rep *rep = data;
    assert(caller == &task && user_range_writable(address, bytes) && sent > completed);
    assert(rep->handle == FENCE_HANDLE && rep->seqno == sent && rep->fd == FENCE_FD);
    assert(fd_live && handle_live && fence.object.references == 2 && bo.reservations == 1);
    ++copyouts;
    return -EFAULT;
}

struct svga_object *svga_object_create(struct drm_file *owner, enum svga_object_type type, size_t size)
{
    assert(owner == &file && type == SVGA_FENCE && size == sizeof(fence));
    assert(!handle_live && !fence.object.references);
    handle_live = true;
    fence = (struct svga_fence){.object = {.type = type, .handle = FENCE_HANDLE, .references = 1}};
    return &fence.object;
}
void svga_object_get(struct svga_object *object) { assert(object == &fence.object); ++object->references; }
void svga_object_put(struct svga_object *object) { assert(object == &fence.object && object->references); --object->references; }
int svga_object_unref(struct drm_file *owner, enum svga_object_type type, uint32_t handle)
{
    assert(owner == &file && type == SVGA_FENCE && handle == FENCE_HANDLE && handle_live);
    handle_live = false;
    svga_object_put(&fence.object);
    return 0;
}
struct svga_object *svga_object_lookup(const struct drm_file *owner, enum svga_object_type type,
                                       uint32_t handle)
{ (void)owner; (void)type; (void)handle; abort(); }
void svga_fence_fill(const struct svga_fence *f, struct drm_vmw_fence_rep *rep)
{
    *rep = (struct drm_vmw_fence_rep){.handle = f->object.handle, .seqno = f->sequence};
}

struct drm_sync_fence *drm_sync_fence_create(const char *driver, const char *timeline)
{
    (void)driver; (void)timeline;
    assert(!sync.references);
    sync = (struct drm_sync_fence){.references = 1};
    return &sync;
}
void drm_sync_fence_put(struct drm_sync_fence *f)
{ assert(f == &sync && f->references); --f->references; }
void drm_sync_fence_signal(struct drm_sync_fence *f, int status)
{ assert(f == &sync && f->references && !f->status); f->status = status ? status : 1; }
int drm_sync_wait_fd(struct task *caller, int fd) { (void)caller; (void)fd; abort(); }
int drm_sync_export(struct task *caller, struct drm_sync_fence *f, int *fd)
{
    assert(caller == &task && f == &sync && !fd_live);
    ++sync.references;
    fd_live = true;
    *fd = FENCE_FD;
    return 0;
}
void task_discard_file_fd(struct task *caller, int fd)
{
    assert(caller == &task && fd == FENCE_FD && fd_live);
    fd_live = false;
    drm_sync_fence_put(&sync);
}

void drm_bo_get(struct drm_bo *b) { ++b->references; }
void drm_bo_put(struct drm_bo *b) { if (b) { assert(b->references); --b->references; } }
int drm_bo_gpu_reserve(struct drm_bo *b) { ++b->reservations; return 0; }
void drm_bo_gpu_release(struct drm_bo *b) { assert(b->reservations); --b->reservations; }
struct device_mapping_access *drm_bo_mapping_access(struct drm_bo *b) { return &b->access; }
int device_mapping_revoke_cpu(struct device_mapping_access *access) { (void)access; abort(); }
bool svga_bo_is_coherent(const struct drm_bo *b) { (void)b; return false; }
bool svga_surface_is_coherent(const struct svga_surface *s) { (void)s; abort(); }
int svga_surface_validate_residency(struct svga_surface *s) { (void)s; abort(); }
void svga_surface_set_bo(struct svga_surface *s, struct drm_bo *b) { (void)s; (void)b; abort(); }
struct drm_bo *svga_region_bo(const struct svga_region *r) { (void)r; abort(); }
uint32_t svga_region_release(struct svga_region *r) { assert(!r); return 0; }
uint32_t svga_dma_retire(uint32_t budget) { (void)budget; return 0; }
uint32_t svga_fence_events_retire(uint32_t budget) { (void)budget; return 0; }

int svga_context_validate_residency(struct svga_context *ctx) { (void)ctx; abort(); }
int svga_context_validate_bindings(struct svga_batch *b, struct svga_context *ctx)
{ (void)b; (void)ctx; abort(); }
int svga_context_prepare(struct svga_batch *b) { assert(!b->dx); return 0; }
void svga_context_finish(struct svga_batch *b, bool commit) { assert(!b->dx && commit); }
void svga_query_finish(const struct svga_batch *b, bool commit) { assert(!b->query_context && commit); }
int svga_validate_legacy(struct svga_batch *b, const struct svga_packet *packet)
{
    /* Stand in for validation of an operand backed by this BO. */
    assert(packet->command == command[0] && !packet->bytes);
    return svga_batch_bo(b, &bo, NULL);
}
int svga_validate_dx(struct svga_batch *b, const struct svga_packet *p) { (void)b; (void)p; abort(); }

bool svga_submit_has_context(void) { return false; }
uint32_t svga_submit_limit(void) { return SVGA_PAGE_SIZE; }
int svga_submit_stage(uint32_t bytes, uint32_t preserve, void **out)
{ (void)bytes; (void)preserve; (void)out; abort(); }
int svga_submit_staged(uint32_t bytes, uint32_t ctx, uint32_t *out)
{ (void)bytes; (void)ctx; (void)out; abort(); }
int svga_submit_commands(const void *data, uint32_t bytes, uint32_t ctx, uint32_t *out)
{
    assert(ctx == SVGA3D_INVALID_ID && bytes == sizeof(command));
    assert(!__builtin_memcmp(data, command, bytes) && bo.reservations == 1);
    *out = ++sent;
    return 0;
}
int svga_submit_poll(uint32_t value) { return value <= completed ? 1 : svga.fifo.fault ? -EIO : 0; }
int svga_submit_wait(uint32_t value)
{
    assert(value == sent && !fd_live && !handle_live && bo.reservations == 1);
    ++waits;
    if (timeout) {
        svga.fifo.fault = true;
        return -ETIMEDOUT;
    }
    completed = value;
    return 0;
}

static void test_reply_fault(bool stall)
{
    timeout = stall;
    uint32_t before = sent;
    const struct drm_vmw_execbuf_arg args = {
        .commands = COMMAND_ADDRESS, .command_size = sizeof(command),
        .version = DRM_VMW_EXECBUF_VERSION, .context_handle = SVGA3D_INVALID_ID,
        .fence_rep = REPLY_ADDRESS, .flags = DRM_VMW_EXECBUF_FLAG_EXPORT_FENCE_FD,
    };
    assert(svga_execbuf(&task, &file, &args) == -EFAULT);
    assert(sent == before + 1 && waits == sent && copyouts == sent);
    assert(!fd_live && !handle_live);
    assert(fence.status == (stall ? -EIO : 1) && sync.status == fence.status);
    assert(!fence.object.references && !sync.references);
    assert(bo.reservations == stall && bo.references == stall && batches == stall);
    if (stall)
        assert(svga_execbuf(&task, &file, &args) == -EIO && sent == before + 1);
}

int main(void)
{
    test_reply_fault(false);
    test_reply_fault(true);
    svga_execbuf_fini();
    puts("PASS SVGA execbuf late reply fault: completion and timeout retain only DMA ownership");
}
