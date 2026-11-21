/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include "render.h"
#include <linux/capability.h>

unsigned svga_num_outputs;
struct drm_vmw_rect svga_layout[SVGA_MAX_OUTPUTS];

static const struct drm_vmw_rect default_layout = {.w = 1280, .h = 800};

void svga_kms_init(void)
{
    svga_num_outputs = svga_screen_type() != SVGA_DISPLAY_LEGACY ||
        (svga.caps & SVGA_CAP_MULTIMON) ? SVGA_MAX_OUTPUTS : 1;
    __builtin_memset(svga_layout, 0, sizeof(svga_layout));
    svga_layout[0] = default_layout;
    if (svga_mode_fits(svga.boot_width, svga.boot_height)) {
        svga_layout[0].w = svga.boot_width;
        svga_layout[0].h = svga.boot_height;
    }
}

bool svga_output_connected(unsigned unit)
{
    /* Older single-display hosts do not implement NUM_DISPLAYS. */
    return svga_layout[unit].w &&
        (svga_num_outputs == 1 || unit < svga_read(SVGA_REG_NUM_DISPLAYS));
}

/* Both requested layout and enabled modes must fit display memory. The
 * layout's origin is (0,0), including any gaps before the first head. */
int svga_layout_check(const struct drm_vmw_rect *rects, unsigned count)
{
    uint32_t right = 0, bottom = 0;
    uint64_t total = 0;
    for (unsigned i = 0; i < count; ++i) {
        const struct drm_vmw_rect *r = &rects[i];
        if (!r->w && !r->h)
            continue;
        int64_t x2 = (int64_t)r->x + r->w;
        int64_t y2 = (int64_t)r->y + r->h;
        if (x2 > INT32_MAX || y2 > INT32_MAX)
            return -ERANGE;
        if (r->x < 0 || r->y < 0 || x2 > svga.max_width || y2 > svga.max_height ||
            !svga_mode_fits(r->w, r->h))
            return -EINVAL;
        if (x2 > right)
            right = x2;
        if (y2 > bottom)
            bottom = y2;
        total += (uint64_t)r->w * r->h * 4;
    }
    uint32_t limit = svga_screen_max_primary();
    bool no_bbox = svga_screen_type() == SVGA_DISPLAY_TARGET &&
                   (svga.caps & SVGA_CAP_NO_BB_RESTRICTION);
    return total > limit || (!no_bbox && (uint64_t)right * bottom * 4 > limit) ? -EINVAL : 0;
}

int svga_update_layout(struct task *task, struct drm_file *file,
                       const struct drm_vmw_update_layout_arg *arg)
{
    bool master = file->is_master && file->master && file->device->master == file->master;
    if (!master && !(task->cap_effective & (UINT64_C(1) << CAP_SYS_ADMIN)))
        return -EACCES;
    unsigned count = arg->num_outputs;
    if (count > SVGA_MAX_OUTPUTS)
        return -E2BIG;
    struct drm_vmw_rect rects[SVGA_MAX_OUTPUTS] = {0};
    if (!count) {
        rects[0] = default_layout;
    } else {
        int ret = svga_import(task, rects, arg->rects, count * sizeof(rects[0]));
        if (ret)
            return ret;
        for (unsigned i = 0; i < count; ++i)
            if (!rects[i].w || !rects[i].h)
                return -EINVAL;
        ret = svga_layout_check(rects, count);
        if (ret)
            return ret;
    }
    __builtin_memcpy(svga_layout, rects, sizeof(svga_layout));
    drm_device_hotplug(file->device);
    return 0;
}
