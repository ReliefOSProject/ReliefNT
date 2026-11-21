/* SPDX-License-Identifier: Apache-2.0 */
#include "render.h"
#include "kms.h"
#include <reliefnt/heap.h>

/* Probing and the two devcap layouts follow vmwgfx_cmd.c, vmwgfx_ioctl.c,
 * vmwgfx_drv.c and vmwgfx_devcaps.c from Linux. */
static struct {
    uint32_t devcap[SVGA3D_DEVCAP_MAX];
    uint32_t hw_version, surface_memory;
    bool supported, gb;
    unsigned shader_model;
} render_caps;

void svga_render_init(void)
{
    render_caps = (__typeof__(render_caps)){0};
    if (!(svga.caps & SVGA_CAP_3D))
        return;
    render_caps.surface_memory = svga_read(SVGA_REG_MEMORY_SIZE);
    if (svga.caps & SVGA_CAP_GBOBJECTS) {
        if (!svga_dma_supported(SVGA_REGION_MOB))
            return;
        for (unsigned i = 0; i < SVGA3D_DEVCAP_MAX; ++i) {
            svga_write(SVGA_REG_DEV_CAP, i);
            render_caps.devcap[i] = svga_read(SVGA_REG_DEV_CAP);
        }
        if (!render_caps.devcap[SVGA3D_DEVCAP_3D])
            return;
        render_caps.gb = true;
        render_caps.supported = true;
        render_caps.hw_version = SVGA3D_HWVERSION_WS8_B1;
        if (svga_submit_has_context() && (svga.caps & SVGA_CAP_DX) &&
            render_caps.devcap[SVGA3D_DEVCAP_DXCONTEXT]) {
            render_caps.shader_model = 40;
            if ((svga.caps2 & SVGA_CAP2_DX2) && render_caps.devcap[SVGA3D_DEVCAP_SM41]) {
                render_caps.shader_model = 41;
                if ((svga.caps2 & SVGA_CAP2_DX3) && render_caps.devcap[SVGA3D_DEVCAP_SM5])
                    render_caps.shader_model = render_caps.devcap[SVGA3D_DEVCAP_GL43] ? 51 : 50;
            }
        }
        return;
    }
    unsigned reg = svga.fifo.caps & SVGA_FIFO_CAP_3D_HWVERSION_REVISED ?
        SVGA_FIFO_3D_HWVERSION_REVISED : SVGA_FIFO_3D_HWVERSION;
    (void)svga_fifo_read_reg(&svga.fifo, reg, &render_caps.hw_version);
    render_caps.supported = render_caps.hw_version >= SVGA3D_HWVERSION_WS8_B1 &&
        svga_screen_type() != SVGA_DISPLAY_LEGACY && svga_dma_supported(SVGA_REGION_GMR);
}

bool svga_render_supported(void)
{
    return render_caps.supported && !svga.fifo.fault;
}

bool svga_render_gb(void)
{
    return render_caps.gb;
}

unsigned svga_render_shader_model(void)
{
    return render_caps.shader_model;
}

uint32_t svga_render_max_surface_size(void)
{
    uint32_t limit = svga_render_gb() ? svga_dma_max_mob_size() : render_caps.surface_memory;
    return limit && limit < SVGA3D_HB_MAX_SURFACE_SIZE ? limit : SVGA3D_HB_MAX_SURFACE_SIZE;
}

uint32_t svga_render_max_texture_dim(unsigned axis)
{
    static const unsigned selectors[] = {SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH,
        SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT, SVGA3D_DEVCAP_MAX_VOLUME_EXTENT};
    return render_caps.gb ? render_caps.devcap[selectors[axis]] : 0;
}

static uint32_t caps_size(bool gb_aware)
{
    if (!svga_render_supported())
        return 0;
    if (render_caps.gb)
        return gb_aware ? sizeof(render_caps.devcap) : sizeof(SVGA3dFifoCapsRecordHeader) +
            sizeof(SVGA3dFifoCapPair) * SVGA3D_DEVCAP_MAX + sizeof(uint32_t);
    uint32_t last = svga.fifo.min / sizeof(uint32_t);
    if (last > SVGA_FIFO_3D_CAPS_LAST + 1)
        last = SVGA_FIFO_3D_CAPS_LAST + 1;
    return last > SVGA_FIFO_3D_CAPS ? (last - SVGA_FIFO_3D_CAPS) * sizeof(uint32_t) : 0;
}

int svga_render_param(struct drm_file *file, struct drm_vmw_getparam_arg *arg)
{
    bool gb_aware = file->svga_render && file->svga_render->gb_aware;
    unsigned shader_model = svga_render_shader_model();
    switch (arg->param) {
    case DRM_VMW_PARAM_3D:
        arg->value = svga_render_supported();
        break;
    case DRM_VMW_PARAM_FIFO_HW_VERSION:
        arg->value = render_caps.hw_version;
        break;
    case DRM_VMW_PARAM_MAX_SURF_MEMORY:
        arg->value = render_caps.gb && !gb_aware ? svga_dma_mob_bytes() / 2 : render_caps.surface_memory;
        break;
    case DRM_VMW_PARAM_3D_CAPS_SIZE:
        arg->value = caps_size(gb_aware);
        break;
    case DRM_VMW_PARAM_MAX_MOB_MEMORY: {
        struct svga_render_file *state = svga_render_file_get(file);
        if (!state)
            return -ENOMEM;
        state->gb_aware = true;
        arg->value = render_caps.gb ? svga_dma_mob_bytes() : 0;
        break;
    }
    case DRM_VMW_PARAM_MAX_MOB_SIZE:
        arg->value = render_caps.gb ? svga_dma_max_mob_size() : 0;
        break;
    case DRM_VMW_PARAM_DX:
        arg->value = shader_model >= 40;
        break;
    case DRM_VMW_PARAM_SM4_1:
        arg->value = shader_model >= 41;
        break;
    case DRM_VMW_PARAM_SM5:
        arg->value = shader_model >= 50;
        break;
    case DRM_VMW_PARAM_GL43:
        arg->value = shader_model >= 51;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

int svga_render_caps(struct task *task, struct drm_file *file, const struct drm_vmw_get_3d_cap_arg *arg)
{
    bool gb_aware = file->svga_render && file->svga_render->gb_aware;
    uint32_t size = caps_size(gb_aware);
    if (arg->pad64 || !arg->max_size)
        return -EINVAL;
    if (!size)
        return -EOPNOTSUPP;
    if (size > arg->max_size)
        size = arg->max_size;
    int ret = svga_prepare_output(task, arg->buffer, size);
    if (ret)
        return ret;
    if (gb_aware && render_caps.gb)
        return user_copy_to_task(task, arg->buffer, render_caps.devcap, size) < 0 ? -EFAULT : 0;
    uint32_t *data = kernel_malloc((size + 3u) & ~3u);
    if (!data)
        return -ENOMEM;
    __builtin_memset(data, 0, (size + 3u) & ~3u);
    if (render_caps.gb) {
        if (size < sizeof(SVGA3dFifoCapsRecordHeader)) {
            kernel_free(data);
            return -EINVAL;
        }
        unsigned pairs = (size - sizeof(SVGA3dFifoCapsRecordHeader)) / sizeof(SVGA3dFifoCapPair);
        data[0] = 2 + 2 * pairs;
        data[1] = SVGA3D_FIFO_CAPS_RECORD_DEVCAPS;
        for (unsigned i = 0; i < pairs; ++i) {
            data[2 + 2 * i] = i;
            data[3 + 2 * i] = i == SVGA3D_DEVCAP_DEAD5 ? 0 : render_caps.devcap[i];
        }
    } else {
        for (unsigned i = 0; i < (size + 3u) / 4u; ++i)
            (void)svga_fifo_read_reg(&svga.fifo, SVGA_FIFO_3D_CAPS + i, &data[i]);
    }
    ret = user_copy_to_task(task, arg->buffer, data, size) < 0 ? -EFAULT : 0;
    kernel_free(data);
    return ret;
}
