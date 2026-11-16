/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include <reliefnt/heap.h>
#include <reliefnt/usercopy.h>

static uint32_t next_fb=0x10000;

/** @brief Resolve a framebuffer only through its creating DRM file description.
 * @param file Authorizing OFD. @param id Object ID. @return Borrowed object or NULL.
 */
struct vg_kms_fb *vg_fb_find(struct drm_file *file,uint32_t id)
{
    for(struct vg_kms_fb *fb=file->virtio_fbs;fb;fb=fb->next)if(fb->id==id)return fb;
    return NULL;
}

/** @brief Validate a linear framebuffer and retain its resource independently.
 * @param file Owner. @param width Pixels. @param height Pixels. @param pitch Byte
 * stride. @param format Fourcc. @param handle File-local GEM ID. @param id Output.
 * @return Zero or errno. No scanout is changed before full validation succeeds.
 */
static int addfb(struct drm_file *file,uint32_t width,uint32_t height,uint32_t pitch,
                   uint32_t format,uint32_t handle,uint32_t *id)
{
    if(!width || !height || width>VG_MAX_WIDTH || height>VG_MAX_HEIGHT ||
        (format!=VG_XRGB8888 && format!=VG_ARGB8888) || pitch<(uint64_t)width*4 || (pitch&3))return -EINVAL;
    struct drm_bo *bo=drm_bo_lookup_handle_get(file,handle);
    struct vg_resource *r=drm_bo_virtio(bo);
    if(!r){drm_bo_put(bo);return -ENOENT;}
    if(width>r->width || height>r->height || (!r->virgl && pitch!=(uint64_t)r->width*4) ||
        (r->virgl && r->format!=VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM &&
         r->format!=VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM) ||
        (!r->virgl && (uint64_t)pitch*height>drm_bo_size(bo))) {
        drm_bo_put(bo);return -EINVAL;
    }
    unsigned count=0;for(struct vg_kms_fb *fb=file->virtio_fbs;fb;fb=fb->next)++count;
    if(count>=VG_MAX_RESOURCES || next_fb==UINT32_MAX){drm_bo_put(bo);return -ENOSPC;}
    struct vg_kms_fb *fb=drm_async_alloc(sizeof(*fb));
    if(!fb){drm_bo_put(bo);return -ENOMEM;}
    *fb=(struct vg_kms_fb){.next=file->virtio_fbs,.bo=bo,.id=next_fb++,
        .width=width,.height=height,.pitch=pitch,.format=format};
    file->virtio_fbs=fb;*id=fb->id;return 0;
}

/** @brief Remove a framebuffer after disabling every hardware output using it.
 * @param file Creating OFD. @param id Object ID; missing objects are ignored.
 * Task context; failed hardware reset keeps backing pinned in the output.
 */
void vg_kms_remove_fb(struct drm_file *file,uint32_t id)
{
    struct vg_kms_fb **link=&file->virtio_fbs;
    while(*link && (*link)->id!=id)link=&(*link)->next;
    if(!*link)return;
    struct vg_kms_fb *fb=*link;
    for(unsigned i=0;i<vg.scanout_count;++i) {
        if(vg_outputs[i].fb==fb) {
            if(vg_output_disable(i))return;
            vg_outputs[i].fb=NULL;vg_outputs[i].owner=NULL;
        }
    }
    *link=fb->next;drm_bo_put(fb->bo);drm_async_free(fb);
}

/** @brief Snapshot/validate all damage rectangles before the first GPU operation.
 * @param task Caller. @param file Owner. @param arg Copied damage descriptor.
 * @return Zero, errno or prerequisite retry; aliases and swapped input are snapshotted.
 */
static int dirty(struct task *task,struct drm_file *file,struct drm_mode_fb_dirty_cmd *arg)
{
    struct vg_kms_fb *fb=vg_fb_find(file,arg->fb_id);if(!fb)return -ENOENT;
    if(arg->flags || arg->num_clips>4096 || (!arg->clips_ptr && arg->num_clips))return -EINVAL;
    uint32_t x1=fb->width,y1=fb->height,x2=0,y2=0;
    struct drm_clip_rect *clips=NULL;
    if(arg->num_clips) {
        uint32_t bytes=arg->num_clips*sizeof(*clips);
        clips=kernel_malloc(bytes);if(!clips)return -ENOMEM;
        int rc;
#ifdef CONFIG_SWAP
        rc=drm_user_import(task,clips,arg->clips_ptr,bytes);
#else
        rc=user_copy_from_task(task,clips,arg->clips_ptr,bytes)<0 ? -EFAULT : 0;
#endif
        if(rc){kernel_free(clips);return rc;}
        for(unsigned i=0;i<arg->num_clips;++i) {
            struct drm_clip_rect r=clips[i];
            if(r.x1>=r.x2 || r.y1>=r.y2 || r.x2>fb->width || r.y2>fb->height) {
                kernel_free(clips);return -EINVAL;
            }
            if(r.x1<x1)x1=r.x1;if(r.y1<y1)y1=r.y1;
            if(r.x2>x2)x2=r.x2;if(r.y2>y2)y2=r.y2;
        }
        kernel_free(clips);
    } else {x1=y1=0;x2=fb->width;y2=fb->height;}
    return vg_resource_flush(fb->bo,(struct virtio_gpu_rect){x1,y1,x2-x1,y2-y1});
}

/** @brief Dispatch Linux framebuffer operations supported by the VirtIO backend.
 * @param task Caller. @param file Owner. @param command Canonical ioctl.
 * @param data Copied argument. @param created_fb Rollback output.
 * @return Zero/errno/retry or -VG_IOCTL_UNOWNED for other KMS operations.
 */
int vg_fb_ioctl(struct task *task,struct drm_file *file,uint32_t command,void *data,uint32_t *created_fb)
{
    switch(command) {
    case DRM_IOCTL_MODE_ADDFB: {
        struct drm_mode_fb_cmd *arg=data;
        if(arg->bpp!=32 || (arg->depth!=24 && arg->depth!=32))return -EINVAL;
        int rc=addfb(file,arg->width,arg->height,arg->pitch,arg->depth==32 ? VG_ARGB8888 : VG_XRGB8888,
            arg->handle,&arg->fb_id);if(!rc)*created_fb=arg->fb_id;return rc;
    }
    case DRM_IOCTL_MODE_ADDFB2: {
        struct drm_mode_fb_cmd2 *arg=data;
        if(arg->flags || arg->offsets[0] || arg->modifier[0])return -EINVAL;
        for(unsigned i=1;i<4;++i)if(arg->handles[i] || arg->pitches[i] || arg->offsets[i] || arg->modifier[i])return -EINVAL;
        int rc=addfb(file,arg->width,arg->height,arg->pitches[0],arg->pixel_format,arg->handles[0],&arg->fb_id);
        if(!rc)*created_fb=arg->fb_id;return rc;
    }
    case DRM_IOCTL_MODE_RMFB: {
        uint32_t id=*(uint32_t *)data;if(!vg_fb_find(file,id))return -ENOENT;
        vg_kms_remove_fb(file,id);return vg_fb_find(file,id) ? -EIO : 0;
    }
    case DRM_IOCTL_MODE_DIRTYFB:return dirty(task,file,data);
    default:return -VG_IOCTL_UNOWNED;
    }
}
