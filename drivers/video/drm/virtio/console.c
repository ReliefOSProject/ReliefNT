/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/framebuffer.h>

static struct {
    struct dma_buffer pixels;
    uint32_t resource,width,height;
    uint64_t generation;
    struct framebuffer_damage pending;
    bool active, restore_pending;
} console;

/** @brief Convert only damaged boot pixels into the retained 2D backing.
 * @param rect Valid damage bounds in destination resource coordinates.
 * Caller holds execution ownership; no GPU command occurs during the copy.
 */
static void copy_pixels(struct virtio_gpu_rect rect)
{
    const struct framebuffer *fb=framebuffer_get();
    uint32_t *pixels=console.pixels.cpu;
    bool native=fb->available && fb->pixels && fb->bytes_per_pixel==4 &&
        fb->red_field_position==16 && fb->green_field_position==8 && !fb->blue_field_position &&
        fb->red_mask_size==8 && fb->green_mask_size==8 && fb->blue_mask_size==8 &&
        rect.x+rect.width<=fb->width && rect.y+rect.height<=fb->height;
    for(uint32_t y=rect.y;y<rect.y+rect.height;++y) {
        uint32_t *dst=pixels+(size_t)y*console.width+rect.x;
        if(native) {
            const uint8_t *src=(const uint8_t *)fb->pixels+(size_t)y*fb->pitch+rect.x*4u;
            /* The boot pixels may live in uncached MMIO. Read native words. */
            size_t count=rect.width;
            __asm__ volatile("cld; rep movsl" : "+D"(dst), "+S"(src), "+c"(count)
                             : : "memory", "cc");
        } else {
            for(uint32_t x=0;x<rect.width;++x)
                dst[x]=framebuffer_get_pixel_public(rect.x+x,y);
        }
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
    if(vg_kms_active()) {
        console.active=false;
        console.restore_pending=false;
        console.pending=(struct framebuffer_damage){0};
        return 0;
    }
    console.restore_pending |= restore;
    if(!console.active && !console.restore_pending)return 0;
    struct framebuffer_damage damage;
    if(framebuffer_take_damage(&console.generation,&damage)) {
        if(damage.x2>console.width)damage.x2=console.width;
        if(damage.y2>console.height)damage.y2=console.height;
        if(damage.x1<damage.x2 && damage.y1<damage.y2)
            framebuffer_damage_union(&console.pending,damage);
    }
    if(console.restore_pending)
        console.pending=(struct framebuffer_damage){0,0,console.width,console.height};
    if(console.pending.x1==console.pending.x2)return 0;
    damage=console.pending;
    struct virtio_gpu_rect rect={damage.x1,damage.y1,damage.x2-damage.x1,damage.y2-damage.y1};
    copy_pixels(rect);
    struct virtio_gpu_transfer_to_host_2d upload={.hdr.type=VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
        .r=rect,.resource_id=console.resource,.offset=((uint64_t)rect.y*console.width+rect.x)*4};
    int rc=vg_command(&upload,sizeof(upload),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
    if(!rc && console.restore_pending)rc=vg_scanout(0,console.resource,rect);
    struct virtio_gpu_resource_flush flush={.hdr.type=VIRTIO_GPU_CMD_RESOURCE_FLUSH,
        .r=rect,.resource_id=console.resource};
    if(!rc)rc=vg_command(&flush,sizeof(flush),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
    if(!rc) {
        console.active=true;
        console.restore_pending=false;
        console.pending=(struct framebuffer_damage){0};
    }
    return rc;
}
