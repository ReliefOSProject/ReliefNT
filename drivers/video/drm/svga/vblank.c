/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include <reliefnt/futex.h>
#include <reliefnt/heap.h>
#include <reliefnt/module.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/time.h>
#include <reliefnt/workqueue.h>

struct svga_vblank_event {
    struct svga_vblank_event *next;
    struct drm_file *file;
    uint64_t target, user_data;
    unsigned unit;
    uint32_t type;
};

static struct svga_vblank_event *events;
static struct kernel_work *vblank_work;
static struct {
    uint64_t origin_us, origin_sequence, interval_us;
} clocks[SVGA_MAX_OUTPUTS];

/* SVGA II exposes command progress but no display retrace IRQ. Advance the
 * logical clock from the current mode, avoiding a timer on idle displays. */
static uint64_t sample(unsigned unit, uint64_t now, uint64_t *timestamp)
{
    uint64_t elapsed = now > clocks[unit].origin_us ? now - clocks[unit].origin_us : 0;
    uint64_t interval = clocks[unit].interval_us;
    uint64_t frames = interval ? elapsed / interval : 0;
    *timestamp = clocks[unit].origin_us + frames * interval;
    return clocks[unit].origin_sequence + frames;
}

static void send_event(struct svga_vblank_event *event, uint64_t sequence, uint64_t timestamp)
{
    if (event->type == DRM_EVENT_CRTC_SEQUENCE) {
        const struct drm_event_crtc_sequence reply = {
            .base = {event->type, sizeof(reply)}, .user_data = event->user_data,
            .sequence = sequence, .time_ns = timestamp * 1000,
        };
        svga_event_publish(event->file, &reply.base);
    } else {
        const struct drm_event_vblank reply = {
            .base = {event->type, sizeof(reply)}, .user_data = event->user_data,
            .sequence = sequence, .crtc_id = SVGA_CRTC_ID + event->unit,
            .tv_sec = timestamp / 1000000, .tv_usec = timestamp % 1000000,
        };
        svga_event_publish(event->file, &reply.base);
    }
    kernel_free(event);
}

static void complete_events(void)
{
    uint64_t now = time_uptime_us();
    struct svga_vblank_event **link = &events;
    while (*link) {
        struct svga_vblank_event *event = *link;
        unsigned unit = event->unit;
        uint64_t timestamp;
        uint64_t sequence = sample(unit, now, &timestamp);
        if (clocks[unit].interval_us && sequence < event->target) {
            link = &event->next;
            continue;
        }
        *link = event->next;
        send_event(event, sequence, timestamp);
    }
}

static void vblank_tick(struct kernel_work *work, void *context)
{
    (void)context;
    complete_events();
    if (events)
        kernel_queue_delayed_work(work, 1);
}

int svga_vblank_init(void)
{
    return kernel_work_create(vblank_tick, NULL, THIS_MODULE, &vblank_work);
}

void svga_vblank_fini(void)
{
    svga_vblank_release(NULL);
    if (vblank_work) {
        (void)kernel_work_destroy(vblank_work);
        vblank_work = NULL;
    }
    __builtin_memset(clocks, 0, sizeof(clocks));
}

void svga_vblank_set(unsigned unit, const struct svga_scanout *scanout)
{
    const struct drm_mode_modeinfo *mode = &scanout->mode;
    uint64_t interval = scanout->active && mode->clock ?
        (uint64_t)mode->htotal * mode->vtotal * 1000 / mode->clock : 0;
    if (scanout->active && !interval)
        interval = 16667;
    if (clocks[unit].interval_us == interval)
        return;
    uint64_t now = time_uptime_us(), timestamp;
    clocks[unit].origin_sequence = sample(unit, now, &timestamp);
    clocks[unit].origin_us = interval ? now : timestamp;
    clocks[unit].interval_us = interval;
    if (!interval)
        complete_events();
}

int svga_vblank_reserve(struct drm_file *file, unsigned unit, uint32_t type,
                        uint64_t user_data, struct svga_vblank_event **out)
{
    struct svga_vblank_event *event = kernel_malloc(sizeof(*event));
    if (!event)
        return -ENOMEM;
    int ret = svga_event_reserve(file);
    if (ret) {
        kernel_free(event);
        return ret;
    }
    *event = (struct svga_vblank_event){.file = file, .unit = unit, .type = type,
                                       .user_data = user_data};
    *out = event;
    return 0;
}

void svga_vblank_cancel(struct svga_vblank_event *event)
{
    if (!event)
        return;
    svga_event_cancel(event->file);
    kernel_free(event);
}

void svga_vblank_release(struct drm_file *file)
{
    struct svga_vblank_event **link = &events;
    while (*link) {
        struct svga_vblank_event *event = *link;
        if (!file || file == event->file) {
            *link = event->next;
            svga_vblank_cancel(event);
        } else {
            link = &event->next;
        }
    }
}

static void arm_event(struct svga_vblank_event *event, uint64_t target,
                      uint64_t sequence, uint64_t timestamp)
{
    if (target <= sequence || !clocks[event->unit].interval_us) {
        send_event(event, sequence, timestamp);
        return;
    }
    struct svga_vblank_event **link = &events;
    while (*link)
        link = &(*link)->next;
    event->target = target;
    *link = event;
    kernel_queue_delayed_work(vblank_work, 1);
}

void svga_vblank_arm_flip(struct svga_vblank_event *event, bool immediate)
{
    if (!event)
        return;
    uint64_t timestamp;
    uint64_t sequence = sample(event->unit, time_uptime_us(), &timestamp);
    arm_event(event, sequence + !immediate, sequence, timestamp);
}

static uint64_t relative_target(uint64_t sequence, uint64_t target)
{
    return target <= UINT64_MAX - sequence ? sequence + target : UINT64_MAX;
}

static int wait_vblank(struct task *task, struct drm_file *file, union drm_wait_vblank *arg)
{
    const uint32_t flags = arg->request.type;
    const uint32_t supported = _DRM_VBLANK_RELATIVE | _DRM_VBLANK_HIGH_CRTC_MASK |
        _DRM_VBLANK_EVENT | _DRM_VBLANK_NEXTONMISS | _DRM_VBLANK_SECONDARY;
    unsigned unit = (flags & _DRM_VBLANK_HIGH_CRTC_MASK) >> _DRM_VBLANK_HIGH_CRTC_SHIFT;
    if (flags & _DRM_VBLANK_SECONDARY)
        unit = 1;
    if ((flags & ~supported) || unit >= svga_num_outputs || !clocks[unit].interval_us)
        return -EINVAL;

    uint64_t now = time_uptime_us(), timestamp;
    uint64_t sequence = sample(unit, now, &timestamp);
    int64_t delta = flags & _DRM_VBLANK_RELATIVE ? (int64_t)arg->request.sequence :
        (int32_t)(arg->request.sequence - (uint32_t)sequence);
    if ((flags & _DRM_VBLANK_NEXTONMISS) && delta <= 0)
        delta = 1;
    uint64_t target = delta > 0 ? relative_target(sequence, delta) : sequence;
    if (flags & _DRM_VBLANK_EVENT) {
        struct svga_vblank_event *event;
        int ret = svga_vblank_reserve(file, unit, DRM_EVENT_VBLANK, arg->request.signal, &event);
        if (ret)
            return ret;
        arm_event(event, target, sequence, timestamp);
        arg->reply.sequence = target;
        return 0;
    }
    struct task_lookup_scope *scope = &task->lookup_scope;
    uint64_t deadline = scope->ioctl_deadline_us;
    if (deadline) {
        target = scope->ioctl_target_sequence;
    } else {
        deadline = now + 3000000;
    }
    if (sequence < target) {
        if (now >= deadline)
            return -EBUSY;
        uint64_t frames = target - sequence, interval = clocks[unit].interval_us;
        uint64_t delay = deadline - now;
        if (frames <= (deadline - timestamp) / interval)
            delay = timestamp + frames * interval - now;
        scope->ioctl_deadline_us = deadline;
        scope->ioctl_target_sequence = target;
        const uint64_t tick_us = 1000000 / RELIEFNT_TICK_HZ;
        sched_sleep_current_until(time_ticks() + (delay + tick_us - 1) / tick_us);
        return KERNEL_SYSCALL_BLOCKED;
    }
    arg->reply = (struct drm_wait_vblank_reply){
        .type = flags & ~_DRM_VBLANK_RELATIVE, .sequence = sequence,
        .tval_sec = timestamp / 1000000, .tval_usec = timestamp % 1000000,
    };
    return 0;
}

int svga_vblank_ioctl(struct task *task, struct drm_file *file, uint32_t command, void *data)
{
    if (command == DRM_IOCTL_WAIT_VBLANK)
        return wait_vblank(task, file, data);
    if (command == DRM_IOCTL_CRTC_GET_SEQUENCE) {
        struct drm_crtc_get_sequence *arg = data;
        int unit = svga_output_index(arg->crtc_id, SVGA_CRTC_ID);
        if (unit < 0)
            return -ENOENT;
        uint64_t timestamp;
        arg->active = clocks[unit].interval_us != 0;
        arg->sequence = sample(unit, time_uptime_us(), &timestamp);
        arg->sequence_ns = timestamp * 1000;
        return 0;
    }
    struct drm_crtc_queue_sequence *arg = data;
    int unit = svga_output_index(arg->crtc_id, SVGA_CRTC_ID);
    if (unit < 0)
        return -ENOENT;
    if (!clocks[unit].interval_us ||
        (arg->flags & ~(DRM_CRTC_SEQUENCE_RELATIVE | DRM_CRTC_SEQUENCE_NEXT_ON_MISS)))
        return -EINVAL;
    uint64_t timestamp;
    uint64_t sequence = sample(unit, time_uptime_us(), &timestamp);
    uint64_t target = arg->sequence;
    if (arg->flags & DRM_CRTC_SEQUENCE_RELATIVE)
        target = relative_target(sequence, target);
    if ((arg->flags & DRM_CRTC_SEQUENCE_NEXT_ON_MISS) && target <= sequence)
        target = sequence + 1;
    struct svga_vblank_event *event;
    int ret = svga_vblank_reserve(file, unit, DRM_EVENT_CRTC_SEQUENCE, arg->user_data, &event);
    if (ret)
        return ret;
    arm_event(event, target, sequence, timestamp);
    arg->sequence = target > sequence ? target : sequence;
    return 0;
}
