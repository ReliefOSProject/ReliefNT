/* SPDX-License-Identifier: Apache-2.0 */
/* Real logo drawing and framebuffer pixel conversion on guarded RAM surfaces. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/boot_logo.h>

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

/** @brief Exercise real drawing, including pitch padding and outside guards. */
static void surface_case(uint32_t width, uint32_t height, uint32_t count,
                         uint32_t expected_height, uint32_t size,
                         uint32_t columns, uint32_t gap, uint8_t bytes,
                         int reverse)
{
    uint32_t pitch = width * bytes + 16;
    size_t length = (size_t)pitch * height;
    uint8_t *allocation = malloc(length + 64);
    CHECK(allocation != NULL);
    memset(allocation, 0xa5, length + 64);
    struct framebuffer fb = {.available = true,
        .pixels = (uint32_t *)(void *)(allocation + 32),
        .width = width, .height = height, .pitch = pitch,
        .bpp = bytes * 8, .bytes_per_pixel = bytes,
        .type = MULTIBOOT2_FRAMEBUFFER_TYPE_RGB,
        .red_field_position = reverse ? 0 : 16, .red_mask_size = 8,
        .green_field_position = 8, .green_mask_size = 8,
        .blue_field_position = reverse ? 16 : 0, .blue_mask_size = 8};
    CHECK(boot_logo_draw(&fb, count) == expected_height);
    unsigned seen = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t *p = allocation + 32 + (size_t)y * pitch + x * bytes;
            if (y >= expected_height) {
                for (unsigned b = 0; b < bytes; ++b) CHECK(p[b] == 0xa5);
                continue;
            }
            int inside = 0;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t left = 16 + (i % columns) * (size + gap);
                uint32_t top = 16 + (i / columns) * (size + gap);
                if (x >= left && x < left + size && y >= top && y < top + size)
                    inside = 1;
            }
            if (!inside) {
                for (unsigned b = 0; b < bytes; ++b) CHECK(p[b] == 0);
            }
        }
        for (uint32_t b = width * bytes; b < pitch; ++b)
            CHECK(allocation[32 + (size_t)y * pitch + b] == 0xa5);
    }
    for (uint32_t i = 0; i < count; ++i) {
        unsigned blue_pixels = 0;
        uint32_t left = 16 + (i % columns) * (size + gap);
        uint32_t top = 16 + (i / columns) * (size + gap);
        for (uint32_t y = 0; y < size; ++y)
            for (uint32_t x = 0; x < size; ++x) {
                uint8_t *p = allocation + 32 + (size_t)(top+y)*pitch+(left+x)*bytes;
                uint8_t blue = p[reverse ? 2 : 0], red = p[reverse ? 0 : 2];
                if (blue > 40 && blue > red + 30) ++blue_pixels;
            }
        if (blue_pixels <= size * size / 3)
            fprintf(stderr, "surface %ux%u count=%u bytes=%u reverse=%d logo=%u blue=%u\n",
                    width,height,count,bytes,reverse,i,blue_pixels);
        CHECK(blue_pixels > size * size / 3);
        ++seen;
    }
    CHECK(seen == count);
    for (unsigned b = 0; b < 32; ++b) {
        CHECK(allocation[b] == 0xa5);
        CHECK(allocation[32 + length + b] == 0xa5);
    }
    free(allocation);
}

/** @brief Verify unusable framebuffer inputs fail without writing memory. */
static void invalid_cases(void)
{
    uint8_t pixels[4096];
    memset(pixels, 0xa5, sizeof(pixels));
    struct framebuffer fb = {.pixels=(uint32_t *)(void *)pixels,
        .width=16,.height=64,.pitch=64,.available=true,.bytes_per_pixel=4};
    CHECK(boot_logo_draw(NULL, 4) == 0);
    CHECK(boot_logo_draw(&fb, 4) == 0);
    CHECK(boot_logo_draw(&fb, 0) == 0);
    fb.width=1280; fb.height=800;
    CHECK(boot_logo_draw(&fb, 4) == 0); /* pitch shorter than a visible row */
    fb.pitch=5120; fb.available=false;
    CHECK(boot_logo_draw(&fb, 4) == 0);
    fb.available=true; fb.pixels=NULL;
    CHECK(boot_logo_draw(&fb, 4) == 0);
    for (unsigned i=0;i<sizeof(pixels);++i) CHECK(pixels[i]==0xa5);
}

/** @brief Validate spacing, wrap, CPU count, scaling and native color layouts. */
int main(void)
{
    invalid_cases();
    surface_case(1280,800,1,112,80,13,16,4,0);
    surface_case(1280,800,4,112,80,13,16,4,0);
    surface_case(640,480,8,208,80,6,16,3,0);
    surface_case(640,480,64,320,40,12,8,4,1);
    printf("PASS boot logo: %u checks; CPU counts 1/4/8/64, spacing, wrap, scale, 24/32-bit RGB/BGR and guards\n",checks);
    return 0;
}
