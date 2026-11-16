/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include <reliefnt/framebuffer.h>
#include <reliefnt/paging.h>

#define PANIC_REPLY_OFFSET 256u
static struct {
    struct dma_buffer command;
    uint8_t *pixels;
    uint32_t resource,width,height,pitch,x,y,scanout;
    uint32_t left[VG_MAX_HEIGHT],right[VG_MAX_HEIGHT];
    bool active,virgl,bind,failed;
} fatal;

/** @brief Reserve a chain and coherent command/reply storage before DRIVER_OK.
 * @return Zero or errno. Init task context, no ordinary request can race this.
 */
int vg_panic_start(void)
{
    int rc=virtio_queue_reserve_emergency(&vg.queues[0]);
    if(rc)return rc;
    return dma_buffer_alloc(&fatal.command,4096,UINT64_MAX);
}

/** @brief Consume fatal DMA only after hardware has acknowledged reset.
 * Task cleanup context, idempotent; unresettable devices retain all storage.
 */
void vg_panic_stop(void)
{
    if(vg.transport.live)return;
    (void)dma_buffer_free(&fatal.command);
    fatal=(__typeof__(fatal)){0};
}

/** @brief Snapshot the retained current scanout while normal execution is frozen.
 * Fatal owner only, all peers parked; no lock, module operation or allocation.
 * GUI uses its existing BO so pixels outside painted terminal cells survive.
 */
void vg_panic_prepare(void)
{
    fatal.active=false;
    if(!vg.ready || vg.fault || !vg.transport.live || !fatal.command.cpu)return;
    for(unsigned i=0;i<vg.scanout_count;++i) {
        const struct vg_output *out=&vg_outputs[i];
        const struct vg_kms_fb *fb=out->fb;
        if(!out->enabled || !fb)continue;
        const struct vg_resource *resource=drm_bo_virtio(fb->bo);
        uint64_t physical=drm_bo_page_physical(fb->bo,0);
        if(!resource || !physical || (fb->format!=VG_XRGB8888 && fb->format!=VG_ARGB8888) ||
           out->x>fb->width || out->y>fb->height ||
           out->mode.hdisplay>fb->width-out->x || out->mode.vdisplay>fb->height-out->y)continue;
        fatal.pixels=paging_kernel_direct_map(physical);
        if(!fatal.pixels)continue;
        fatal.resource=resource->id;fatal.width=out->mode.hdisplay;
        fatal.height=out->mode.vdisplay;fatal.pitch=fb->pitch;
        fatal.x=out->x;fatal.y=out->y;fatal.scanout=i;fatal.virgl=resource->virgl;
        fatal.bind=false;fatal.active=true;break;
    }
    if(!fatal.active) {
        void *pixels;
        if(!vg_console_snapshot(&pixels,&fatal.resource,&fatal.width,&fatal.height))return;
        fatal.pixels=pixels;fatal.pitch=fatal.width*4;fatal.x=fatal.y=fatal.scanout=0;
        fatal.bind=true;fatal.virgl=false;fatal.active=true;
    }
    for(uint32_t row=0;row<fatal.height;++row){fatal.left[row]=UINT32_MAX;fatal.right[row]=0;}
}

/** @brief Copy an exact painted rectangle and retain a bounded row damage span.
 * @param x Boot-console left. @param y Top. @param width Width. @param height Height.
 * Fatal owner only; CPU backing stays retained forever after peers are stopped.
 */
void vg_panic_damage(uint32_t x,uint32_t y,uint32_t width,uint32_t height)
{
    const struct framebuffer *source=framebuffer_get();
    if(!fatal.active || fatal.failed || x>=fatal.width || y>=fatal.height ||
       x>=source->width || y>=source->height)return;
    if(width>fatal.width-x)width=fatal.width-x;
    if(width>source->width-x)width=source->width-x;
    if(height>fatal.height-y)height=fatal.height-y;
    if(height>source->height-y)height=source->height-y;
    for(uint32_t row=y;row<y+height;++row) {
        uint32_t *destination=(void *)(fatal.pixels+(uint64_t)(row+fatal.y)*fatal.pitch);
        for(uint32_t column=x;column<x+width;++column)
            destination[column+fatal.x]=0xff000000u|framebuffer_get_pixel_public(column,row);
        if(x<fatal.left[row])fatal.left[row]=x;
        if(x+width>fatal.right[row])fatal.right[row]=x+width;
    }
}

/** @brief Exchange one fatal packet without normal queue or request locks.
 * @param command Small kernel packet. @param bytes Exact packet bytes.
 * @return True only for a matching completed fence and successful response.
 * An unresponsive device permanently disables further fatal exchanges.
 */
static bool exchange(const void *command,uint32_t bytes)
{
    if(fatal.failed || bytes>PANIC_REPLY_OFFSET || vg.next_fence==UINT64_MAX)return false;
    __builtin_memcpy(fatal.command.cpu,command,bytes);
    struct virtio_gpu_ctrl_hdr *header=fatal.command.cpu;
    struct virtio_gpu_ctrl_hdr *reply=(void *)((uint8_t *)fatal.command.cpu+PANIC_REPLY_OFFSET);
    __builtin_memset(reply,0,sizeof(*reply));
    header->flags=VIRTIO_GPU_FLAG_FENCE;header->fence_id=++vg.next_fence;
    uint32_t length=0;
    int rc=virtio_queue_emergency(&vg.queues[0],
        (struct virtio_segment){fatal.command.physical,bytes,false},
        (struct virtio_segment){fatal.command.physical+PANIC_REPLY_OFFSET,sizeof(*reply),true},&length);
    if(rc || length<sizeof(*reply) || reply->type!=VIRTIO_GPU_RESP_OK_NODATA ||
       !(reply->flags&VIRTIO_GPU_FLAG_FENCE) || reply->fence_id!=header->fence_id) {
        fatal.failed=true;return false;
    }
    return true;
}

/** @brief Upload painted row spans, preserving every other host-rendered pixel.
 * Fatal owner only; all DMA was allocated before panic, polling is bounded, and
 * no allocation, normal request callback, lock or user mapping is accessed.
 */
void vg_panic_present(void)
{
    if(!fatal.active || fatal.failed)return;
    if(fatal.bind) {
        struct virtio_gpu_set_scanout bind={.hdr.type=VIRTIO_GPU_CMD_SET_SCANOUT,
            .r={0,0,fatal.width,fatal.height},.scanout_id=fatal.scanout,.resource_id=fatal.resource};
        if(!exchange(&bind,sizeof(bind)))return;
        fatal.bind=false;
    }
    for(uint32_t row=0;row<fatal.height;) {
        if(fatal.left[row]>=fatal.right[row]){++row;continue;}
        uint32_t end=row+1;
        while(end<fatal.height && fatal.left[end]==fatal.left[row] && fatal.right[end]==fatal.right[row])++end;
        struct virtio_gpu_rect rect={fatal.x+fatal.left[row],fatal.y+row,
            fatal.right[row]-fatal.left[row],end-row};
        uint64_t offset=(uint64_t)rect.y*fatal.pitch+(uint64_t)rect.x*4;
        if(fatal.virgl) {
            struct virtio_gpu_transfer_host_3d transfer={.hdr.type=VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D,
                .box={rect.x,rect.y,0,rect.width,rect.height,1},.offset=offset,
                .resource_id=fatal.resource,.stride=fatal.pitch};
            if(!exchange(&transfer,sizeof(transfer)))return;
        } else {
            struct virtio_gpu_transfer_to_host_2d transfer={.hdr.type=VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
                .r=rect,.offset=offset,.resource_id=fatal.resource};
            if(!exchange(&transfer,sizeof(transfer)))return;
        }
        struct virtio_gpu_resource_flush flush={.hdr.type=VIRTIO_GPU_CMD_RESOURCE_FLUSH,
            .r=rect,.resource_id=fatal.resource};
        if(!exchange(&flush,sizeof(flush)))return;
        for(uint32_t y=row;y<end;++y){fatal.left[y]=UINT32_MAX;fatal.right[y]=0;}
        row=end;
    }
}
