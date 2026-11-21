/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../../drivers/video/drm/svga/dma.h"
#include "../../drivers/video/drm/svga/kms.h"
#include "../../drivers/video/drm/svga/render.h"
#include "../../drivers/video/drm/svga/submit.h"

struct svga_device svga;

static uint32_t packet[16], packet_bytes;
static unsigned submitted, retired;
static bool gmrs, fail_wait;
static uint8_t vram[16 * SVGA_PAGE_SIZE];

uint32_t svga_read(uint32_t reg)
{
    switch (reg) {
    case SVGA_REG_FB_OFFSET: return SVGA_PAGE_SIZE;
    case SVGA_REG_BYTES_PER_LINE: return 256;
    case SVGA_REG_HEIGHT: return 32;
    default: assert(0); return 0;
    }
}

bool svga_dma_supported(enum svga_region_type type)
{
    return gmrs && type == SVGA_REGION_GMR;
}

uint32_t svga_dma_max_mob_size(void) { assert(0); return 0; }

int svga_region_create(enum svga_region_type type, uint32_t bytes, struct svga_region **region)
{
    (void)type;
    (void)bytes;
    (void)region;
    /* An SO2 screen uses BAR1, so it must not request a guest DMA region. */
    return -ENOSPC;
}

uint32_t svga_region_id(const struct svga_region *region) { assert(!region); return SVGA_GMR_NULL; }
uint32_t svga_region_release(struct svga_region *region) { assert(!region); return 0; }
void svga_object_get(struct svga_object *object) { assert(!object); }
void svga_object_put(struct svga_object *object) { assert(!object); }
int svga_region_bind(enum svga_region_type type, struct drm_bo *bo, struct svga_region **region)
{
    assert(type == SVGA_REGION_GMR && bo && region);
    return -ENOSPC;
}
void *svga_region_pixels(const struct svga_region *region) { (void)region; abort(); }
struct drm_bo *svga_region_bo(const struct svga_region *region) { (void)region; abort(); }
int svga_buffer_wait(struct drm_bo *bo) { assert(bo); return 0; }
int svga_surface_cpu_access(struct drm_bo *bo) { assert(bo); return 0; }
void *kernel_malloc(size_t bytes) { return malloc(bytes); }
void kernel_free(void *p) { free(p); }
int svga_display_submit(const void *commands, uint32_t bytes, struct svga_surface *source,
                        struct svga_surface *target, struct drm_bo *bo, uint32_t *sequence)
{
    assert(!source && !target && bo);
    return svga_submit_fenced(commands, bytes, sequence);
}

void svga_copy_to_vram(uint64_t offset, const uint8_t *pixels, size_t bytes)
{
    assert(offset + bytes <= sizeof(vram));
    memcpy(vram + offset, pixels, bytes);
}

void svga_copy_from_vram(uint8_t *pixels, uint64_t offset, size_t bytes)
{
    assert(retired == submitted && offset + bytes <= sizeof(vram));
    memcpy(pixels, vram + offset, bytes);
}

int svga_submit_commands(const void *commands, uint32_t bytes, uint32_t context,
                         uint32_t *sequence)
{
    assert(context == SVGA3D_INVALID_ID && bytes <= sizeof(packet));
    assert(!svga.fifo.fault);
    memcpy(packet, commands, bytes);
    packet_bytes = bytes;
    ++submitted;
    if (sequence)
        *sequence = submitted;
    return 0;
}

int svga_submit_wait(uint32_t sequence)
{
    assert(sequence == submitted);
    if (fail_wait)
        return -ETIMEDOUT;
    if (packet[0] == SVGA_CMD_DEFINE_GMRFB) {
        assert(packet[1] == SVGA_GMR_FRAMEBUFFER && packet[3] == 8);
        assert(packet[2] >= 3 * SVGA_PAGE_SIZE && packet[2] <= sizeof(vram) - 16);
        uint8_t *staging = vram + packet[2];
        if (packet[5] == SVGA_CMD_BLIT_GMRFB_TO_SCREEN) {
            assert(packet[6] == 0 && packet[7] == 0 && packet[8] == 1 && packet[9] == 2);
            assert(packet[10] == 3 && packet[11] == 4 && packet[12] == 0);
            assert(staging[0] == 0x12 && staging[8] == 0x34);
        } else {
            assert(packet[5] == SVGA_CMD_BLIT_SCREEN_TO_GMRFB);
            assert(packet[6] == 0 && packet[7] == 0 && packet[8] == 1 && packet[9] == 2);
            assert(packet[10] == 3 && packet[11] == 4 && packet[12] == 0);
            memset(staging, 0x5a, 16);
        }
    }
    retired = sequence;
    return 0;
}

static uint32_t screen_definition(uint32_t size, unsigned unit, unsigned width)
{
    assert(packet_bytes == size + sizeof(uint32_t));
    assert(packet[0] == SVGA_CMD_DEFINE_SCREEN && packet[1] == size);
    assert(packet[2] == unit);
    assert(packet[3] == (SVGA_SCREEN_HAS_ROOT | (unit ? 0 : SVGA_SCREEN_IS_PRIMARY)));
    assert(packet[4] == width && packet[5] == 16);
    if (size == sizeof(SVGAScreenObject)) {
        assert(packet[8] == SVGA_GMR_FRAMEBUFFER);
        assert(packet[9] % SVGA_PAGE_SIZE == 0 && packet[9] >= 3 * SVGA_PAGE_SIZE);
        assert(packet[10] == width * 4 && packet[11] == 0);
        return packet[9];
    }
    return 0;
}

int main(void)
{
    struct svga_fb fb = {0};
    struct svga_scanout scanout = {.fb = &fb, .mode = {.hdisplay = 32, .vdisplay = 16}};
    svga.vram_bytes = 16 * SVGA_PAGE_SIZE;
    svga.max_width = svga.max_height = 256;

    svga.fifo.caps = SVGA_FIFO_CAP_SCREEN_OBJECT;
    svga_screen_init(0);
    assert(svga_screen_type() == SVGA_DISPLAY_OBJECT);
    assert(svga_screen_configure(0, &scanout) == 0);
    screen_definition(offsetof(SVGAScreenObject, backingStore), 0, 32);
    assert(svga_screen_configure(0, &scanout) == 0 && submitted == 1);
    uint8_t pixels[128] = {0};
    fb = (struct svga_fb){.bo = (struct drm_bo *)pixels, .pixels = pixels, .pitch = 24, .offset = 4};
    scanout.x = scanout.y = 1;
    pixels[84] = 0x12;
    pixels[108] = 0x34;
    uint32_t sequence;
    struct framebuffer_damage damage = {1, 2, 3, 4};
    assert(svga_screen_present(0, &fb, &scanout, damage, &sequence) == 0);
    assert(sequence == retired);
    const struct svga_blit readback = {.sx = 1, .sy = 2, .dx = 2, .dy = 3, .width = 2, .height = 2};
    assert(svga_screen_readback(0, &fb, &readback, 1, &sequence) == 0);
    assert(sequence == retired && pixels[84] == 0x5a && pixels[108] == 0x5a);
    assert(pixels[83] == 0 && pixels[92] == 0 && pixels[107] == 0 && pixels[116] == 0);
    svga_screen_release(0);
    assert(submitted == 4 && packet_bytes == 8 && packet[0] == SVGA_CMD_DESTROY_SCREEN);

    /* Either namespace can advertise SO2. Both heads occupy page-aligned,
     * disjoint slices even though an image is smaller than one page. */
    svga.fifo.caps = SVGA_FIFO_CAP_SCREEN_OBJECT_2;
    svga_screen_init(0);
    assert(svga_screen_type() == SVGA_DISPLAY_OBJECT);
    assert(svga_screen_configure(0, &scanout) == 0);
    uint32_t first = screen_definition(sizeof(SVGAScreenObject), 0, 32);
    assert(svga_screen_configure(1, &scanout) == 0);
    uint32_t second = screen_definition(sizeof(SVGAScreenObject), 1, 32);
    assert(first != second);
    scanout.mode.hdisplay = 64;
    unsigned resize = submitted;
    assert(svga_screen_configure(0, &scanout) == 0);
    assert(retired == resize + 1 && submitted == resize + 2);
    assert(screen_definition(sizeof(SVGAScreenObject), 0, 64) == first);
    svga_screen_release(1);
    assert(retired == submitted);
    fail_wait = true;
    svga_screen_release(0);
    assert(svga.fifo.fault && retired + 1 == submitted);
    assert(svga_screen_configure(0, &scanout) == -EIO);
    return 0;
}
