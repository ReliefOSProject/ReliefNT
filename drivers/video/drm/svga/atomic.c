/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include "render.h"
#include <reliefnt/console.h>
#include <reliefnt/module.h>
#include <reliefnt/workqueue.h>

#define SVGA_ATOMIC_MAX_OBJECTS (SVGA_MAX_OUTPUTS * 4)
/* CRTC, connector, primary plane and cursor plane respectively. */
#define SVGA_ATOMIC_MAX_PROPS (SVGA_MAX_OUTPUTS * (2 + 1 + 11 + 12))

enum atomic_object { ATOMIC_CRTC, ATOMIC_CONNECTOR, ATOMIC_PRIMARY, ATOMIC_CURSOR };

/* The device has one outstanding asynchronous modeset. Framebuffers remain
 * owned by the submitting OFD. Normal KMS operations drain it before changing
 * their state; final close and VT handoff cancel it while the OFD is still live. */
static struct {
    struct drm_file *file;
    struct svga_scanout state[SVGA_MAX_OUTPUTS];
    struct svga_vblank_event *events[SVGA_MAX_OUTPUTS];
    unsigned scanouts, cursors;
} queued;
static struct kernel_work *commit_work;

static void release_commit(bool ran, bool failed)
{
    for (unsigned i = 0; i < svga_num_outputs; ++i) {
        if ((queued.scanouts | queued.cursors) & (1u << i)) {
            svga_blob_put(queued.state[i].mode_blob);
            svga_blob_put(queued.state[i].damage_blob);
        }
        if (ran)
            svga_vblank_arm_flip(queued.events[i], failed);
        else
            svga_vblank_cancel(queued.events[i]);
    }
    queued.scanouts = queued.cursors = 0;
}

void svga_atomic_drain(void)
{
    if (!queued.file)
        return;
    /* Clear admission before any fault path calls the VT release hook. */
    queued.file = NULL;
    int ret = svga_kms_commit(queued.state, queued.scanouts, queued.cursors);
    if (ret)
        console_warn("[svga] asynchronous display commit failed: %d", ret);
    /* An accepted nonblocking commit has no ioctl error return here, and the
     * DRM flip event carries no status. After rollback or display release,
     * complete the waiter without simulating a retrace of the failed frame. */
    release_commit(true, ret != 0);
}

static void execute_commit(struct kernel_work *work, void *context)
{
    (void)work;
    (void)context;
    svga_atomic_drain();
}

int svga_atomic_init(void)
{
    return kernel_work_create(execute_commit, NULL, THIS_MODULE, &commit_work);
}

void svga_atomic_cancel(struct drm_file *file)
{
    if (queued.file && (!file || file == queued.file)) {
        queued.file = NULL;
        release_commit(false, false);
    }
}

void svga_atomic_fini(void)
{
    svga_atomic_cancel(NULL);
    if (commit_work) {
        (void)kernel_work_destroy(commit_work);
        commit_work = NULL;
    }
}

int svga_queue_commit(struct drm_file *file, const struct svga_scanout *state,
                      unsigned scanouts, unsigned cursors,
                      struct svga_vblank_event *const *events)
{
    if (queued.file)
        return -EBUSY;
    if (svga.fifo.fault || !commit_work)
        return -EIO;
    /* Work runs under the same execution owner as ioctl. A previous queued
     * work generation may still be ready after an inline drain. */
    kernel_queue_work(commit_work);
    queued.file = file;
    queued.scanouts = scanouts;
    queued.cursors = cursors;
    __builtin_memcpy(queued.state, state, sizeof(queued.state));
    __builtin_memcpy(queued.events, events, sizeof(queued.events));
    for (unsigned i = 0; i < svga_num_outputs; ++i)
        if ((scanouts | cursors) & (1u << i)) {
            svga_blob_get(queued.state[i].mode_blob);
            svga_blob_get(queued.state[i].damage_blob);
        }
    return 0;
}

static int set_property(struct drm_file *file, unsigned unit, enum atomic_object type,
                        struct svga_scanout *state, uint32_t property, uint64_t value)
{
    if (type == ATOMIC_CURSOR)
        return svga_cursor_set_property(file, unit, &state->cursor, property, value);
    if (type == ATOMIC_CRTC) {
        switch (property) {
        case SVGA_PROP_ACTIVE:
            if (value > 1)
                return -EINVAL;
            state->active = value;
            return 0;
        case SVGA_PROP_MODE_ID:
            if (value > UINT32_MAX)
                return -EINVAL;
            state->mode_blob = value ? svga_blob_lookup(value, sizeof(state->mode)) : NULL;
            if (value && !state->mode_blob)
                return -EINVAL;
            if (state->mode_blob) {
                __builtin_memcpy(&state->mode, svga_blob_data(state->mode_blob), sizeof(state->mode));
                state->mode.name[sizeof(state->mode.name) - 1] = 0;
            } else {
                state->mode = (struct drm_mode_modeinfo){0};
            }
            return 0;
        default:
            return -EINVAL;
        }
    }

    uint32_t crtc_id = SVGA_CRTC_ID + unit;
    if (type == ATOMIC_CONNECTOR) {
        if (property != SVGA_PROP_CONNECTOR_CRTC || (value && value != crtc_id))
            return -EINVAL;
        state->connector_routed = value != 0;
        return 0;
    }
    switch (property) {
    case SVGA_PROP_PLANE_CRTC:
        if (value && value != crtc_id)
            return -EINVAL;
        state->plane_routed = value != 0;
        break;
    case SVGA_PROP_FB_ID:
        if (value > UINT32_MAX)
            return -EINVAL;
        state->fb = value ? svga_fb_lookup(file, value) : NULL;
        if (value && !state->fb)
            return -ENOENT;
        break;
    case SVGA_PROP_DAMAGE_CLIPS:
        if (value > UINT32_MAX)
            return -EINVAL;
        state->damage_blob = value ? svga_damage_lookup(value) : NULL;
        if (value && !state->damage_blob)
            return -EINVAL;
        break;
    case SVGA_PROP_SRC_X:
    case SVGA_PROP_SRC_Y:
        if (value > UINT32_MAX || (value & 0xffff))
            return -EINVAL;
        if (property == SVGA_PROP_SRC_X)
            state->x = value >> 16;
        else
            state->y = value >> 16;
        break;
    case SVGA_PROP_SRC_W:
    case SVGA_PROP_SRC_H:
        if (value > UINT32_MAX)
            return -EINVAL;
        if (property == SVGA_PROP_SRC_W)
            state->src_w = value;
        else
            state->src_h = value;
        break;
    case SVGA_PROP_CRTC_X:
    case SVGA_PROP_CRTC_Y:
        if ((int64_t)value < INT32_MIN || (int64_t)value > INT32_MAX)
            return -EINVAL;
        if (property == SVGA_PROP_CRTC_X)
            state->dst_x = value;
        else
            state->dst_y = value;
        break;
    case SVGA_PROP_CRTC_W:
    case SVGA_PROP_CRTC_H:
        if (value > INT32_MAX)
            return -EINVAL;
        if (property == SVGA_PROP_CRTC_W)
            state->dst_w = value;
        else
            state->dst_h = value;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static bool needs_modeset(const struct svga_scanout *old, const struct svga_scanout *state)
{
    return old->active != state->active || old->connector_routed != state->connector_routed ||
        old->root_x != state->root_x || old->root_y != state->root_y ||
        __builtin_memcmp(&old->mode, &state->mode, sizeof(old->mode));
}

/* One snapshot of every operand is imported before mode validation. TEST_ONLY
 * does no device I/O and holds no additional mode, framebuffer or event refs. */
int svga_atomic_ioctl(struct task *task, struct drm_file *file, const struct drm_mode_atomic *arg)
{
    if (!file->atomic_modeset || arg->reserved || (arg->flags & ~DRM_MODE_ATOMIC_FLAGS) ||
        ((arg->flags & DRM_MODE_ATOMIC_TEST_ONLY) && (arg->flags & DRM_MODE_PAGE_FLIP_EVENT)))
        return -EINVAL;
    if (arg->flags & DRM_MODE_PAGE_FLIP_ASYNC)
        return -EINVAL;
    if (queued.file && (arg->flags & DRM_MODE_ATOMIC_NONBLOCK))
        return -EBUSY;
    svga_atomic_drain();
    bool cursor_available = svga_cursor_plane_available(file);
    if (arg->count_objs > svga_num_outputs * (cursor_available ? 4u : 3u))
        return -E2BIG;
    if (!arg->count_objs)
        return arg->flags & DRM_MODE_PAGE_FLIP_EVENT ? -EINVAL : 0;

    uint32_t objects[SVGA_ATOMIC_MAX_OBJECTS], counts[SVGA_ATOMIC_MAX_OBJECTS];
    int ret = svga_import(task, objects, arg->objs_ptr, arg->count_objs * sizeof(objects[0]));
    if (!ret)
        ret = svga_import(task, counts, arg->count_props_ptr, arg->count_objs * sizeof(counts[0]));
    if (ret)
        return ret;
    unsigned total = 0;
    for (unsigned i = 0; i < arg->count_objs; ++i) {
        if (counts[i] > SVGA_ATOMIC_MAX_PROPS - total)
            return -E2BIG;
        total += counts[i];
    }
    if (!total)
        return arg->flags & DRM_MODE_PAGE_FLIP_EVENT ? -EINVAL : 0;

    uint32_t properties[SVGA_ATOMIC_MAX_PROPS];
    uint64_t values[SVGA_ATOMIC_MAX_PROPS];
    ret = svga_import(task, properties, arg->props_ptr, total * sizeof(properties[0]));
    if (!ret)
        ret = svga_import(task, values, arg->prop_values_ptr, total * sizeof(values[0]));
    if (ret)
        return ret;

    struct svga_scanout state[SVGA_MAX_OUTPUTS];
    __builtin_memcpy(state, svga_scanouts, sizeof(state));
    unsigned scanouts = 0, touched[ATOMIC_CURSOR + 1] = {0}, value_index = 0;
    for (unsigned i = 0; i < arg->count_objs; ++i) {
        if (!counts[i])
            continue;
        enum atomic_object type = ATOMIC_CRTC;
        int unit = svga_output_index(objects[i], SVGA_CRTC_ID);
        if (unit < 0) {
            type = ATOMIC_CONNECTOR;
            unit = svga_output_index(objects[i], SVGA_CONNECTOR_ID);
        }
        if (unit < 0) {
            type = ATOMIC_PRIMARY;
            unit = svga_output_index(objects[i], SVGA_PLANE_ID);
        }
        if (unit < 0 && cursor_available) {
            type = ATOMIC_CURSOR;
            unit = svga_output_index(objects[i], SVGA_CURSOR_PLANE_ID);
        }
        if (unit < 0)
            return -ENOENT;
        struct svga_scanout *s = &state[unit];
        if (s->owner && s->owner != file)
            return -EACCES;
        /* Each commit supplies its own damage hint. */
        if (type != ATOMIC_CURSOR && !(scanouts & (1u << unit)))
            s->damage_blob = NULL;
        for (unsigned j = 0; j < counts[i]; ++j, ++value_index) {
            ret = set_property(file, unit, type, s, properties[value_index], values[value_index]);
            if (ret)
                return ret;
        }
        touched[type] |= 1u << unit;
        if (type != ATOMIC_CURSOR)
            scanouts |= 1u << unit;
    }

    unsigned cursors = touched[ATOMIC_CURSOR], events = 0;
    for (unsigned i = 0; i < svga_num_outputs; ++i) {
        unsigned bit = 1u << i;
        if (!((scanouts | cursors) & bit))
            continue;
        struct svga_scanout *s = &state[i];
        const struct svga_scanout *old = &svga_scanouts[i];
        if (scanouts & bit) {
            s->owner = s->mode_blob ? file : NULL;
            s->root_x = s->mode_blob ? svga_layout[i].x : 0;
            s->root_y = s->mode_blob ? svga_layout[i].y : 0;
            if (needs_modeset(old, s)) {
                if (!(arg->flags & DRM_MODE_ATOMIC_ALLOW_MODESET))
                    return -EINVAL;
                s->damage_blob = NULL;
            }
        }
        if (!(arg->flags & DRM_MODE_PAGE_FLIP_EVENT))
            continue;
        /* Like DRM atomic state, include the old and new CRTC of a routed
         * object. The event request is all-or-nothing for those CRTCs. */
        bool has_crtc = (touched[ATOMIC_CRTC] & bit) ||
            ((touched[ATOMIC_CONNECTOR] & bit) && (old->connector_routed || s->connector_routed)) ||
            ((touched[ATOMIC_PRIMARY] & bit) && (old->plane_routed || s->plane_routed)) ||
            ((cursors & bit) && (old->cursor.routed || s->cursor.routed));
        if (has_crtc) {
            if (!old->active && !s->active)
                return -EINVAL;
            events |= bit;
        }
    }
    if ((arg->flags & DRM_MODE_PAGE_FLIP_EVENT) && !events)
        return -EINVAL;
    ret = svga_kms_check(state);
    if (ret || (arg->flags & DRM_MODE_ATOMIC_TEST_ONLY))
        return ret;

    struct svga_vblank_event *pending[SVGA_MAX_OUTPUTS] = {0};
    for (unsigned i = 0; i < svga_num_outputs; ++i) {
        if (!(events & (1u << i)))
            continue;
        ret = svga_vblank_reserve(file, i, DRM_EVENT_FLIP_COMPLETE, arg->user_data, &pending[i]);
        if (ret)
            break;
    }
    if (!ret && (arg->flags & DRM_MODE_ATOMIC_NONBLOCK)) {
        ret = svga_queue_commit(file, state, scanouts, cursors, pending);
        if (!ret)
            return 0;
    } else if (!ret) {
        ret = svga_kms_commit(state, scanouts, cursors);
    }
    for (unsigned i = 0; i < svga_num_outputs; ++i) {
        if (ret)
            svga_vblank_cancel(pending[i]);
        else
            svga_vblank_arm_flip(pending[i], false);
    }
    return ret;
}
