/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/framebuffer.h>

static struct {
    struct dma_buffer pixels;
    uint32_t resource,width,height;
    uint64_t generation;
    bool active;
} console;

/** @brief Convert boot pixels into a retained, ordinary 2D VirtIO backing.
 * Caller holds execution ownership; no device access occurs during the copy.
 */
static void copy_pixels(void)
{
    const struct framebuffer *fb=framebuffer_get();
    uint32_t *pixels=console.pixels.cpu;
    for(uint32_t y=0;y<console.height;++y) {
        if(fb->bytes_per_pixel==4 && fb->red_field_position==16 &&
           fb->green_field_position==8 && !fb->blue_field_position &&
           fb->red_mask_size==8 && fb->green_mask_size==8 && fb->blue_mask_size==8)
            __builtin_memcpy(pixels+(size_t)y*console.width,
                (const uint8_t *)fb->pixels+(size_t)y*fb->pitch,console.width*4);
        else for(uint32_t x=0;x<console.width;++x)
            pixels[(size_t)y*console.width+x]=framebuffer_get_pixel_public(x,y);
    }
}

/** @brief Preallocate the text console independently of any user GEM namespace.
 * @return Zero or errno. Init context before vg.ready; cleanup follows reset.
 */
int vg_console_start(void)
{
    const struct framebuffer *fb=framebuffer_get();
    if(!fb->available || !fb->pixels || !fb->width || !fb->height ||
       fb->width>VG_MAX_WIDTH || fb->height>VG_MAX_HEIGHT)return 0;
    int rc=dma_buffer_alloc(&console.pixels,(size_t)fb->width*fb->height*4,UINT64_MAX);
    if(rc)return rc;
    console.width=fb->width;console.height=fb->height;
    console.resource=++vg.next_resource;
    struct virtio_gpu_resource_create_2d create={
        .hdr.type=VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,.resource_id=console.resource,
        .format=VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,.width=fb->width,.height=fb->height};
    rc=vg_command(&create,sizeof(create),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
    if(rc)return rc;
    struct {struct virtio_gpu_resource_attach_backing command;struct virtio_gpu_mem_entry entry;} attach={
        .command={.hdr.type=VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
            .resource_id=console.resource,.nr_entries=1},
        .entry={.addr=console.pixels.physical,.length=console.width*console.height*4}};
    return vg_command(&attach,sizeof(attach),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
}

/** @brief Release the kernel text surface after observed device reset only.
 * Task context; the module cleanup ledger preserves this storage on failure.
 */
void vg_console_stop(void)
{
    if(vg.transport.live)return;
    (void)dma_buffer_free(&console.pixels);
    console=(__typeof__(console)){0};
}

/** @brief Read the permanent console surface after fatal execution takes over.
 * @param pixels CPU pointer output. @param resource Host ID output.
 * @param width Surface width output. @param height Surface height output.
 * @return True for a completely initialized surface; no reference escapes a
 * resumable execution path. Cleanup cannot run after the fatal owner parks peers.
 */
bool vg_console_snapshot(void **pixels,uint32_t *resource,uint32_t *width,uint32_t *height)
{
    if(!console.pixels.cpu || !console.resource || !console.width || !console.height)return false;
    *pixels=console.pixels.cpu;*resource=console.resource;
    *width=console.width;*height=console.height;return true;
}

/** @brief Upload current text pixels, restoring scanout zero on a VT handoff.
 * @param restore True to bind this console; false only refreshes an active one.
 * @return Zero or hardware errno. Task context, never called from a console IRQ.
 */
int vg_console_update(bool restore)
{
    if(!console.resource || vg.fault || !vg.transport.live)return 0;
    if(vg_kms_active()){console.active=false;return 0;}
    uint64_t generation=framebuffer_generation();
    if(!restore && (!console.active || console.generation==generation))return 0;
    copy_pixels();
    struct virtio_gpu_rect rect={0,0,console.width,console.height};
    struct virtio_gpu_transfer_to_host_2d upload={.hdr.type=VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
        .r=rect,.resource_id=console.resource};
    int rc=vg_command(&upload,sizeof(upload),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
    if(!rc && restore)rc=vg_scanout(0,console.resource,rect);
    struct virtio_gpu_resource_flush flush={.hdr.type=VIRTIO_GPU_CMD_RESOURCE_FLUSH,
        .r=rect,.resource_id=console.resource};
    if(!rc)rc=vg_command(&flush,sizeof(flush),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
    if(!rc){console.active=true;console.generation=generation;}
    return rc;
}
