/* SPDX-License-Identifier: Apache-2.0 */
#include "exec.h"
#include "kms.h"
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/futex.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/time.h>
#include <linux/time.h>

#define FENCE_EVENT_LIMIT 1024u

struct svga_fence_event {
    struct svga_fence_event *next, **previous;
    struct svga_fence_event *owner_next, **owner_previous;
    struct drm_file *file;
    struct svga_fence *fence;
    struct drm_vmw_event_fence reply;
    uint32_t sequence;
    bool timestamp;
};

static struct svga_fence_event *events, **tail = &events;
static unsigned event_count;

/* Wait cookies survive syscall replay on another CPU. The event timestamp
 * is in the same monotonic domain as sync_file and userspace clock_gettime. */
static uint64_t monotonic_us(void)
{
    struct linux_timespec now;
    (void)time_clock_get(LINUX_CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

void svga_fence_fill(const struct svga_fence *fence, struct drm_vmw_fence_rep *rep)
{
    /* Match vmw_execbuf_copy_fence_user(): leave mask zero because Mesa
     * treats masked flags as already satisfied, even on a pending fence. */
    *rep = (struct drm_vmw_fence_rep){
        .handle = fence->object.handle, .seqno = fence->sequence,
        .passed_seqno = svga_submit_completed(), .fd = -1,
    };
}

static int fence_status(struct svga_fence *fence)
{
    if (!fence->status) {
        int ret = svga_submit_poll(fence->sequence);
        if (ret)
            fence->status = ret;
    }
    return fence->status;
}

static void free_event(struct svga_fence_event *event)
{
    *event->previous = event->next;
    if (event->next)
        event->next->previous = event->previous;
    else
        tail = event->previous;
    *event->owner_previous = event->owner_next;
    if (event->owner_next)
        event->owner_next->owner_previous = event->owner_previous;
    if (event->fence)
        svga_object_put(&event->fence->object);
    --event_count;
    kernel_free(event);
}

void svga_fence_events_release(struct svga_render_file *state)
{
    while (state->fence_events)
        free_event(state->fence_events);
}

uint32_t svga_fence_events_retire(uint32_t budget)
{
    uint32_t count = 0;
    for (struct svga_fence_event *event = events, *next; event && count < budget; event = next) {
        next = event->next;
        int status = event->fence ? fence_status(event->fence) : svga_submit_poll(event->sequence);
        if (!status)
            continue;
        if (event->timestamp) {
            uint64_t usec = monotonic_us();
            event->reply.tv_sec = usec / 1000000;
            event->reply.tv_usec = usec % 1000000;
        }
        svga_event_publish(event->file, &event->reply.base);
        free_event(event);
        ++count;
    }
    return count;
}

static int fence_event(struct task *task, struct drm_file *file,
                       const struct drm_vmw_fence_event_arg *arg)
{
    if (arg->flags & ~DRM_VMW_FE_FLAG_REQ_TIME)
        return -EINVAL;
    if (event_count >= FENCE_EVENT_LIMIT)
        return -EBUSY;
    int ret;
    if (arg->fence_rep) {
        ret = svga_prepare_output(task, arg->fence_rep, sizeof(struct drm_vmw_fence_rep));
        if (ret)
            return ret;
    }
    struct svga_render_file *state = svga_render_file_get(file);
    if (!state)
        return -ENOMEM;
    ret = svga_event_reserve(file);
    if (ret)
        return ret;
    struct svga_fence_event *event = kernel_malloc(sizeof(*event));
    if (!event) {
        svga_event_cancel(file);
        return -ENOMEM;
    }
    uint32_t sequence = 0;
    struct svga_fence *fence = NULL;
    if (arg->handle) {
        fence = (struct svga_fence *)svga_object_lookup(file, SVGA_FENCE, arg->handle);
        if (!fence || (arg->fence_rep && fence->object.handle_refs == UINT32_MAX)) {
            ret = -EINVAL;
            goto fail;
        }
        sequence = fence->sequence;
        if (arg->fence_rep)
            ++fence->object.handle_refs;
    } else {
        ret = svga_submit_checkpoint(&sequence);
        if (ret)
            goto fail;
        if (arg->fence_rep) {
            fence = (struct svga_fence *)svga_object_create(file, SVGA_FENCE, sizeof(*fence));
            if (!fence) {
                ret = -ENOMEM;
                goto fail;
            }
            fence->sequence = sequence;
        }
    }
    if (arg->fence_rep) {
        struct drm_vmw_fence_rep rep;
        svga_fence_fill(fence, &rep);
        if (user_copy_to_task(task, arg->fence_rep, &rep, sizeof(rep)) < 0) {
            (void)svga_object_unref(file, SVGA_FENCE, rep.handle);
            ret = -EFAULT;
            goto fail;
        }
    }
    if (fence)
        svga_object_get(&fence->object);
    *event = (struct svga_fence_event){
        .previous = tail, .owner_next = state->fence_events,
        .owner_previous = &state->fence_events, .file = file, .sequence = sequence,
        .fence = fence, .timestamp = arg->flags & DRM_VMW_FE_FLAG_REQ_TIME,
        .reply = {.base = {DRM_VMW_EVENT_FENCE_SIGNALED, sizeof(event->reply)},
                  .user_data = arg->user_data},
    };
    *tail = event;
    tail = &event->next;
    if (event->owner_next)
        event->owner_next->owner_previous = &event->owner_next;
    state->fence_events = event;
    ++event_count;
    (void)svga_fence_events_retire(FENCE_EVENT_LIMIT);
    return 0;

fail:
    kernel_free(event);
    svga_event_cancel(file);
    return ret;
}

static int fence_wait(struct task *task, struct drm_file *file, struct drm_vmw_fence_wait_arg *arg)
{
    if (arg->wait_options & ~DRM_VMW_WAIT_OPTION_UNREF)
        return -EINVAL;
    struct svga_fence *fence = (struct svga_fence *)svga_object_lookup(file, SVGA_FENCE, arg->handle);
    if (!fence)
        return -EINVAL;
    uint64_t *deadline = task && task->lookup_scope.armed ?
        &task->lookup_scope.ioctl_deadline_us : NULL;
    uint64_t now = monotonic_us();
    /* Syscall replay may import the original user snapshot, without the
     * cookie that we copied out on the preceding wait. */
    if (deadline && *deadline) {
        arg->kernel_cookie = *deadline;
    } else if (!arg->cookie_valid) {
        arg->kernel_cookie = arg->timeout_us > UINT64_MAX - now ? UINT64_MAX : now + arg->timeout_us;
    }
    arg->cookie_valid = 1;
    int status = fence_status(fence);
    if (status) {
        if (status < 0)
            return status;
        if (arg->wait_options & DRM_VMW_WAIT_OPTION_UNREF)
            return svga_object_unref(file, SVGA_FENCE, arg->handle);
        return 0;
    }
    if (now >= arg->kernel_cookie)
        return -EBUSY;
    if (deadline)
        *deadline = arg->kernel_cookie;
    sched_sleep_current_until(time_ticks() + 1);
    return KERNEL_SYSCALL_BLOCKED;
}

int svga_fence_ioctl(struct task *task, struct drm_file *file, uint32_t command, void *data)
{
    (void)svga_render_retire(128);
    switch (command) {
    case VMW_FENCE_EVENT:
        return fence_event(task, file, data);
    case VMW_FENCE_UNREF:
        return svga_object_unref(file, SVGA_FENCE, ((const struct drm_vmw_fence_arg *)data)->handle);
    case VMW_FENCE_WAIT:
        return fence_wait(task, file, data);
    case VMW_FENCE_SIGNALED: {
        struct drm_vmw_fence_signaled_arg *arg = data;
        struct svga_fence *fence = (struct svga_fence *)svga_object_lookup(file, SVGA_FENCE, arg->handle);
        if (!fence)
            return -EINVAL;
        int status = fence_status(fence);
        if (status < 0)
            return status;
        arg->signaled = status != 0;
        arg->signaled_flags = arg->flags;
        arg->passed_seqno = svga_submit_completed();
        return 0;
    }
    default:
        return -EINVAL;
    }
}
