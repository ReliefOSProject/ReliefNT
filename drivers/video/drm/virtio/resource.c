/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Create the host resource and attach exactly the allocated GEM extent.
 * @param bo Exclusive GEM allocation. @param p Copied dimensions/format.
 * @param virgl True for a VirGL resource. @return Zero or errno; task context.
 * Device addresses refer only to driver-owned pages, never user virtual memory.
 */
int vg_resource_create(struct drm_bo *bo,const struct drm_virtgpu_resource_create *p,bool virgl)
{
    if(!bo || !p || drm_bo_virtio(bo) || !p->width || !p->height ||
        p->target>8 || p->last_level>15 || p->array_size>2048 || p->depth>2048 ||
        p->height>VG_MAX_HEIGHT || (p->target && p->width>VG_MAX_WIDTH) ||
        p->nr_samples>64 || (p->flags&~VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP))return -EINVAL;
    if(!virgl && (p->target!=2 || p->depth>1 || p->array_size>1 || p->last_level ||
        p->nr_samples>1 ||
        (p->format!=VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM && p->format!=VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM) ||
        (uint64_t)p->width*p->height*4>drm_bo_size(bo) ||
        (p->stride && p->stride!=(uint64_t)p->width*4)))return -EINVAL;
    if(vg.next_resource==UINT32_MAX)return -ENOSPC;
    struct vg_resource *r=drm_async_alloc(sizeof(*r));if(!r)return -ENOMEM;
    *r=(struct vg_resource){.id=++vg.next_resource,.width=p->width,.height=p->height,
        .depth=p->depth ? p->depth : 1,.array_size=p->array_size ? p->array_size : 1,
        .format=p->format,.target=p->target,.levels=p->last_level+1,.virgl=virgl,
        .stride=p->stride ? p->stride : (p->target ? p->width*4 : 0)};
    int rc;
    if(virgl) {
        struct virtio_gpu_resource_create_3d command={.hdr.type=VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
            .resource_id=r->id,.target=p->target,.format=p->format,.bind=p->bind,
            .width=p->width,.height=p->height,.depth=r->depth,.array_size=r->array_size,
            .last_level=p->last_level,.nr_samples=p->nr_samples,.flags=p->flags};
        rc=vg_command(&command,sizeof(command),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
    } else {
        struct virtio_gpu_resource_create_2d command={.hdr.type=VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
            .resource_id=r->id,.format=p->format,.width=p->width,.height=p->height};
        rc=vg_command(&command,sizeof(command),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
    }
    if(rc){drm_async_free(r);return rc;}
    /* GEM currently supplies contiguous physical pages; this standard scatter
     * list has one exact extent, and remains independent from CPU addresses. */
    struct { struct virtio_gpu_resource_attach_backing command; struct virtio_gpu_mem_entry entry; } attach={
        .command={.hdr.type=VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,.resource_id=r->id,.nr_entries=1},
        .entry={.addr=drm_bo_page_physical(bo,0),.length=drm_bo_size(bo)}};
    rc=vg_command(&attach,sizeof(attach),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
    /* Adopt metadata even on a partial failure so final BO retirement cannot
     * free pages still owned by a timed-out RESOURCE_ATTACH_BACKING command. */
    drm_bo_set_virtio(bo,r);
    return rc;
}

/** @brief Detach contexts and fence host destruction before returning GEM pages.
 * @param r Final resource. @return True only when host DMA has ended; false
 * quarantines its module pin and all backing memory. Task/execution context.
 */
bool vg_resource_destroy(struct vg_resource *r)
{
    if(!r)return true;
    int rc=vg_context_detach_resource(r);
    if(!rc && vg.transport.live && !vg.fault) {
        struct virtio_gpu_resource_unref command={.hdr.type=VIRTIO_GPU_CMD_RESOURCE_UNREF,.resource_id=r->id};
        rc=vg_command(&command,sizeof(command),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
    }
    if((rc || vg.fault) && vg.transport.live && vg_device_fault(-EIO))return false;
    drm_async_free(r);return true;
}

/** @brief Make CPU-written 2D pixels visible, then flush the chosen rectangle.
 * @param bo Retained resource BO. @param rect Valid damage bounds.
 * @return Zero or errno; for VirGL the host renderer already owns pixel contents.
 */
int vg_resource_upload(struct drm_bo *bo,struct virtio_gpu_rect rect)
{
    struct vg_resource *r=drm_bo_virtio(bo);
    if(!r || rect.x>r->width || rect.y>r->height || rect.width>r->width-rect.x ||
        rect.height>r->height-rect.y || !rect.width || !rect.height)return -EINVAL;
    int rc=0;
    if(!r->virgl) {
        struct virtio_gpu_transfer_to_host_2d transfer={.hdr.type=VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
            .r=rect,.resource_id=r->id,.offset=(uint64_t)rect.y*r->width*4+(uint64_t)rect.x*4};
        rc=vg_command(&transfer,sizeof(transfer),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
    }
    return rc;
}

/** @brief Upload any CPU damage then fence its host display flush.
 * @param bo Retained BO. @param rect Valid damage rectangle.
 * @return Zero or transfer/flush errno; task context.
 */
int vg_resource_flush(struct drm_bo *bo,struct virtio_gpu_rect rect)
{
    int rc=vg_resource_upload(bo,rect);if(rc)return rc;
    struct virtio_gpu_resource_flush flush={.hdr.type=VIRTIO_GPU_CMD_RESOURCE_FLUSH,
        .r=rect,.resource_id=drm_bo_virtio(bo)->id};
    return vg_command(&flush,sizeof(flush),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
}

/** @brief Bind a resource rectangle or disable one hardware output.
 * @param scanout Output index. @param resource Host resource ID or zero.
 * @param rect Exact rectangle. @return Zero or protocol errno; task context.
 */
int vg_scanout(uint32_t scanout,uint32_t resource,struct virtio_gpu_rect rect)
{
    if(scanout>=vg.scanout_count)return -EINVAL;
    struct virtio_gpu_set_scanout command={.hdr.type=VIRTIO_GPU_CMD_SET_SCANOUT,
        .r=rect,.scanout_id=scanout,.resource_id=resource};
    return vg_command(&command,sizeof(command),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
}
