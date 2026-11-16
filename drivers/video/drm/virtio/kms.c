/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include <reliefnt/usercopy.h>

struct vg_output vg_outputs[VIRTIO_GPU_MAX_SCANOUTS];

/** @brief Test whether any hardware output is owned by DRM/KMS.
 * @return True for at least one enabled output; execution-owned snapshot.
 */
bool vg_kms_active(void)
{
    for(unsigned i=0;i<vg.scanout_count;++i)if(vg_outputs[i].enabled)return true;
    return false;
}

/** @brief Disable one scanout, fencing before releasing its cursor backing.
 * @param i Hardware output. @return Zero or errno, retaining state on failure.
 */
int vg_output_disable(unsigned i)
{
    if(i>=vg.scanout_count)return -EINVAL;
    struct vg_output *out=&vg_outputs[i];
    int rc=0;
    if(vg.fault && vg.transport.live && vg_device_fault(-EIO))return -EIO;
    if(out->cursor && vg.transport.live)rc=vg_cursor_disable(i);
    if(!rc && out->enabled && vg.transport.live)rc=vg_scanout(i,0,(struct virtio_gpu_rect){0});
    if(rc && vg.transport.live && vg_device_fault(rc))return rc;
    out->enabled=false;
    if(out->cursor){drm_bo_put(out->cursor);out->cursor=NULL;}
    return 0;
}

/** @brief Disable all user outputs and restore a retained VirtIO text scanout.
 * Task/execution context; failed-reset resources remain pinned in their owners.
 */
void vg_kms_vt_release(void)
{
    for(unsigned i=0;i<vg.scanout_count;++i) {
        if(vg_output_disable(i))continue;
        vg_outputs[i].fb=NULL;vg_outputs[i].owner=NULL;
    }
    (void)vg_console_update(true);
}

/** @brief Release a closing file's modes, framebuffer objects and unread events.
 * @param file Closing OFD. Task context, hardware drains before object destruction.
 */
void vg_kms_file_release(struct drm_file *file)
{
    for(unsigned i=0;i<vg.scanout_count;++i) {
        if(vg_outputs[i].owner==file) {
            if(!vg_output_disable(i)){vg_outputs[i].owner=NULL;vg_outputs[i].fb=NULL;}
        }
    }
    while(file->virtio_fbs) {
        struct vg_kms_fb *fb=file->virtio_fbs;vg_kms_remove_fb(file,fb->id);
        if(file->virtio_fbs==fb)break; /* preserve live hardware on failed reset */
    }
    vg_events_close(file);
}

/** @brief Upload, bind and flush a validated framebuffer rectangle in protocol order.
 * @param file Owner. @param i Hardware output. @param fb Retained local FB.
 * @param mode Valid progressive mode. @param x Source left. @param y Source top.
 * @return Zero or errno, publishing software state only after the host fence.
 */
static int program(struct drm_file *file,unsigned i,struct vg_kms_fb *fb,
                     const struct drm_mode_modeinfo *mode,uint32_t x,uint32_t y)
{
    if(!fb || !mode->hdisplay || !mode->vdisplay || x>fb->width || y>fb->height ||
        mode->hdisplay>fb->width-x || mode->vdisplay>fb->height-y ||
        (mode->flags&(DRM_MODE_FLAG_INTERLACE|DRM_MODE_FLAG_DBLSCAN)))return -EINVAL;
    struct virtio_gpu_rect rect={x,y,mode->hdisplay,mode->vdisplay};
    int rc=vg_resource_upload(fb->bo,rect);if(rc)return rc;
    rc=vg_scanout(i,drm_bo_virtio(fb->bo)->id,rect);if(rc)return rc;
    /* SET_SCANOUT can already expose this backing even if FLUSH later fails. */
    struct vg_output *out=&vg_outputs[i];
    out->fb=fb;out->owner=file;out->mode=*mode;out->x=x;out->y=y;out->enabled=true;
    struct virtio_gpu_resource_flush flush={.hdr.type=VIRTIO_GPU_CMD_RESOURCE_FLUSH,
        .r=rect,.resource_id=drm_bo_virtio(fb->bo)->id};
    return vg_command(&flush,sizeof(flush),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
}

/** @brief Import connector assignment before changing one legacy CRTC.
 * @param task Caller. @param file Master. @param arg Copied mode request.
 * @return Zero, errno or prerequisite retry, with no side effects before import.
 */
static int setcrtc(struct task *task,struct drm_file *file,struct drm_mode_crtc *arg)
{
    unsigned i=arg->crtc_id-VG_CRTC;if(i>=vg.scanout_count)return -ENOENT;
    if(!arg->mode_valid) {
        int rc=vg_output_disable(i);if(!rc){vg_outputs[i].fb=NULL;vg_outputs[i].owner=NULL;}return rc;
    }
    if(arg->mode_valid!=1 || arg->count_connectors!=1)return -EINVAL;
    uint32_t connector;int rc;
#ifdef CONFIG_SWAP
    rc=drm_user_import(task,&connector,arg->set_connectors_ptr,4);
#else
    rc=user_copy_from_task(task,&connector,arg->set_connectors_ptr,4)<0 ? -EFAULT : 0;
#endif
    if(rc)return rc;
    if(connector!=VG_CONNECTOR+i || !vg.display.pmodes[i].enabled)return -EINVAL;
    struct vg_kms_fb *fb=vg_fb_find(file,arg->fb_id);if(!fb)return -ENOENT;
    return program(file,i,fb,&arg->mode,arg->x,arg->y);
}

/** @brief Flip a primary resource and emit an event only after host completion.
 * @param file Master. @param arg Copied flip request. @return Zero or errno.
 * Virtual vblank completion uses the actual GPU fence, never a fabricated IRQ.
 */
static int pageflip(struct drm_file *file,struct drm_mode_crtc_page_flip *arg)
{
    unsigned i=arg->crtc_id-VG_CRTC;if(i>=vg.scanout_count)return -ENOENT;
    if(arg->flags&~DRM_MODE_PAGE_FLIP_EVENT || arg->reserved)return -EINVAL;
    struct vg_output *out=&vg_outputs[i];
    if(!out->enabled || out->owner!=file)return -EINVAL;
    struct vg_kms_fb *fb=vg_fb_find(file,arg->fb_id);if(!fb)return -ENOENT;
    struct vg_event *event=NULL;
    if(arg->flags&DRM_MODE_PAGE_FLIP_EVENT) {
        if(file->virtio_event_bytes>4096-sizeof(struct drm_event_vblank))return -EBUSY;
        event=drm_async_alloc(sizeof(*event));if(!event)return -ENOMEM;
    }
    int rc=program(file,i,fb,&out->mode,out->x,out->y);
    if(!rc && event)vg_event_publish(file,event,i,arg->user_data);
    else if(event)drm_async_free(event);
    return rc;
}

/** @brief Apply the standard DPMS property while retaining the selected FB.
 * @param file Master. @param object Connector ID. @param property Property ID.
 * @param value DPMS enum. @return Zero or errno; immutable properties rejected.
 */
static int setproperty(struct drm_file *file,uint32_t object,uint32_t property,uint64_t value)
{
    unsigned i=object-VG_CONNECTOR;if(i>=vg.scanout_count)return -ENOENT;
    if(property!=VG_PROP_DPMS || value>DRM_MODE_DPMS_OFF)return -EINVAL;
    struct vg_output *out=&vg_outputs[i];
    if(value!=DRM_MODE_DPMS_ON)return vg_output_disable(i);
    if(!out->fb)return 0;
    return program(file,i,out->fb,&out->mode,out->x,out->y);
}

/** @brief Dispatch validated legacy KMS plus primary-plane queries/updates.
 * @param task Caller. @param file Authorized OFD. @param command Canonical ioctl.
 * @param data Copied argument. @param created_fb Rollback output.
 * @return Zero, negative errno or prerequisite retry before state changes.
 */
int vg_kms_ioctl(struct task *task,struct drm_file *file,uint32_t command,void *data,uint32_t *created_fb)
{
    if(!vg.ready || vg.fault)return -EIO;
    int rc=vg_kms_query(task,file,command,data);if(rc!=-VG_IOCTL_UNOWNED)return rc;
    rc=vg_fb_ioctl(task,file,command,data,created_fb);if(rc!=-VG_IOCTL_UNOWNED)return rc;
    switch(command) {
    case DRM_IOCTL_MODE_SETCRTC:return setcrtc(task,file,data);
    case DRM_IOCTL_MODE_PAGE_FLIP:return pageflip(file,data);
    case DRM_IOCTL_MODE_CURSOR:case DRM_IOCTL_MODE_CURSOR2:return vg_cursor_ioctl(file,command,data);
    case DRM_IOCTL_MODE_SETPROPERTY: {
        struct drm_mode_connector_set_property *arg=data;return setproperty(file,arg->connector_id,arg->prop_id,arg->value);
    }
    case DRM_IOCTL_MODE_OBJ_SETPROPERTY: {
        struct drm_mode_obj_set_property *arg=data;
        if(arg->obj_type!=DRM_MODE_OBJECT_CONNECTOR)return -EINVAL;
        return setproperty(file,arg->obj_id,arg->prop_id,arg->value);
    }
    case DRM_IOCTL_MODE_SETPLANE: {
        struct drm_mode_set_plane *arg=data;unsigned i=arg->plane_id-VG_PLANE;
        if(i>=vg.scanout_count || arg->crtc_id!=VG_CRTC+i)return -ENOENT;
        if(!arg->fb_id)return vg_output_disable(i);
        if(arg->flags || arg->crtc_x || arg->crtc_y || (arg->src_x&65535) || (arg->src_y&65535) ||
            arg->src_w!=(uint64_t)arg->crtc_w*65536 || arg->src_h!=(uint64_t)arg->crtc_h*65536)return -EINVAL;
        struct drm_mode_modeinfo mode=vg_outputs[i].mode;
        if(mode.hdisplay!=arg->crtc_w || mode.vdisplay!=arg->crtc_h)return -EINVAL;
        struct vg_kms_fb *fb=vg_fb_find(file,arg->fb_id);if(!fb)return -ENOENT;
        return program(file,i,fb,&mode,arg->src_x>>16,arg->src_y>>16);
    }
    default:return -EOPNOTSUPP;
    }
}
