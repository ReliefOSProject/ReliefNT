#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/framebuffer.h>
#include "../../drivers/video/drm/virtio/console.c"

enum { WIDTH = 16, HEIGHT = 8, STRIDE = WIDTH + 3 };
struct vg_device vg;
static uint32_t source[HEIGHT][STRIDE], backing[HEIGHT][WIDTH];
static struct framebuffer fb;
static struct framebuffer_damage damage;
static uint64_t generation;
static struct virtio_gpu_transfer_to_host_2d transfer;
static struct virtio_gpu_resource_flush flush;
static struct virtio_gpu_rect bound;
static unsigned transfers, flushes, binds;
static bool kms_active, fail_flush;

const struct framebuffer *framebuffer_get(void) { return &fb; }
uint32_t framebuffer_get_pixel_public(uint32_t x, uint32_t y)
{ assert(x < fb.width && y < fb.height); return source[y][x]; }
uint64_t framebuffer_generation(void) { return generation; }
bool framebuffer_take_damage(uint64_t *seen, struct framebuffer_damage *out)
{
    if (*seen == generation) return false;
    *seen = generation;
    *out = damage;
    damage = (struct framebuffer_damage){0};
    return out->x1 < out->x2;
}
bool vg_kms_active(void) { return kms_active; }
int vg_scanout(uint32_t scanout, uint32_t resource, struct virtio_gpu_rect rect)
{
    assert(scanout == 0 && resource == console.resource);
    bound = rect;
    ++binds;
    return 0;
}
int vg_command(const void *command, uint32_t bytes, void *response,
               uint32_t response_bytes, uint32_t expected)
{
    assert(!response && !response_bytes && expected == VIRTIO_GPU_RESP_OK_NODATA);
    uint32_t type = ((const struct virtio_gpu_ctrl_hdr *)command)->type;
    if (type == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D) {
        assert(bytes == sizeof(transfer));
        memcpy(&transfer, command, bytes);
        assert(transfer.resource_id == console.resource);
        ++transfers;
        return 0;
    }
    assert(type == VIRTIO_GPU_CMD_RESOURCE_FLUSH && bytes == sizeof(flush));
    memcpy(&flush, command, bytes);
    assert(flush.resource_id == console.resource);
    ++flushes;
    return fail_flush ? -EIO : 0;
}

static void publish(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
    damage = (struct framebuffer_damage){x1, y1, x2, y2};
    ++generation;
}

static void expect_rect(struct virtio_gpu_rect rect, uint32_t x, uint32_t y,
                        uint32_t width, uint32_t height)
{
    if (rect.x != x || rect.y != y || rect.width != width || rect.height != height)
        fprintf(stderr, "upload=(%u,%u %ux%u), expected (%u,%u %ux%u)\n",
                rect.x, rect.y, rect.width, rect.height, x, y, width, height);
    assert(rect.x == x && rect.y == y && rect.width == width && rect.height == height);
}

int main(void)
{
    fb = (struct framebuffer){.pixels = &source[0][0], .width = WIDTH, .height = HEIGHT,
        .pitch = sizeof(source[0]), .bytes_per_pixel = 4, .available = true,
        .red_field_position = 16, .green_field_position = 8,
        .red_mask_size = 8, .green_mask_size = 8, .blue_mask_size = 8};
    console.pixels.cpu = backing;
    console.resource = 9;
    console.width = WIDTH;
    console.height = HEIGHT;
    vg.transport.live = true;
    assert(!vg_console_update(true));
    expect_rect(bound, 0, 0, WIDTH, HEIGHT);
    expect_rect(transfer.r, 0, 0, WIDTH, HEIGHT);

    source[3][4] = 0x111111;
    source[4][5] = 0x222222;
    source[3][6] = 0xffffff; /* Outside the uploaded rectangle. */
    publish(4, 3, 6, 5);
    assert(!vg_console_update(false));
    expect_rect(transfer.r, 4, 3, 2, 2);
    expect_rect(flush.r, 4, 3, 2, 2);
    assert(transfer.offset == 4 * (3 * WIDTH + 4));
    assert(backing[3][4] == 0x111111 && backing[4][5] == 0x222222 && !backing[3][6]);
    assert(!vg_console_update(false) && transfers == 2 && flushes == 2 && binds == 1);

    publish(2, 1, 4, 2);
    fail_flush = true;
    assert(vg_console_update(false) == -EIO);
    publish(10, 5, 11, 6);
    fail_flush = false;
    assert(!vg_console_update(false));
    expect_rect(transfer.r, 2, 1, 9, 5);
    assert(!vg_console_update(false) && transfers == 4 && flushes == 4);

    kms_active = true;
    publish(1, 1, 2, 2);
    assert(!vg_console_update(false) && transfers == 4);
    kms_active = false;
    fail_flush = true;
    assert(vg_console_update(true) == -EIO);
    fail_flush = false;
    assert(!vg_console_update(false) && binds == 3);
    expect_rect(transfer.r, 0, 0, WIDTH, HEIGHT);
    assert(backing[3][6] == 0xffffff);
    assert(!vg_console_update(false) && transfers == 6 && flushes == 6);
    puts("VirtIO text scanout: exact transfer/offset, retained damage and full VT restore passed");
}
