/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include <reliefnt/heap.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/framebuffer.h>

#define VG_DIRTY_SPLIT_MAX 4u
/* Each additional 2D upload and flush must avoid at least 256 KiB of pixels. */
#define VG_DIRTY_SPLIT_SAVED_PIXELS (64u * 1024u)

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

static struct virtio_gpu_rect damage_rect(struct framebuffer_damage bounds)
{
    return (struct virtio_gpu_rect){bounds.x1,bounds.y1,bounds.x2-bounds.x1,bounds.y2-bounds.y1};
}

/** @brief Snapshot/validate all damage rectangles before the first GPU operation.
 * @param task Caller. @param file Owner. @param arg Copied damage descriptor.
 * @return Zero, errno or prerequisite retry; aliases and swapped input are snapshotted.
 */
static int dirty(struct task *task,struct drm_file *file,struct drm_mode_fb_dirty_cmd *arg)
{
    struct vg_kms_fb *fb=vg_fb_find(file,arg->fb_id);if(!fb)return -ENOENT;
    unsigned count=arg->num_clips;
    if(arg->flags || count>4096 || (!arg->clips_ptr && count))return -EINVAL;
    if(!count)return vg_resource_flush(fb->bo,(struct virtio_gpu_rect){0,0,fb->width,fb->height});

    struct drm_clip_rect local[VG_DIRTY_SPLIT_MAX],*clips=local;
    uint32_t bytes=count*sizeof(*clips);
    if(count>VG_DIRTY_SPLIT_MAX) {
        clips=kernel_malloc(bytes);
        if(!clips)return -ENOMEM;
    }
    int rc;
#ifdef CONFIG_SWAP
    rc=drm_user_import(task,clips,arg->clips_ptr,bytes);
#else
    rc=user_copy_from_task(task,clips,arg->clips_ptr,bytes)<0 ? -EFAULT : 0;
#endif
    struct framebuffer_damage bounds={0};
    uint64_t pixels=0;
    bool separate=count>1 && count<=VG_DIRTY_SPLIT_MAX;
    if(!rc)for(unsigned i=0;i<count;++i) {
        struct drm_clip_rect r=clips[i];
        if(r.x1>=r.x2 || r.y1>=r.y2 || r.x2>fb->width || r.y2>fb->height) {
            rc=-EINVAL;
            break;
        }
        framebuffer_damage_union(&bounds,(struct framebuffer_damage){r.x1,r.y1,r.x2,r.y2});
        if(separate) {
            pixels+=(uint64_t)(r.x2-r.x1)*(r.y2-r.y1);
            for(unsigned j=0;j<i;++j) {
                struct drm_clip_rect a=clips[j];
                if(r.x1<a.x2 && a.x1<r.x2 && r.y1<a.y2 && a.y1<r.y2) {
                    separate=false;
                    break;
                }
            }
        }
    }
    if(clips!=local)kernel_free(clips);
    if(rc)return rc;

    struct virtio_gpu_rect combined=damage_rect(bounds);
    if(separate && (uint64_t)combined.width*combined.height >=
       pixels+(uint64_t)(count-1)*VG_DIRTY_SPLIT_SAVED_PIXELS) {
        const struct vg_resource *resource=drm_bo_virtio(fb->bo);
        if(resource && !resource->virgl) {
            for(unsigned i=0;i<count;++i) {
                struct drm_clip_rect r=local[i];
                rc=vg_resource_flush(fb->bo,damage_rect((struct framebuffer_damage){r.x1,r.y1,r.x2,r.y2}));
                if(rc)return rc;
            }
            return 0;
        }
    }
    return vg_resource_flush(fb->bo,combined);
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
