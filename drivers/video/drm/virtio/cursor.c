/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"

/** @brief Submit a cursor-only packet on its independent fast virtqueue.
 * @param command Kernel cursor packet. @return Zero or errno; fenced population
 * must precede UPDATE_CURSOR, as required by VirtIO 1.3 section 5.7.6.5.
 */
static int cursor_send(struct virtio_gpu_update_cursor *command)
{
    struct vg_request *request=vg_request_create(command,sizeof(*command),0,0,true);
    if(!request)return -ENOMEM;
    int rc=vg_request_submit(request);if(!rc)rc=vg_request_wait(request);
    vg_request_free(request);return rc;
}

/** @brief Remove a hardware cursor before its last backing reference is dropped.
 * @param output Scanout index. @return Zero or fenced queue errno; task context.
 */
int vg_cursor_disable(unsigned output)
{
    struct virtio_gpu_update_cursor packet={.hdr.type=VIRTIO_GPU_CMD_UPDATE_CURSOR,
        .pos.scanout_id=output};
    return cursor_send(&packet);
}

/** @brief Validate legacy cursor geometry, populate backing, then update/move it.
 * @param file Authorized OFD. @param command Canonical ioctl selector.
 * @param data Copied cursor/cursor2 argument. @return Zero or errno.
 */
int vg_cursor_ioctl(struct drm_file *file,uint32_t command,void *data)
{
    struct drm_mode_cursor2 arg={0};
    __builtin_memcpy(&arg,data,command==DRM_IOCTL_MODE_CURSOR ? sizeof(struct drm_mode_cursor) : sizeof(arg));
    unsigned i=arg.crtc_id-VG_CRTC;
    if(i>=vg.scanout_count)return -ENOENT;
    if(!arg.flags || arg.flags&~(DRM_MODE_CURSOR_BO|DRM_MODE_CURSOR_MOVE))return -EINVAL;
    struct vg_output *out=&vg_outputs[i];
    struct drm_bo *bo=NULL;
    if(arg.flags&DRM_MODE_CURSOR_BO) {
        if(arg.handle) {
            if(arg.width!=64 || arg.height!=64 || arg.hot_x<0 || arg.hot_y<0 || arg.hot_x>=64 || arg.hot_y>=64)return -EINVAL;
            bo=drm_bo_lookup_handle_get(file,arg.handle);struct vg_resource *r=drm_bo_virtio(bo);
            if(!r || r->width!=64 || r->height!=64){drm_bo_put(bo);return -EINVAL;}
            int rc=vg_resource_upload(bo,(struct virtio_gpu_rect){0,0,64,64});
            if(rc){drm_bo_put(bo);return rc;}
        }
        struct virtio_gpu_update_cursor packet={.hdr.type=VIRTIO_GPU_CMD_UPDATE_CURSOR,
            .pos={.scanout_id=i,.x=(arg.flags&DRM_MODE_CURSOR_MOVE) ? arg.x : out->cursor_x,
                .y=(arg.flags&DRM_MODE_CURSOR_MOVE) ? arg.y : out->cursor_y},
            .resource_id=bo ? drm_bo_virtio(bo)->id : 0,.hot_x=arg.hot_x,.hot_y=arg.hot_y};
        int rc=cursor_send(&packet);if(rc){drm_bo_put(bo);return rc;}
        struct drm_bo *old=out->cursor;out->cursor=bo;drm_bo_put(old);
        out->hot_x=arg.hot_x;out->hot_y=arg.hot_y;
    } else {
        struct virtio_gpu_update_cursor packet={.hdr.type=VIRTIO_GPU_CMD_MOVE_CURSOR,
            .pos={.scanout_id=i,.x=arg.x,.y=arg.y}};
        int rc=cursor_send(&packet);if(rc)return rc;
    }
    if(arg.flags&DRM_MODE_CURSOR_MOVE){out->cursor_x=arg.x;out->cursor_y=arg.y;}
    return 0;
}
