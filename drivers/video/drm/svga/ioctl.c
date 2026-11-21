/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include "mksstat.h"
#include "render.h"
#include <drm/vmwgfx_drm.h>

/* The preserved upstream UAPI supplies types and command numbers. The
 * shape and permissions of these entries follow vmwgfx_drv.c. */
#define VMW_GET_PARAM DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_GET_PARAM, struct drm_vmw_getparam_arg)
#define VMW_ALLOC_BO DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_ALLOC_BO, union drm_vmw_alloc_bo_arg)
#define VMW_HANDLE_CLOSE DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_HANDLE_CLOSE, struct drm_vmw_handle_close_arg)
#define VMW_UPDATE_LAYOUT SVGA_VMW_IOW(UPDATE_LAYOUT, struct drm_vmw_update_layout_arg)
#define VMW_CURSOR_BYPASS SVGA_VMW_IOW(CURSOR_BYPASS, struct drm_vmw_cursor_bypass_arg)
#define VMW_CLAIM_STREAM DRM_IOR(DRM_COMMAND_BASE + DRM_VMW_CLAIM_STREAM, struct drm_vmw_stream_arg)
#define VMW_CONTROL_STREAM SVGA_VMW_IOW(CONTROL_STREAM, struct drm_vmw_control_stream_arg)
#define VMW_UNREF_STREAM SVGA_VMW_IOW(UNREF_STREAM, struct drm_vmw_stream_arg)
#define VMW_MSG DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_MSG, struct drm_vmw_msg_arg)

static const struct drm_driver_ioctl ioctls[] = {
    {VMW_GET_PARAM, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_ALLOC_BO, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_HANDLE_CLOSE, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_CREATE_CONTEXT, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_UNREF_CONTEXT, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_CREATE_EXTENDED_CONTEXT, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_CREATE_SURFACE, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_UNREF_SURFACE, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_REF_SURFACE, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_GB_SURFACE_CREATE, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_GB_SURFACE_REF, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_GB_SURFACE_CREATE_EXT, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_GB_SURFACE_REF_EXT, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_CREATE_SHADER, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_UNREF_SHADER, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_EXECBUF, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_GET_3D_CAP, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_FENCE_WAIT, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_FENCE_SIGNALED, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_FENCE_UNREF, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_FENCE_EVENT, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_SYNCCPU, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_PRESENT, DRM_PERMISSION_MASTER | DRM_PERMISSION_AUTH},
    {VMW_PRESENT_READBACK, DRM_PERMISSION_MASTER | DRM_PERMISSION_AUTH},
    {VMW_UPDATE_LAYOUT, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_CURSOR_BYPASS, DRM_PERMISSION_MASTER},
    {VMW_CLAIM_STREAM, DRM_PERMISSION_MASTER},
    {VMW_CONTROL_STREAM, DRM_PERMISSION_MASTER},
    {VMW_UNREF_STREAM, DRM_PERMISSION_MASTER},
    {VMW_MSG, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_MKSSTAT_RESET, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_MKSSTAT_ADD, DRM_PERMISSION_RENDER_ALLOW},
    {VMW_MKSSTAT_REMOVE, DRM_PERMISSION_RENDER_ALLOW},
};

static int get_param(struct drm_file *file, struct drm_vmw_getparam_arg *arg)
{
    switch (arg->param) {
    case DRM_VMW_PARAM_DEVICE_ID:
        arg->value = svga.pci.device_id;
        break;
    case DRM_VMW_PARAM_MAX_FB_SIZE:
        arg->value = svga_screen_max_primary();
        break;
    case DRM_VMW_PARAM_SCREEN_TARGET:
        arg->value = svga_screen_type() == SVGA_DISPLAY_TARGET;
        break;
    case DRM_VMW_PARAM_HW_CAPS:
        arg->value = svga_render_supported() ? svga.caps :
            svga.caps & (SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_8BIT_EMULATION);
        break;
    case DRM_VMW_PARAM_FIFO_CAPS:
        arg->value = svga_render_supported() ? svga.fifo.caps :
            svga.fifo.caps & (SVGA_FIFO_CAP_RESERVE | SVGA_FIFO_CAP_FENCE);
        break;
    case DRM_VMW_PARAM_HW_CAPS2:
        arg->value = svga_render_supported() ? svga.caps2 : 0;
        break;
    case DRM_VMW_PARAM_NUM_STREAMS:
        arg->value = svga_overlay_supported();
        break;
    case DRM_VMW_PARAM_NUM_FREE_STREAMS:
        arg->value = svga_overlay_free();
        break;
    default:
        return svga_render_param(file, arg);
    }
    return 0;
}

static int svga_ioctl(struct task *task, struct drm_file *file, uint32_t command,
                      void *data, uint32_t *handle, int *fd, uint32_t *object)
{
    (void)fd;
    if (!svga.ready)
        return -EIO;
    /* The transport cannot accept new work on fault. Existing handles
     * are still owned by the file and confirmed fences retain their status. */
    switch (command) {
    case VMW_FENCE_WAIT:
    case VMW_FENCE_SIGNALED:
    case VMW_FENCE_UNREF:
        if (svga.fifo.fault || svga_render_supported())
            return svga_fence_ioctl(task, file, command, data);
        return -EOPNOTSUPP;
    case VMW_HANDLE_CLOSE:
        return drm_bo_handle_delete(file, ((const struct drm_vmw_handle_close_arg *)data)->handle);
    case VMW_UNREF_CONTEXT:
        return svga_object_unref(file, SVGA_CONTEXT, ((const struct drm_vmw_context_arg *)data)->cid);
    case VMW_UNREF_SURFACE:
        return svga_surface_ioctl(task, file, command, data, handle, object);
    case VMW_UNREF_SHADER:
        return svga_object_unref(file, SVGA_SHADER, ((const struct drm_vmw_shader_arg *)data)->handle);
    case VMW_SYNCCPU:
        return svga_synccpu(task, file, data);
    case VMW_UNREF_STREAM:
        return svga_overlay_unref(file, data);
    case VMW_MSG:
        return svga_host_message(task, data);
    case VMW_MKSSTAT_RESET:
    case VMW_MKSSTAT_ADD:
    case VMW_MKSSTAT_REMOVE:
        return svga_mksstat_ioctl(task, file, command, data);
    default:
        break;
    }
    if (svga.fifo.fault)
        return -EIO;
    switch (command) {
    case VMW_GET_PARAM:
        return get_param(file, data);
    case VMW_ALLOC_BO: {
        union drm_vmw_alloc_bo_arg *arg = data;
        uint64_t offset;
        int ret = drm_bo_create(file, NULL, arg->req.size, handle, &offset);
        if (!ret)
            arg->rep = (struct drm_vmw_bo_rep){
                .map_handle = offset, .handle = *handle, .cur_gmr_id = *handle,
            };
        return ret;
    }
    case VMW_GET_3D_CAP:
        return svga_render_caps(task, file, data);
    case VMW_PRESENT:
        svga_atomic_drain();
        return svga_kms_present_ioctl(task, file, data);
    case VMW_PRESENT_READBACK:
        svga_atomic_drain();
        return svga_kms_readback_ioctl(task, file, data);
    case VMW_UPDATE_LAYOUT:
        svga_atomic_drain();
        return svga_update_layout(task, file, data);
    case VMW_CURSOR_BYPASS:
        svga_atomic_drain();
        return svga_cursor_bypass(data);
    case VMW_CLAIM_STREAM:
        return svga_overlay_claim(file, data);
    case VMW_CONTROL_STREAM:
        svga_atomic_drain();
        return svga_overlay_control(file, data);
    default:
        break;
    }
    if (!svga_render_supported())
        return -EOPNOTSUPP;
    switch (command) {
    case VMW_CREATE_CONTEXT:
        return svga_context_create(file, false, data);
    case VMW_CREATE_EXTENDED_CONTEXT: {
        union drm_vmw_extended_context_arg *arg = data;
        if (arg->req != drm_vmw_context_legacy && arg->req != drm_vmw_context_dx)
            return -EINVAL;
        return svga_context_create(file, arg->req == drm_vmw_context_dx, &arg->rep);
    }
    case VMW_CREATE_SURFACE:
    case VMW_REF_SURFACE:
    case VMW_GB_SURFACE_CREATE:
    case VMW_GB_SURFACE_REF:
    case VMW_GB_SURFACE_CREATE_EXT:
    case VMW_GB_SURFACE_REF_EXT:
        return svga_surface_ioctl(task, file, command, data, handle, object);
    case VMW_CREATE_SHADER:
        return svga_shader_create(file, data);
    case VMW_EXECBUF:
        return svga_execbuf(task, file, data);
    case VMW_FENCE_EVENT:
        return svga_fence_ioctl(task, file, command, data);
    default:
        return -EINVAL;
    }
}

static int dumb_init(struct drm_bo *bo, uint32_t width, uint32_t height,
                     uint32_t bpp, uint32_t pitch)
{
    (void)bo;
    (void)pitch;
    if (!svga.ready || svga.fifo.fault)
        return -EIO;
    return (bpp == 24 || bpp == 32) && svga_mode_fits(width, height) ? 0 : -EINVAL;
}

static int get_cap(struct drm_get_cap *arg)
{
    switch (arg->capability) {
    case DRM_CAP_DUMB_PREFERRED_DEPTH:
        arg->value = 24;
        return 0;
    case DRM_CAP_ADDFB2_MODIFIERS:
    case DRM_CAP_ASYNC_PAGE_FLIP:
        arg->value = 0;
        return 0;
    case DRM_CAP_CRTC_IN_VBLANK_EVENT:
        arg->value = 1;
        return 0;
    case DRM_CAP_CURSOR_WIDTH:
    case DRM_CAP_CURSOR_HEIGHT:
        arg->value = svga_cursor_limit();
        return 0;
    default:
        return -EOPNOTSUPP;
    }
}

static void svga_ioctl_rollback(struct drm_file *file, uint32_t command, const void *data, uint32_t object)
{
    if (command == DRM_IOCTL_MODE_CREATEPROPBLOB) {
        const struct drm_mode_create_blob *arg = data;
        (void)svga_blob_destroy(file, arg->blob_id);
    } else if (command == VMW_CLAIM_STREAM)
        (void)svga_overlay_unref(file, data);
    else if (command == VMW_MKSSTAT_ADD)
        svga_mksstat_rollback(file, ((const struct drm_vmw_mksstat_add_arg *)data)->id);
    else
        svga_render_rollback(file, command, data, object);
}

static void svga_file_release(struct drm_file *file)
{
    svga_mksstat_file_release(file);
    svga_render_file_release(file);
}

static uint32_t service_retirements(uint32_t budget)
{
    uint32_t retired = svga_render_retire(budget);
    svga_console_update();
    return retired;
}

const struct drm_driver svga_driver = {
    .name = "vmwgfx", .description = "VMware SVGA II",
    .major = 2, .minor = 21,
    .start = svga_device_start, .stop = svga_device_stop,
    .render_ready = svga_render_supported, .file_release = svga_file_release,
    .ioctl_rollback = svga_ioctl_rollback,
    .prime_bo = true, .prime_export = svga_prime_export, .prime_import = svga_prime_import,
    .ioctls = ioctls, .ioctl_count = sizeof(ioctls) / sizeof(ioctls[0]), .ioctl = svga_ioctl,
    .dumb_init = dumb_init, .get_cap = get_cap, .universal_planes = true,
    .bo_cpu_access = svga_surface_cpu_access, .atomic_modeset = true, .cursor_hotspot = true,
    .events_read = svga_events_read, .events_pending = svga_events_pending,
    .kms_ioctl = svga_kms_ioctl, .kms_file_release = svga_kms_file_release,
    .kms_remove_fb = svga_kms_remove_fb, .kms_active = svga_kms_active,
    .kms_vt_release = svga_kms_vt_release, .connector_info = svga_connector_info,
    .retire = service_retirements,
};
