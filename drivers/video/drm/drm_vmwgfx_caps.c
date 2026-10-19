#include "drm_internal.h"

#include <drm/vmwgfx_drm.h>
#include <linux/errno.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/heap.h>
#include <reliefnt/svga.h>
#include <reliefnt/usercopy.h>

#include "../svga/svga_protocol.h"

#define DRM_VMW_CAP_RECORD_TYPE 0x100u
#define DRM_VMW_LOCAL_RENDER_TARGETS 4u
#define DRM_VMW_LOCAL_TEXTURES 32u
#define DRM_VMW_LOCAL_CONTEXTS 63u
#define DRM_VMW_LOCAL_SURFACES 255u
#define DRM_VMW_LOCAL_TEXTURE_DIMENSION 16384u
#define DRM_VMW_LOCAL_VOLUME_DIMENSION 256u
#define DRM_VMW_VERTEX_SHADER_VERSION_30 7u
#define DRM_VMW_FRAGMENT_SHADER_VERSION_30 13u
#define DRM_VMW_COMPAT_DEVCAP_MAX 262u
#define DRM_VMW_DEVCAP_MAX_LINE_WIDTH 89u
#define DRM_VMW_DEVCAP_MAX_AA_LINE_WIDTH 90u
#define DRM_VMW_CAP_HEADER_DWORDS 2u
#define DRM_VMW_CAP_TRAILER_DWORDS 1u
#define DRM_VMW_CAP_RECORD_DWORDS \
    (DRM_VMW_CAP_HEADER_DWORDS + 2u * DRM_VMW_COMPAT_DEVCAP_MAX + \
     DRM_VMW_CAP_TRAILER_DWORDS)
#define DRM_VMW_CAP_RECORD_BYTES (DRM_VMW_CAP_RECORD_DWORDS * sizeof(uint32_t))

struct drm_vmw_format_cap {
    uint32_t cap;
    uint32_t format;
};

static const struct drm_vmw_format_cap drm_vmw_format_caps[] = {
    {SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8, SVGA3D_X8R8G8B8},
    {SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8, SVGA3D_A8R8G8B8},
    {SVGA3D_DEVCAP_SURFACEFMT_A2R10G10B10, SVGA3D_A2R10G10B10},
    {SVGA3D_DEVCAP_SURFACEFMT_X1R5G5B5, SVGA3D_X1R5G5B5},
    {SVGA3D_DEVCAP_SURFACEFMT_A1R5G5B5, SVGA3D_A1R5G5B5},
    {SVGA3D_DEVCAP_SURFACEFMT_A4R4G4B4, SVGA3D_A4R4G4B4},
    {SVGA3D_DEVCAP_SURFACEFMT_R5G6B5, SVGA3D_R5G6B5},
    {SVGA3D_DEVCAP_SURFACEFMT_LUMINANCE16, SVGA3D_LUMINANCE16},
    {SVGA3D_DEVCAP_SURFACEFMT_LUMINANCE8_ALPHA8, SVGA3D_LUMINANCE8_ALPHA8},
    {SVGA3D_DEVCAP_SURFACEFMT_ALPHA8, SVGA3D_ALPHA8},
    {SVGA3D_DEVCAP_SURFACEFMT_LUMINANCE8, SVGA3D_LUMINANCE8},
    {SVGA3D_DEVCAP_SURFACEFMT_Z_D16, SVGA3D_Z_D16},
    {SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8, SVGA3D_Z_D24S8},
    {SVGA3D_DEVCAP_SURFACEFMT_Z_D24X8, SVGA3D_Z_D24X8},
    {SVGA3D_DEVCAP_SURFACEFMT_DXT1, SVGA3D_DXT1},
    {SVGA3D_DEVCAP_SURFACEFMT_DXT2, SVGA3D_DXT2},
    {SVGA3D_DEVCAP_SURFACEFMT_DXT3, SVGA3D_DXT3},
    {SVGA3D_DEVCAP_SURFACEFMT_DXT4, SVGA3D_DXT4},
    {SVGA3D_DEVCAP_SURFACEFMT_DXT5, SVGA3D_DXT5},
    {SVGA3D_DEVCAP_SURFACEFMT_BUMPX8L8V8U8, SVGA3D_BUMPX8L8V8U8},
    {SVGA3D_DEVCAP_SURFACEFMT_A2W10V10U10, SVGA3D_A2W10V10U10},
    {SVGA3D_DEVCAP_SURFACEFMT_BUMPU8V8, SVGA3D_BUMPU8V8},
    {SVGA3D_DEVCAP_SURFACEFMT_Q8W8V8U8, SVGA3D_Q8W8V8U8},
    {SVGA3D_DEVCAP_SURFACEFMT_CxV8U8, SVGA3D_CxV8U8},
    {SVGA3D_DEVCAP_SURFACEFMT_R_S10E5, SVGA3D_R_S10E5},
    {SVGA3D_DEVCAP_SURFACEFMT_R_S23E8, SVGA3D_R_S23E8},
    {SVGA3D_DEVCAP_SURFACEFMT_RG_S10E5, SVGA3D_RG_S10E5},
    {SVGA3D_DEVCAP_SURFACEFMT_RG_S23E8, SVGA3D_RG_S23E8},
    {SVGA3D_DEVCAP_SURFACEFMT_ARGB_S10E5, SVGA3D_ARGB_S10E5},
    {SVGA3D_DEVCAP_SURFACEFMT_ARGB_S23E8, SVGA3D_ARGB_S23E8},
    {SVGA3D_DEVCAP_SURFACEFMT_V16U16, SVGA3D_V16U16},
    {SVGA3D_DEVCAP_SURFACEFMT_G16R16, SVGA3D_G16R16},
    {SVGA3D_DEVCAP_SURFACEFMT_A16B16G16R16, SVGA3D_A16B16G16R16},
    {SVGA3D_DEVCAP_SURFACEFMT_UYVY, SVGA3D_UYVY},
    {SVGA3D_DEVCAP_SURFACEFMT_YUY2, SVGA3D_YUY2},
    {SVGA3D_DEVCAP_SURFACEFMT_NV12, SVGA3D_NV12},
    {SVGA3D_DEVCAP_SURFACEFMT_AYUV, SVGA3D_AYUV},
    {SVGA3D_DEVCAP_SURFACEFMT_Z_DF16, SVGA3D_Z_DF16},
    {SVGA3D_DEVCAP_SURFACEFMT_Z_DF24, SVGA3D_Z_DF24},
    {SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8_INT, SVGA3D_Z_D24S8_INT},
    {SVGA3D_DEVCAP_SURFACEFMT_BC4_UNORM, SVGA3D_BC4_UNORM},
    {SVGA3D_DEVCAP_SURFACEFMT_BC5_UNORM, SVGA3D_BC5_UNORM},
};

static bool drm_vmw_svga_ready(struct svga_info *info)
{
    uint32_t cap_3d = 0;
    svga_get_info(info);
    return info->available && info->fifo_ready &&
           svga3d_capability(SVGA3D_DEVCAP_3D, &cap_3d) == 0 && cap_3d != 0;
}

/* Mesa's VGPU9 screen rejects devices below shader model 3. Advertise its
 * capability table only when both shader stages and the transactional host
 * path are usable; a raw host 3D bit alone is not enough. */
bool drm_vmwgfx_vgpu9_ready(struct svga_info *info)
{
    uint32_t vertex_version = 0, fragment_version = 0;
    if (!drm_vmw_svga_ready(info) ||
        svga3d_capability(SVGA3D_DEVCAP_VERTEX_SHADER_VERSION,
                          &vertex_version) != 0 ||
        svga3d_capability(SVGA3D_DEVCAP_FRAGMENT_SHADER_VERSION,
                          &fragment_version) != 0)
        return false;
    /* These are protocol enum values, not ordinal major/minor versions:
     * VSVERSION_30 is 7 and PSVERSION_30 is 13 in the SVGA3D ABI. */
    return vertex_version >= DRM_VMW_VERTEX_SHADER_VERSION_30 &&
           fragment_version >= DRM_VMW_FRAGMENT_SHADER_VERSION_30;
}

static uint32_t drm_vmw_filter_float(uint32_t value, uint32_t maximum)
{
    uint32_t exponent = value & 0x7f800000u;
    if ((value & 0x80000000u) || !value || exponent == 0x7f800000u)
        return 0;
    return value > maximum ? maximum : value;
}

static bool drm_vmw_get_format_for_cap(uint32_t cap, uint32_t *format)
{
    for (uint32_t i = 0; i < sizeof(drm_vmw_format_caps) /
                              sizeof(drm_vmw_format_caps[0]); ++i) {
        if (drm_vmw_format_caps[i].cap != cap) continue;
        *format = drm_vmw_format_caps[i].format;
        return true;
    }
    return false;
}

static uint32_t drm_vmw_filtered_cap(uint32_t index, bool ready)
{
    uint32_t value = 0;
    if (!ready || svga3d_capability(index, &value) != 0) return 0;

    switch (index) {
    case SVGA3D_DEVCAP_3D:
        return value ? 1u : 0u;
    case SVGA3D_DEVCAP_VERTEX_SHADER_VERSION:
        return value > DRM_VMW_VERTEX_SHADER_VERSION_30
            ? DRM_VMW_VERTEX_SHADER_VERSION_30 : value;
    case SVGA3D_DEVCAP_FRAGMENT_SHADER_VERSION:
        return value > DRM_VMW_FRAGMENT_SHADER_VERSION_30
            ? DRM_VMW_FRAGMENT_SHADER_VERSION_30 : value;
    case SVGA3D_DEVCAP_VERTEX_SHADER:
        return value && drm_vmw_filtered_cap(
            SVGA3D_DEVCAP_VERTEX_SHADER_VERSION, ready) >=
                DRM_VMW_VERTEX_SHADER_VERSION_30;
    case SVGA3D_DEVCAP_FRAGMENT_SHADER:
        return value && drm_vmw_filtered_cap(
            SVGA3D_DEVCAP_FRAGMENT_SHADER_VERSION, ready) >=
                DRM_VMW_FRAGMENT_SHADER_VERSION_30;
    case SVGA3D_DEVCAP_MAX_RENDER_TARGETS:
    case SVGA3D_DEVCAP_MAX_SIMULTANEOUS_RENDER_TARGETS:
        return value > DRM_VMW_LOCAL_RENDER_TARGETS
            ? DRM_VMW_LOCAL_RENDER_TARGETS : value;
    case SVGA3D_DEVCAP_S23E8_TEXTURES:
    case SVGA3D_DEVCAP_S10E5_TEXTURES:
        /* The current descriptor and DMA paths do not implement float textures. */
        return 0;
    case SVGA3D_DEVCAP_MAX_POINT_SIZE:
        return drm_vmw_filter_float(value, 0x42800000u); /* 64.0f bits */
    case DRM_VMW_DEVCAP_MAX_LINE_WIDTH:
    case DRM_VMW_DEVCAP_MAX_AA_LINE_WIDTH:
        return drm_vmw_filter_float(value, 0x41200000u); /* 10.0f bits */
    case SVGA3D_DEVCAP_MAX_SHADER_TEXTURES:
    case SVGA3D_DEVCAP_MAX_VERTEX_SHADER_TEXTURES:
    case SVGA3D_DEVCAP_MAX_TEXTURES:
        return value > DRM_VMW_LOCAL_TEXTURES ? DRM_VMW_LOCAL_TEXTURES : value;
    case SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH:
    case SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT:
        return value > DRM_VMW_LOCAL_TEXTURE_DIMENSION
            ? DRM_VMW_LOCAL_TEXTURE_DIMENSION : value;
    case SVGA3D_DEVCAP_MAX_VOLUME_EXTENT:
        return value > DRM_VMW_LOCAL_VOLUME_DIMENSION
            ? DRM_VMW_LOCAL_VOLUME_DIMENSION : value;
    case SVGA3D_DEVCAP_MAX_TEXTURE_ANISOTROPY:
        return value > 16u ? 16u : value;
    case SVGA3D_DEVCAP_MAX_CONTEXT_IDS:
        return value > DRM_VMW_LOCAL_CONTEXTS ? DRM_VMW_LOCAL_CONTEXTS : value;
    case SVGA3D_DEVCAP_MAX_SURFACE_IDS:
        return value > DRM_VMW_LOCAL_SURFACES ? DRM_VMW_LOCAL_SURFACES : value;
    case SVGA3D_DEVCAP_QUERY_TYPES:
        /* The accepted backend has only the legacy occlusion-query path. */
        return value & (1u << SVGA3D_QUERYTYPE_OCCLUSION);
    default:
        break;
    }

    uint32_t format;
    if (drm_vmw_get_format_for_cap(index, &format)) {
        uint32_t operations = 0;
        if (svga3d_format_operations(format, &operations) != 0) return 0;
        return operations & value;
    }

    /* Hole 62 and caps without a matching local contract remain zero. Do not
     * forward unknown host bits as operations the guest may rely on. */
    return 0;
}

static uint32_t drm_vmw_build_compat_caps(uint32_t *record)
{
    struct svga_info info;
    bool ready = drm_vmwgfx_vgpu9_ready(&info);
    if (!ready) return 0;
    record[0] = DRM_VMW_CAP_HEADER_DWORDS + 2u * DRM_VMW_COMPAT_DEVCAP_MAX;
    record[1] = DRM_VMW_CAP_RECORD_TYPE;
    for (uint32_t i = 0; i < DRM_VMW_COMPAT_DEVCAP_MAX; ++i) {
        record[DRM_VMW_CAP_HEADER_DWORDS + i * 2u] = i;
        record[DRM_VMW_CAP_HEADER_DWORDS + i * 2u + 1u] =
            drm_vmw_filtered_cap(i, ready);
    }
    record[record[0]] = 0;
    return DRM_VMW_CAP_RECORD_BYTES;
}

static int drm_vmw_get_param(struct drm_file *file,
                             struct drm_vmw_getparam_arg *arg)
{
    struct svga_info info;
    struct pci_device pci;
    (void)file;
    svga_get_info(&info);
    arg->value = 0;
    switch (arg->param) {
    case DRM_VMW_PARAM_NUM_STREAMS:
    case DRM_VMW_PARAM_NUM_FREE_STREAMS:
    case DRM_VMW_PARAM_HW_CAPS:
    case DRM_VMW_PARAM_HW_CAPS2:
    case DRM_VMW_PARAM_MAX_MOB_MEMORY:
    case DRM_VMW_PARAM_MAX_MOB_SIZE:
    case DRM_VMW_PARAM_SCREEN_TARGET:
    case DRM_VMW_PARAM_DX:
    case DRM_VMW_PARAM_SM4_1:
    case DRM_VMW_PARAM_SM5:
    case DRM_VMW_PARAM_GL43:
        return (arg->param == DRM_VMW_PARAM_HW_CAPS ||
                arg->param == DRM_VMW_PARAM_HW_CAPS2)
            ? -LINUX_EINVAL : 0;
    case DRM_VMW_PARAM_FIFO_CAPS:
        arg->value = info.fifo_caps;
        return 0;
    case DRM_VMW_PARAM_MAX_FB_SIZE: {
        const struct framebuffer *fb = framebuffer_get();
        if (fb) arg->value = fb->max_bytes;
        return 0;
    }
    case DRM_VMW_PARAM_FIFO_HW_VERSION:
        arg->value = info.host_version;
        return 0;
    case DRM_VMW_PARAM_MAX_SURF_MEMORY:
        arg->value = info.compat_surface_limit_bytes;
        return 0;
    case DRM_VMW_PARAM_3D_CAPS_SIZE:
        arg->value = drm_vmwgfx_vgpu9_ready(&info) ? DRM_VMW_CAP_RECORD_BYTES : 0;
        return 0;
    case DRM_VMW_PARAM_3D:
        arg->value = drm_vmwgfx_vgpu9_ready(&info) ? 1u : 0u;
        return 0;
    case DRM_VMW_PARAM_DEVICE_ID:
        if (drm_device_pci_identity(&pci) < 0) return -LINUX_ENODEV;
        arg->value = pci.device_id;
        return 0;
    default:
        return -LINUX_EINVAL;
    }
}

static int drm_vmw_get_3d_cap(struct task *task,
                              struct drm_vmw_get_3d_cap_arg *arg)
{
    uint32_t record[DRM_VMW_CAP_RECORD_DWORDS] = {0};
    if (arg->pad64 || !arg->max_size) return -LINUX_EINVAL;
    uint32_t total = drm_vmw_build_compat_caps(record);
    if (!total) return -LINUX_ENOMEM;
    if (arg->max_size < DRM_VMW_CAP_HEADER_DWORDS * sizeof(uint32_t))
        return -LINUX_EINVAL;

    uint32_t bytes = arg->max_size < total ? arg->max_size : total;
    uint32_t pair_count = (bytes - DRM_VMW_CAP_HEADER_DWORDS * sizeof(uint32_t)) /
                          (2u * sizeof(uint32_t));
    if (pair_count > DRM_VMW_COMPAT_DEVCAP_MAX)
        pair_count = DRM_VMW_COMPAT_DEVCAP_MAX;
    record[0] = DRM_VMW_CAP_HEADER_DWORDS + 2u * pair_count;
    record[1] = DRM_VMW_CAP_RECORD_TYPE;
    record[record[0]] = 0;
    if (user_copy_to_task(task, arg->buffer, record, bytes) < 0)
        return -LINUX_EFAULT;
    return 0;
}

int drm_vmwgfx_caps_ioctl(struct task *task, struct drm_file *file,
                          uint32_t command, void *data)
{
    if (!file || !data) return -LINUX_EINVAL;
    switch (_IOC_NR(command) - DRM_COMMAND_BASE) {
    case DRM_VMW_GET_PARAM:
        return drm_vmw_get_param(file, data);
    case DRM_VMW_GET_3D_CAP:
        return drm_vmw_get_3d_cap(task, data);
    default:
        return -LINUX_EINVAL;
    }
}
